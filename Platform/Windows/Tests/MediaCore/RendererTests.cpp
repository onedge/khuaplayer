// The D3D11 renderer drawn into an offscreen target: colour math against a
// CPU reference of Video.metal, format equivalence, geometry, EDR output and
// the Mac renderer's submission state machine.
#include "Player/SPAVFrameRef.hpp"
#include "SPD3D11Renderer.hpp"
#include "SPDoviReshapeQueue.hpp"
#include "SPOffscreenRenderTarget.hpp"
#include "SPRenderPolicy.hpp"

#include <gtest/gtest.h>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

using namespace std::chrono;
using Microsoft::WRL::ComPtr;

namespace {

using RGB = std::array<float, 3>;

struct Yuv {
    int y, cb, cr; // 8-bit code values
};

// ---- CPU reference of the shader's SDR path (BT.709, video range) -------

float eotfCoreMedia(float c) { return std::max(std::pow(c, 1.9609375f), c / 16.0f); }
float oetfSRGB(float c) { return c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }

RGB reference709(Yuv v, bool fullRange = false) {
    float Y = v.y / 255.0f, Cb = v.cb / 255.0f, Cr = v.cr / 255.0f;
    if (!fullRange) {
        Y = (Y - 16.0f / 255.0f) / (219.0f / 255.0f);
        Cb = (Cb - 16.0f / 255.0f) / (224.0f / 255.0f);
        Cr = (Cr - 16.0f / 255.0f) / (224.0f / 255.0f);
    }
    Y = std::clamp(Y, 0.0f, 1.0f);
    Cb = std::clamp(Cb, 0.0f, 1.0f) - (fullRange ? 128.0f / 255.0f : 0.5f);
    Cr = std::clamp(Cr, 0.0f, 1.0f) - (fullRange ? 128.0f / 255.0f : 0.5f);
    RGB rgb = {Y + 1.5748f * Cr, Y - 0.187324f * Cb - 0.468124f * Cr, Y + 1.8556f * Cb};
    // What macOS does with the BT.709-tagged layer on an sRGB display.
    for (float &c : rgb) c = oetfSRGB(std::clamp(eotfCoreMedia(std::clamp(c, 0.0f, 1.0f)), 0.0f, 1.0f));
    return rgb;
}

// ---- Frames ---------------------------------------------------------------

struct FrameDeleter {
    void operator()(AVFrame *f) const { av_frame_free(&f); }
};
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;

// A flat frame of `v` in `format`, `w`x`h`.
FramePtr makeFlatFrame(AVPixelFormat format, int w, int h, Yuv v) {
    FramePtr f(av_frame_alloc());
    f->format = format;
    f->width = w;
    f->height = h;
    f->color_range = AVCOL_RANGE_MPEG;
    if (av_frame_get_buffer(f.get(), 0) < 0) return nullptr;
    const int cw = (w + 1) / 2, ch = (h + 1) / 2;
    auto fill8 = [](uint8_t *p, int linesize, int width, int height, int value) {
        for (int y = 0; y < height; y++) std::memset(p + (size_t)y * linesize, value, (size_t)width);
    };
    auto fill16 = [](uint8_t *p, int linesize, int width, int height, uint16_t a, int step, uint16_t b) {
        for (int y = 0; y < height; y++) {
            auto *row = reinterpret_cast<uint16_t *>(p + (size_t)y * linesize);
            for (int x = 0; x < width * step; x += step) {
                row[x] = a;
                if (step == 2) row[x + 1] = b;
            }
        }
    };
    switch (format) {
    case AV_PIX_FMT_NV12:
        fill8(f->data[0], f->linesize[0], w, h, v.y);
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++) {
                f->data[1][(size_t)y * f->linesize[1] + x * 2] = (uint8_t)v.cb;
                f->data[1][(size_t)y * f->linesize[1] + x * 2 + 1] = (uint8_t)v.cr;
            }
        break;
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
        fill8(f->data[0], f->linesize[0], w, h, v.y);
        fill8(f->data[1], f->linesize[1], cw, ch, v.cb);
        fill8(f->data[2], f->linesize[2], cw, ch, v.cr);
        break;
    case AV_PIX_FMT_P010LE: // 10-bit MSB-justified
        fill16(f->data[0], f->linesize[0], w, h, (uint16_t)((v.y << 2) << 6), 1, 0);
        fill16(f->data[1], f->linesize[1], cw, ch, (uint16_t)((v.cb << 2) << 6), 2, (uint16_t)((v.cr << 2) << 6));
        break;
    case AV_PIX_FMT_YUV420P10LE: // 10-bit LSB-justified
        fill16(f->data[0], f->linesize[0], w, h, (uint16_t)(v.y << 2), 1, 0);
        fill16(f->data[1], f->linesize[1], cw, ch, (uint16_t)(v.cb << 2), 1, 0);
        fill16(f->data[2], f->linesize[2], cw, ch, (uint16_t)(v.cr << 2), 1, 0);
        break;
    default:
        break;
    }
    return f;
}

// ---- Harness --------------------------------------------------------------

bool waitFor(const std::function<bool()> &condition, milliseconds timeout = milliseconds(2000)) {
    const auto deadline = steady_clock::now() + timeout;
    while (steady_clock::now() < deadline) {
        if (condition()) return true;
        std::this_thread::sleep_for(milliseconds(2));
    }
    return condition();
}

class RendererTest : public ::testing::Test {
protected:
    void SetUp() override {
        device_ = sp::D3D11Device::shared();
        if (!device_) GTEST_SKIP() << "no Direct3D 11 device";
        target_ = std::make_shared<sp::OffscreenRenderTarget>();
        renderer_ = std::make_unique<sp::D3D11Renderer>(device_, target_, 1);
        ASSERT_TRUE(renderer_->isReady());
        renderer_->setViewportPixelSize(64, 36);
        renderer_->setColorimetry(AVCOL_PRI_BT709, AVCOL_TRC_BT709, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 0);
        synchronize();
    }

    void synchronize() {
        std::atomic<bool> done{false}, ready{false};
        renderer_->synchronizeOutputMode([&](bool r) {
            ready = r;
            done = true;
        });
        ASSERT_TRUE(waitFor([&] { return done.load(); }));
        EXPECT_TRUE(ready.load());
    }

    // Renders `frame` and returns the target's pixels.
    std::vector<float> render(const AVFrame *frame) {
        sp::VideoFrameRef handle = sp::spFrameFromAVFrame(frame);
        const uint64_t before = renderer_->committedFrameCount();
        EXPECT_TRUE(renderer_->renderFrame(handle));
        sp::spFrameRelease(handle);
        EXPECT_TRUE(waitFor([&] { return renderer_->committedFrameCount() > before; }));
        return read();
    }

    std::vector<float> read() {
        std::lock_guard<std::recursive_mutex> lock(device_->mutex());
        return target_->readPixels(device_->device(), device_->context());
    }

    RGB pixel(const std::vector<float> &px, int x, int y) const {
        const size_t i = ((size_t)y * target_->width() + x) * 4;
        return {px[i], px[i + 1], px[i + 2]};
    }

    std::shared_ptr<sp::D3D11Device> device_;
    std::shared_ptr<sp::OffscreenRenderTarget> target_;
    std::unique_ptr<sp::D3D11Renderer> renderer_;
};

void expectNear(const RGB &actual, const RGB &expected, float tolerance) {
    for (int c = 0; c < 3; c++) EXPECT_NEAR(actual[c], expected[c], tolerance) << "channel " << c;
}

constexpr float kByte = 1.0f / 255.0f;

} // namespace

TEST_F(RendererTest, SoftwareNV12MatchesTheReference) {
    for (const Yuv v : {Yuv{180, 100, 160}, Yuv{16, 128, 128}, Yuv{235, 128, 128}, Yuv{81, 90, 240}}) {
        auto frame = makeFlatFrame(AV_PIX_FMT_NV12, 64, 36, v);
        const auto px = render(frame.get());
        expectNear(pixel(px, 32, 18), reference709(v), 1.5f * kByte);
    }
    EXPECT_EQ(renderer_->hardRenderFailureCount(), 0u);
}

TEST_F(RendererTest, EveryInputFormatRendersTheSame) {
    const Yuv v{150, 110, 170};
    const RGB expected = reference709(v);
    for (AVPixelFormat format : {AV_PIX_FMT_NV12, AV_PIX_FMT_YUV420P, AV_PIX_FMT_P010LE, AV_PIX_FMT_YUV420P10LE}) {
        auto frame = makeFlatFrame(format, 64, 36, v);
        const auto px = render(frame.get());
        SCOPED_TRACE(av_get_pix_fmt_name(format));
        expectNear(pixel(px, 32, 18), expected, 1.5f * kByte);
    }
}

TEST_F(RendererTest, FullRangeComesFromTheFrame) {
    const Yuv v{200, 100, 150};
    auto frame = makeFlatFrame(AV_PIX_FMT_NV12, 64, 36, v);
    frame->color_range = AVCOL_RANGE_JPEG;
    expectNear(pixel(render(frame.get()), 32, 18), reference709(v, true), 1.5f * kByte);
    auto jpeg = makeFlatFrame(AV_PIX_FMT_YUVJ420P, 64, 36, v);
    expectNear(pixel(render(jpeg.get()), 32, 18), reference709(v, true), 1.5f * kByte);
}

// A 16:9 picture in a 4:3 viewport is letterboxed; rotated by 90 degrees in a
// 16:9 viewport it is pillarboxed.
TEST_F(RendererTest, FitsTheVideoIntoTheViewport) {
    const Yuv white{235, 128, 128};
    auto frame = makeFlatFrame(AV_PIX_FMT_NV12, 64, 36, white);
    renderer_->setViewportPixelSize(64, 48);
    auto px = render(frame.get());
    ASSERT_EQ(target_->height(), 48);
    expectNear(pixel(px, 32, 1), {0, 0, 0}, kByte);  // bar
    expectNear(pixel(px, 32, 24), {1, 1, 1}, kByte); // picture
    expectNear(pixel(px, 32, 46), {0, 0, 0}, kByte); // bar

    renderer_->setViewportPixelSize(64, 36);
    renderer_->setRotation(90);
    px = render(frame.get());
    expectNear(pixel(px, 2, 18), {0, 0, 0}, kByte);
    expectNear(pixel(px, 32, 18), {1, 1, 1}, kByte);
    expectNear(pixel(px, 61, 18), {0, 0, 0}, kByte);
    renderer_->resetPictureTransform();
}

// D3D11VA output: a slice of a shader-readable NV12 texture array whose
// surfaces are taller than the picture, as 1080p decodes into 1088 lines.
TEST_F(RendererTest, SamplesOnlyThePictureOfAPaddedDecoderSlice) {
    const int w = 64, h = 36, paddedH = 48, slices = 3;
    const Yuv picture{180, 100, 160}, padding{235, 128, 128};
    std::vector<std::vector<uint8_t>> planes(slices);
    std::vector<D3D11_SUBRESOURCE_DATA> init(slices);
    for (int s = 0; s < slices; s++) {
        std::vector<uint8_t> &mem = planes[s];
        mem.assign((size_t)w * paddedH * 3 / 2, 0);
        const Yuv v = s == 1 ? picture : Yuv{16, 128, 128};
        for (int y = 0; y < paddedH; y++)
            std::memset(&mem[(size_t)y * w], y < h ? v.y : padding.y, (size_t)w);
        uint8_t *uv = &mem[(size_t)w * paddedH];
        for (int y = 0; y < paddedH / 2; y++)
            for (int x = 0; x < w / 2; x++) {
                const Yuv c = y < h / 2 ? v : padding;
                uv[(size_t)y * w + x * 2] = (uint8_t)c.cb;
                uv[(size_t)y * w + x * 2 + 1] = (uint8_t)c.cr;
            }
        init[s] = {mem.data(), (UINT)w, 0};
    }
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = paddedH;
    desc.MipLevels = 1;
    desc.ArraySize = slices;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // As a D3D11VA pool: drivers only make NV12 arrays shader-readable when
    // they are decoder targets too.
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_DECODER;
    ComPtr<ID3D11Texture2D> texture;
    {
        // A planar array cannot take initial data, so each slice is filled
        // from a single texture.
        std::lock_guard<std::recursive_mutex> lock(device_->mutex());
        const HRESULT hr = device_->device()->CreateTexture2D(&desc, nullptr, &texture);
        if (FAILED(hr)) GTEST_SKIP() << "no shader-readable NV12 texture arrays (0x" << std::hex << (uint32_t)hr << ")";
        D3D11_TEXTURE2D_DESC sliceDesc = desc;
        sliceDesc.ArraySize = 1;
        sliceDesc.BindFlags = 0;
        for (int s = 0; s < slices; s++) {
            ComPtr<ID3D11Texture2D> slice;
            ASSERT_HRESULT_SUCCEEDED(device_->device()->CreateTexture2D(&sliceDesc, &init[s], &slice));
            device_->context()->CopySubresourceRegion(texture.Get(), D3D11CalcSubresource(0, s, 1), 0, 0, 0,
                                                      slice.Get(), 0, nullptr);
        }
    }

    FramePtr frame(av_frame_alloc());
    frame->format = AV_PIX_FMT_D3D11;
    frame->width = w;
    frame->height = h;
    frame->color_range = AVCOL_RANGE_MPEG;
    frame->data[0] = reinterpret_cast<uint8_t *>(texture.Get());
    frame->data[1] = reinterpret_cast<uint8_t *>((intptr_t)1);
    static uint8_t dummy;
    frame->buf[0] = av_buffer_create(&dummy, 1, [](void *, uint8_t *) {}, nullptr, 0);
    const auto px = render(frame.get());
    expectNear(pixel(px, 32, 18), reference709(picture), 1.5f * kByte);
    // No padding bleeding in: the last row's chroma falls between the last
    // visible chroma row and the first padding row.
    expectNear(pixel(px, 32, 35), reference709(picture), 1.5f * kByte);
}

// PQ content at 100 nits on an HDR display: EDR 1.0, SDR white on screen.
TEST_F(RendererTest, HdrOutputPutsReferenceWhiteAtTheSdrWhiteLevel) {
    renderer_->updateOutputMode(10.0, 10.0); // 1000 nits of headroom
    renderer_->setSdrWhiteLevelNits(240.0f);
    renderer_->setColorimetry(AVCOL_PRI_BT2020, AVCOL_TRC_SMPTE2084, AVCOL_SPC_BT2020_NCL, AVCOL_RANGE_MPEG, 1000);
    synchronize();
    EXPECT_EQ(renderer_->outputModeDescription(), "HDR 10.0x");
    // PQ(100 nits) = 0.5081, as 10-bit video range: 64 + 876 * 0.5081 = 509.
    FramePtr frame = makeFlatFrame(AV_PIX_FMT_P010LE, 64, 36, {0, 0, 0});
    for (int y = 0; y < 36; y++) {
        auto *row = reinterpret_cast<uint16_t *>(frame->data[0] + (size_t)y * frame->linesize[0]);
        for (int x = 0; x < 64; x++) row[x] = (uint16_t)(509 << 6);
    }
    for (int y = 0; y < 18; y++) {
        auto *row = reinterpret_cast<uint16_t *>(frame->data[1] + (size_t)y * frame->linesize[1]);
        for (int x = 0; x < 64; x++) row[x] = (uint16_t)(512 << 6);
    }
    const auto px = render(frame.get());
    EXPECT_EQ(target_->mode(), sp::kRenderOutputEDR);
    const RGB c = pixel(px, 32, 18);
    for (int i = 0; i < 3; i++) EXPECT_NEAR(c[i], 240.0f / 80.0f, 0.1f) << i;

    // Back on an SDR display the same frame is tone-mapped into BGRA8.
    renderer_->updateOutputMode(1.0, 1.0);
    synchronize();
    const auto sdr = render(frame.get());
    EXPECT_EQ(target_->mode(), sp::kRenderOutputSDR);
    const RGB s = pixel(sdr, 32, 18);
    EXPECT_GT(s[0], 0.3f);
    EXPECT_LT(s[0], 1.0f);
}

TEST_F(RendererTest, BlendsThePremultipliedSubtitleOverlay) {
    // Half-transparent white, premultiplied: (0.5, 0.5, 0.5, 0.5).
    std::vector<uint32_t> texels(16 * 8, 0x80808080u);
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 16;
    desc.Height = 8;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA init = {texels.data(), 16 * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    {
        std::lock_guard<std::recursive_mutex> lock(device_->mutex());
        ASSERT_HRESULT_SUCCEEDED(device_->device()->CreateTexture2D(&desc, &init, &texture));
        ASSERT_HRESULT_SUCCEEDED(device_->device()->CreateShaderResourceView(texture.Get(), nullptr, &view));
    }
    renderer_->setSubtitleTexture(view.Get());
    renderer_->setSubtitleRect({8, 20, 16, 8});
    auto frame = makeFlatFrame(AV_PIX_FMT_NV12, 64, 36, {16, 128, 128}); // black
    const auto px = render(frame.get());
    // Like the Mac, white is blended at alpha 0.5 in the layer's encoding (the
    // source's BT.709 curve), which is then shown on the sRGB display.
    const float half = oetfSRGB(eotfCoreMedia(128.0f / 255.0f));
    expectNear(pixel(px, 16, 24), {half, half, half}, 2.0f * kByte);
    expectNear(pixel(px, 40, 24), {0, 0, 0}, kByte); // outside the rectangle
    renderer_->setSubtitleTexture(nullptr);
}

// ---- Submission -------------------------------------------------------------

TEST_F(RendererTest, SpeculativeFrameIsAcceptedOnlyFirst) {
    auto frame = makeFlatFrame(AV_PIX_FMT_NV12, 64, 36, {100, 128, 128});
    sp::VideoFrameRef handle = sp::spFrameFromAVFrame(frame.get());
    EXPECT_TRUE(renderer_->renderSpeculativeFirstFrame(handle));
    EXPECT_TRUE(waitFor([&] { return target_->presentCount() >= 1; }));
    EXPECT_EQ(renderer_->committedFrameCount(), 0u); // speculative frames do not count
    EXPECT_TRUE(renderer_->renderFrame(handle));
    EXPECT_FALSE(renderer_->renderSpeculativeFirstFrame(handle));
    renderer_->discardPendingSubmits(); // a new session
    EXPECT_TRUE(renderer_->renderSpeculativeFirstFrame(handle));
    sp::spFrameRelease(handle);
}

TEST_F(RendererTest, UnavailableTargetIsRetriedUntilItPresents) {
    target_->setAvailable(false); // an occluded window
    auto frame = makeFlatFrame(AV_PIX_FMT_NV12, 64, 36, {100, 128, 128});
    sp::VideoFrameRef handle = sp::spFrameFromAVFrame(frame.get());
    EXPECT_TRUE(renderer_->renderFrame(handle));
    sp::spFrameRelease(handle);
    std::this_thread::sleep_for(milliseconds(250));
    EXPECT_EQ(renderer_->committedFrameCount(), 0u);
    EXPECT_EQ(renderer_->hardRenderFailureCount(), 0u); // not a render failure
    target_->setAvailable(true);
    EXPECT_TRUE(waitFor([&] { return renderer_->committedFrameCount() == 1; }, milliseconds(500)));
}

TEST_F(RendererTest, SuspendedSubmitsKeepOnlyTheLatestFrameAndDiscardDropsIt) {
    renderer_->setSubmitsSuspended(true);
    auto frame = makeFlatFrame(AV_PIX_FMT_NV12, 64, 36, {100, 128, 128});
    sp::VideoFrameRef handle = sp::spFrameFromAVFrame(frame.get());
    EXPECT_TRUE(renderer_->renderFrame(handle));
    EXPECT_TRUE(renderer_->renderFrame(handle));
    std::this_thread::sleep_for(milliseconds(50));
    EXPECT_EQ(renderer_->committedFrameCount(), 0u);
    renderer_->setSubmitsSuspended(false);
    renderer_->kickSubmitDrain();
    EXPECT_TRUE(waitFor([&] { return renderer_->committedFrameCount() == 1; }));
    std::this_thread::sleep_for(milliseconds(50));
    EXPECT_EQ(renderer_->committedFrameCount(), 1u); // latest wins: one commit

    renderer_->setSubmitsSuspended(true);
    EXPECT_TRUE(renderer_->renderFrame(handle));
    renderer_->discardPendingSubmits();
    renderer_->setSubmitsSuspended(false);
    renderer_->kickSubmitDrain();
    std::this_thread::sleep_for(milliseconds(100));
    EXPECT_EQ(renderer_->committedFrameCount(), 1u);
    sp::spFrameRelease(handle);
}

TEST_F(RendererTest, UnsupportedFormatIsAHardFailure) {
    FramePtr frame(av_frame_alloc());
    frame->format = AV_PIX_FMT_RGB24;
    frame->width = 16;
    frame->height = 16;
    ASSERT_GE(av_frame_get_buffer(frame.get(), 0), 0);
    sp::VideoFrameRef handle = sp::spFrameFromAVFrame(frame.get());
    EXPECT_TRUE(renderer_->renderFrame(handle));
    sp::spFrameRelease(handle);
    EXPECT_TRUE(waitFor([&] { return renderer_->hardRenderFailureCount() >= 1; }));
    EXPECT_EQ(renderer_->committedFrameCount(), 0u);
}

TEST_F(RendererTest, ClearToBlackPresentsWithoutCountingAFrame) {
    auto frame = makeFlatFrame(AV_PIX_FMT_NV12, 64, 36, {235, 128, 128});
    render(frame.get());
    const int presents = target_->presentCount();
    renderer_->clearToBlack();
    EXPECT_TRUE(waitFor([&] { return target_->presentCount() > presents; }));
    expectNear(pixel(read(), 32, 18), {0, 0, 0}, kByte);
    EXPECT_EQ(renderer_->committedFrameCount(), 1u);
}

// The renderer keeps the frame on screen alive, which for D3D11VA holds the
// decoder's surface pool; a new session or a black screen lets it go.
TEST_F(RendererTest, DiscardAndClearReleaseTheFrameOnScreen) {
    auto frame = makeFlatFrame(AV_PIX_FMT_NV12, 64, 36, {235, 128, 128});
    render(frame.get());
    EXPECT_GT(av_buffer_get_ref_count(frame->buf[0]), 1);
    renderer_->discardPendingSubmits();
    EXPECT_TRUE(waitFor([&] { return av_buffer_get_ref_count(frame->buf[0]) == 1; }));

    render(frame.get());
    EXPECT_GT(av_buffer_get_ref_count(frame->buf[0]), 1);
    renderer_->clearToBlack();
    EXPECT_TRUE(waitFor([&] { return av_buffer_get_ref_count(frame->buf[0]) == 1; }));
}

// A synchronize still queued when the renderer goes away is answered, once.
TEST_F(RendererTest, DestroyingTheRendererAnswersAQueuedSynchronize) {
    std::atomic<int> calls{0};
    // HDR content on an HDR display: a mode change, so not answered inline.
    renderer_->updateOutputMode(10.0, 10.0);
    renderer_->setColorimetry(AVCOL_PRI_BT2020, AVCOL_TRC_SMPTE2084, AVCOL_SPC_BT2020_NCL, AVCOL_RANGE_MPEG, 1000);
    renderer_->synchronizeOutputMode([&](bool) { calls++; });
    renderer_.reset();
    EXPECT_EQ(calls.load(), 1);
}

// ---- Dolby Vision ring --------------------------------------------------------

namespace {
std::vector<float> doviValues(float tag) {
    std::vector<float> v(sp::kSPDoviFloatCount, 0.0f);
    v[0] = tag;
    return v;
}
} // namespace

TEST(DoviReshapeQueue, BindsTheNewestEntryWithinAQuarterFrame) {
    sp::DoviReshapeQueue q;
    const float defaults = q.current()[0];
    q.queue(doviValues(1).data(), 1000000);
    q.queue(doviValues(2).data(), 1040000);
    q.queue(doviValues(3).data(), 1080000);
    // Frame at 1.030 s, 40 ms apart: the limit is 1.040 s.
    EXPECT_EQ(q.bind(1030000, 40000), 1040000);
    EXPECT_EQ(q.current()[0], 2);
    // Older entries were consumed: an earlier frame finds nothing and the
    // previous RPU carries on.
    EXPECT_EQ(q.bind(1000000, 40000), INT64_MIN);
    EXPECT_EQ(q.current()[0], 2);
    EXPECT_EQ(q.bind(1080000, 40000), 1080000);
    EXPECT_EQ(q.current()[0], 3);
    // A seek forgets the queue but keeps the values; a new session resets them.
    q.queue(doviValues(4).data(), 2000000);
    q.clear();
    EXPECT_EQ(q.bind(2000000, 40000), INT64_MIN);
    EXPECT_EQ(q.current()[0], 3);
    q.reset();
    EXPECT_EQ(q.current()[0], defaults);
    // No timestamp: applied at once.
    q.queue(doviValues(5).data(), -1);
    EXPECT_EQ(q.current()[0], 5);
}

TEST(DoviReshapeQueue, KeepsTheLatest32Entries) {
    sp::DoviReshapeQueue q;
    for (int i = 0; i < 40; i++) q.queue(doviValues((float)i).data(), 1000 * (int64_t)i);
    EXPECT_EQ(q.bind(7000, 0), INT64_MIN); // overwritten
    EXPECT_EQ(q.bind(39000, 0), 39000);
}
