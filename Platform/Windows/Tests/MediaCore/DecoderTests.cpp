// FFmpegVideoDecoder on the clips in Media/ (make_test_media.py): software
// decoding of every clip, D3D11VA output checked bit for bit against
// software, the hardware support boundary, colour, flush, catch-up, drain and
// the hardware surface pool under a player that holds frames.
#include "Player/SPAVFrameRef.hpp"
#include "SPD3D11Device.hpp"
#include "SPD3D11Renderer.hpp"
#include "SPFFmpegVideoDecoder.hpp"
#include "SPOffscreenRenderTarget.hpp"

#include <gtest/gtest.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace std::chrono;

namespace {

// ---- Clips ------------------------------------------------------------------

struct PacketDeleter {
    void operator()(AVPacket *p) const { av_packet_free(&p); }
};
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;

class Clip {
public:
    explicit Clip(const std::string &name) {
        const std::string path = std::string(KHUA_TEST_MEDIA_DIR) + "/" + name;
        if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0) return;
        if (avformat_find_stream_info(fmt_, nullptr) < 0) return;
        stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    }
    ~Clip() { avformat_close_input(&fmt_); }

    bool ok() const { return fmt_ && stream_ >= 0; }
    const AVCodecParameters *par() const { return fmt_->streams[stream_]->codecpar; }
    AVRational timeBase() const { return fmt_->streams[stream_]->time_base; }

    // The next packet of the video stream, or null at the end.
    PacketPtr next() {
        PacketPtr pkt(av_packet_alloc());
        while (av_read_frame(fmt_, pkt.get()) >= 0) {
            if (pkt->stream_index == stream_) return pkt;
            av_packet_unref(pkt.get());
        }
        return nullptr;
    }

    void rewind() { av_seek_frame(fmt_, stream_, INT64_MIN, AVSEEK_FLAG_BACKWARD); }

private:
    AVFormatContext *fmt_ = nullptr;
    int stream_ = -1;
};

// An owned decoder output.
struct Output {
    sp::VideoFrameRef frame;
    int64_t ptsUs = 0;

    Output(sp::VideoFrameRef f, int64_t pts) : frame(f), ptsUs(pts) {}
    Output(Output &&o) noexcept : frame(o.frame), ptsUs(o.ptsUs) { o.frame = {}; }
    Output(const Output &) = delete;
    ~Output() { sp::spFrameRelease(frame); }
    const AVFrame *av() const { return sp::spFrameAVFrame(frame); }
};

int setupFor(sp::VideoDecoding &decoder, const Clip &clip) {
    return decoder.setup(clip.par(), clip.timeBase().num, clip.timeBase().den);
}

// Feeds the whole clip and drains, passing each output to `sink`. Returns
// false on a decode error.
bool decodeClip(sp::VideoDecoding &decoder, Clip &clip, const std::function<void(Output)> &sink) {
    auto take = [&](sp::DecodedVideoOutput out) {
        if (!out.frame) return false;
        sink(Output(out.frame, out.ptsUs));
        return true;
    };
    while (PacketPtr pkt = clip.next()) {
        const sp::DecodedVideoOutput out = decoder.decodePacket(pkt.get());
        if (!out.frame && decoder.lastError() != 0) return false;
        take(out);
    }
    for (int i = 0; i < 64; i++) {
        if (!take(decoder.decodePacket(nullptr))) break;
    }
    return decoder.lastError() == 0;
}

std::vector<Output> decodeAll(sp::VideoDecoding &decoder, Clip &clip) {
    std::vector<Output> outputs;
    EXPECT_TRUE(decodeClip(decoder, clip, [&](Output o) { outputs.push_back(std::move(o)); }));
    return outputs;
}

std::shared_ptr<sp::D3D11Device> hardwareDevice() {
    auto device = sp::D3D11Device::shared();
    return device && !device->isWarp() ? device : nullptr;
}

struct ClipInfo {
    const char *name;
    int frames;
    AVPixelFormat softwareFormat; // what software decoding hands the renderer
    bool hardware;                // D3D11VA has a profile for it
};

const ClipInfo kClips[] = {
    {"h264_high_8bit.mp4", 24, AV_PIX_FMT_YUV420P, true},
    {"h264_high_1080p.mp4", 6, AV_PIX_FMT_YUV420P, true},
    {"h264_high10.mkv", 24, AV_PIX_FMT_YUV420P10LE, false},
    {"h264_444.mkv", 24, AV_PIX_FMT_NV12, false}, // converted
    {"hevc_main.mp4", 24, AV_PIX_FMT_YUV420P, true},
    {"hevc_main10_pq.mkv", 24, AV_PIX_FMT_YUV420P10LE, true},
    {"vp9_profile0.webm", 24, AV_PIX_FMT_YUV420P, true},
    {"vp9_profile2.webm", 24, AV_PIX_FMT_YUV420P10LE, true},
    {"av1_main_8bit.mkv", 24, AV_PIX_FMT_YUV420P, true},
    {"av1_main_10bit.mkv", 24, AV_PIX_FMT_YUV420P10LE, true},
};

class DecoderClipTest : public ::testing::TestWithParam<ClipInfo> {};

std::string clipTestName(const ::testing::TestParamInfo<ClipInfo> &info) {
    std::string name = info.param.name;
    for (char &c : name)
        if (!std::isalnum((unsigned char)c)) c = '_';
    return name;
}

// ---- Hardware frame readback -------------------------------------------------

// Y and chroma samples of the visible picture, as 16-bit code values of the
// stream's bit depth.
struct Planes {
    int width = 0, height = 0;
    std::vector<uint16_t> y, cb, cr;
};

Planes softwarePlanes(const AVFrame *f) {
    Planes p;
    p.width = f->width;
    p.height = f->height;
    const int cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;
    const bool ten = f->format == AV_PIX_FMT_YUV420P10LE;
    auto sample = [&](int plane, int x, int y) -> uint16_t {
        const uint8_t *row = f->data[plane] + (size_t)y * f->linesize[plane];
        return ten ? reinterpret_cast<const uint16_t *>(row)[x] : row[x];
    };
    for (int y = 0; y < p.height; y++)
        for (int x = 0; x < p.width; x++) p.y.push_back(sample(0, x, y));
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) {
            p.cb.push_back(sample(1, x, y));
            p.cr.push_back(sample(2, x, y));
        }
    return p;
}

Planes hardwarePlanes(sp::D3D11Device &device, const AVFrame *f) {
    Planes p;
    auto *texture = reinterpret_cast<ID3D11Texture2D *>(f->data[0]);
    const UINT slice = (UINT)(intptr_t)f->data[1];
    D3D11_TEXTURE2D_DESC desc;
    texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC sd = desc;
    sd.ArraySize = 1;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    sd.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    std::lock_guard<std::recursive_mutex> lock(device.mutex());
    if (FAILED(device.device()->CreateTexture2D(&sd, nullptr, &staging))) return p;
    device.context()->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, D3D11CalcSubresource(0, slice, 1),
                                            nullptr);
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(device.context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return p;
    const bool ten = desc.Format == DXGI_FORMAT_P010;
    const auto *base = static_cast<const uint8_t *>(mapped.pData);
    const uint8_t *uvBase = base + (size_t)mapped.RowPitch * desc.Height;
    auto sample = [&](const uint8_t *plane, int index, int y) -> uint16_t {
        const uint8_t *row = plane + (size_t)y * mapped.RowPitch;
        return ten ? (uint16_t)(reinterpret_cast<const uint16_t *>(row)[index] >> 6) : row[index];
    };
    p.width = f->width;
    p.height = f->height;
    const int cw = (f->width + 1) / 2, ch = (f->height + 1) / 2;
    for (int y = 0; y < p.height; y++)
        for (int x = 0; x < p.width; x++) p.y.push_back(sample(base, x, y));
    for (int y = 0; y < ch; y++)
        for (int x = 0; x < cw; x++) {
            p.cb.push_back(sample(uvBase, x * 2, y));
            p.cr.push_back(sample(uvBase, x * 2 + 1, y));
        }
    device.context()->Unmap(staging.Get(), 0);
    return p;
}

size_t mismatches(const std::vector<uint16_t> &a, const std::vector<uint16_t> &b) {
    if (a.size() != b.size()) return std::max(a.size(), b.size());
    size_t n = 0;
    for (size_t i = 0; i < a.size(); i++) n += a[i] != b[i];
    return n;
}

} // namespace

// ---- Every clip ------------------------------------------------------------

TEST_P(DecoderClipTest, SoftwareDecodesEveryFrameInOrder) {
    const ClipInfo info = GetParam();
    Clip clip(info.name);
    ASSERT_TRUE(clip.ok()) << info.name;
    sp::FFmpegVideoDecoder decoder;
    ASSERT_EQ(setupFor(decoder, clip), 0);
    EXPECT_FALSE(decoder.isHardwareDecoding());
    EXPECT_EQ(decoder.backend(), sp::VideoDecodingBackend::FFmpegSoftware);

    const auto outputs = decodeAll(decoder, clip);
    ASSERT_EQ((int)outputs.size(), info.frames);
    for (size_t i = 0; i < outputs.size(); i++) {
        const AVFrame *f = outputs[i].av();
        ASSERT_NE(f, nullptr);
        EXPECT_EQ(f->format, info.softwareFormat) << av_get_pix_fmt_name((AVPixelFormat)f->format);
        EXPECT_EQ(f->width, clip.par()->width);
        EXPECT_EQ(f->height, clip.par()->height);
        // 24 fps from zero.
        EXPECT_NEAR((double)outputs[i].ptsUs, i * 1e6 / 24.0, 1000.0) << "frame " << i;
    }
}

TEST_P(DecoderClipTest, D3D11VAMatchesSoftwareBitForBit) {
    const ClipInfo info = GetParam();
    auto device = hardwareDevice();
    if (!device) GTEST_SKIP() << "no hardware Direct3D 11 device";
    Clip clip(info.name);
    ASSERT_TRUE(clip.ok());
    if (!info.hardware) {
        EXPECT_FALSE(sp::FFmpegVideoDecoder::hardwareSupports(*device, clip.par()));
        sp::FFmpegVideoDecoder decoder({device});
        EXPECT_LT(setupFor(decoder, clip), 0); // the core then falls back to software
        EXPECT_FALSE(decoder.isHardwareDecoding());
        return;
    }
    if (!sp::FFmpegVideoDecoder::hardwareSupports(*device, clip.par()))
        GTEST_SKIP() << "this GPU has no decoder for " << info.name;

    sp::FFmpegVideoDecoder software;
    ASSERT_EQ(setupFor(software, clip), 0);
    const auto reference = decodeAll(software, clip);
    ASSERT_EQ((int)reference.size(), info.frames);

    clip.rewind();
    sp::FFmpegVideoDecoder hardware({device});
    ASSERT_EQ(setupFor(hardware, clip), 0);
    EXPECT_TRUE(hardware.isHardwareDecoding());
    EXPECT_EQ(hardware.backend(), sp::VideoDecodingBackend::FFmpegD3D11VA);
    size_t index = 0;
    ASSERT_TRUE(decodeClip(hardware, clip, [&](Output out) {
        ASSERT_LT(index, reference.size());
        const AVFrame *f = out.av();
        ASSERT_EQ(f->format, AV_PIX_FMT_D3D11);
        EXPECT_EQ(out.ptsUs, reference[index].ptsUs) << "frame " << index;
        const Planes hw = hardwarePlanes(*device, f);
        const Planes sw = softwarePlanes(reference[index].av());
        EXPECT_EQ(hw.width, sw.width);
        EXPECT_EQ(hw.height, sw.height);
        EXPECT_EQ(mismatches(hw.y, sw.y), 0u) << "luma of frame " << index;
        EXPECT_EQ(mismatches(hw.cb, sw.cb), 0u) << "Cb of frame " << index;
        EXPECT_EQ(mismatches(hw.cr, sw.cr), 0u) << "Cr of frame " << index;
        index++;
    }));
    EXPECT_EQ(index, reference.size());
}

INSTANTIATE_TEST_SUITE_P(Clips, DecoderClipTest, ::testing::ValuesIn(kClips), clipTestName);

// ---- Behaviour ---------------------------------------------------------------

// The hardware check accepts exactly what FFmpeg's D3D11VA hwaccel will
// decode; anything else must fail setup so the core uses software.
TEST(FFmpegVideoDecoder, HardwareSupportFollowsFFmpegProfiles) {
    auto device = hardwareDevice();
    if (!device) GTEST_SKIP() << "no hardware Direct3D 11 device";
    struct ParDeleter {
        void operator()(AVCodecParameters *p) const { avcodec_parameters_free(&p); }
    };
    auto variant = [](const char *name, int profile) {
        Clip clip(name);
        std::unique_ptr<AVCodecParameters, ParDeleter> par(avcodec_parameters_alloc());
        avcodec_parameters_copy(par.get(), clip.par());
        par->profile = profile;
        return par;
    };
    // Plain Baseline plays as Constrained Baseline.
    EXPECT_TRUE(sp::FFmpegVideoDecoder::hardwareSupports(*device, variant("h264_high_8bit.mp4", AV_PROFILE_H264_BASELINE).get()));
    EXPECT_TRUE(sp::FFmpegVideoDecoder::hardwareSupports(*device, variant("h264_high_8bit.mp4", AV_PROFILE_UNKNOWN).get()));
    EXPECT_FALSE(sp::FFmpegVideoDecoder::hardwareSupports(*device, variant("h264_high_8bit.mp4", AV_PROFILE_H264_EXTENDED).get()));
    EXPECT_FALSE(sp::FFmpegVideoDecoder::hardwareSupports(*device, variant("hevc_main.mp4", AV_PROFILE_HEVC_MAIN_STILL_PICTURE).get()));
    EXPECT_FALSE(sp::FFmpegVideoDecoder::hardwareSupports(*device, variant("hevc_main.mp4", AV_PROFILE_HEVC_REXT).get()));
    EXPECT_FALSE(sp::FFmpegVideoDecoder::hardwareSupports(*device, variant("vp9_profile0.webm", AV_PROFILE_VP9_1).get()));
    EXPECT_FALSE(sp::FFmpegVideoDecoder::hardwareSupports(*device, variant("av1_main_8bit.mkv", AV_PROFILE_AV1_HIGH).get()));

    std::unique_ptr<AVCodecParameters, ParDeleter> mpeg2(avcodec_parameters_alloc());
    mpeg2->codec_type = AVMEDIA_TYPE_VIDEO;
    mpeg2->codec_id = AV_CODEC_ID_MPEG2VIDEO;
    mpeg2->width = 720;
    mpeg2->height = 576;
    mpeg2->profile = AV_PROFILE_MPEG2_HIGH;
    EXPECT_FALSE(sp::FFmpegVideoDecoder::hardwareSupports(*device, mpeg2.get()));

    // A Baseline-tagged stream still decodes on the GPU.
    Clip clip("h264_high_8bit.mp4");
    auto baseline = variant("h264_high_8bit.mp4", AV_PROFILE_H264_BASELINE);
    sp::FFmpegVideoDecoder decoder({device});
    ASSERT_EQ(decoder.setup(baseline.get(), clip.timeBase().num, clip.timeBase().den), 0);
    const auto outputs = decodeAll(decoder, clip);
    EXPECT_EQ(outputs.size(), 24u);
}

// The stream's HDR10 description reaches the frame, where the renderer reads
// it; the range defaults to video range.
TEST(FFmpegVideoDecoder, WritesTheStreamColourIntoTheFrame) {
    Clip clip("hevc_main10_pq.mkv");
    ASSERT_TRUE(clip.ok());
    std::vector<std::shared_ptr<sp::D3D11Device>> devices = {nullptr};
    if (auto device = hardwareDevice()) devices.push_back(device);
    for (const auto &device : devices) {
        clip.rewind();
        sp::FFmpegVideoDecoder decoder({device});
        ASSERT_EQ(setupFor(decoder, clip), 0);
        const auto outputs = decodeAll(decoder, clip);
        ASSERT_FALSE(outputs.empty());
        const AVFrame *f = outputs.front().av();
        SCOPED_TRACE(decoder.decoderName());
        EXPECT_EQ(f->color_primaries, AVCOL_PRI_BT2020);
        EXPECT_EQ(f->color_trc, AVCOL_TRC_SMPTE2084);
        EXPECT_EQ(f->colorspace, AVCOL_SPC_BT2020_NCL);
        EXPECT_EQ(f->color_range, AVCOL_RANGE_MPEG);
    }
}

// A seek: flush drops what is pending, and decoding starts over cleanly.
TEST(FFmpegVideoDecoder, FlushRestartsFromTheNextKeyFrame) {
    Clip clip("h264_high_8bit.mp4");
    ASSERT_TRUE(clip.ok());
    sp::FFmpegVideoDecoder decoder;
    ASSERT_EQ(setupFor(decoder, clip), 0);
    for (int i = 0; i < 10; i++) {
        PacketPtr pkt = clip.next();
        ASSERT_TRUE(pkt);
        Output(decoder.decodePacket(pkt.get()).frame, 0);
    }
    decoder.flush();
    clip.rewind();
    const auto outputs = decodeAll(decoder, clip);
    ASSERT_EQ(outputs.size(), 24u);
    EXPECT_EQ(outputs.front().ptsUs, 0);
}

// Catching up to 0.5 s: nothing earlier than 40 ms before the target comes
// out, and nothing from there on is lost (frames 12 to 23 at 24 fps).
TEST(FFmpegVideoDecoder, CatchUpSkipsOutputsBeforeTheTarget) {
    for (const char *name : {"h264_high_8bit.mp4", "av1_main_8bit.mkv"}) {
        SCOPED_TRACE(name);
        Clip clip(name);
        ASSERT_TRUE(clip.ok());
        sp::FFmpegVideoDecoder decoder;
        ASSERT_EQ(setupFor(decoder, clip), 0);
        decoder.setCatchUpTargetUs(500000);
        const auto outputs = decodeAll(decoder, clip);
        decoder.setCatchUpTargetUs(0);
        ASSERT_FALSE(outputs.empty());
        EXPECT_EQ(outputs.size(), 12u);
        EXPECT_NEAR((double)outputs.front().ptsUs, 500000.0, 1000.0);
    }
}

// End of stream: draining returns the rest one by one, then nothing, with
// no error; "needs more input" before that is also error free.
TEST(FFmpegVideoDecoder, DrainReturnsTheRestThenNothing) {
    Clip clip("hevc_main.mp4");
    ASSERT_TRUE(clip.ok());
    sp::FFmpegVideoDecoder decoder;
    ASSERT_EQ(setupFor(decoder, clip), 0);
    int fed = 0, before = 0;
    while (PacketPtr pkt = clip.next()) {
        Output out(decoder.decodePacket(pkt.get()).frame, 0);
        EXPECT_EQ(decoder.lastError(), 0);
        before += out.frame ? 1 : 0;
        fed++;
    }
    int drained = 0;
    for (;;) {
        Output out(decoder.decodePacket(nullptr).frame, 0);
        EXPECT_EQ(decoder.lastError(), 0);
        if (!out.frame) break;
        drained++;
    }
    EXPECT_GT(drained, 0);
    EXPECT_EQ(before + drained, 24);
    EXPECT_FALSE(decoder.decodePacket(nullptr).frame);
}

// A player holds frames in its queue and on screen; the fixed D3D11VA pool
// has room for that many and decoding never stalls or fails.
TEST(FFmpegVideoDecoder, D3D11VAPoolLeavesRoomForHeldFrames) {
    auto device = hardwareDevice();
    if (!device) GTEST_SKIP() << "no hardware Direct3D 11 device";
    for (const char *name : {"h264_high_1080p.mp4", "hevc_main10_pq.mkv", "av1_main_10bit.mkv"}) {
        SCOPED_TRACE(name);
        Clip clip(name);
        ASSERT_TRUE(clip.ok());
        if (!sp::FFmpegVideoDecoder::hardwareSupports(*device, clip.par())) continue;
        sp::FFmpegVideoDecoderOptions options;
        options.device = device;
        options.heldHardwareFrames = 8;
        sp::FFmpegVideoDecoder decoder(options);
        ASSERT_EQ(setupFor(decoder, clip), 0);
        std::deque<Output> held;
        int count = 0;
        ASSERT_TRUE(decodeClip(decoder, clip, [&](Output out) {
            held.push_back(std::move(out));
            if (held.size() > 8) held.pop_front();
            count++;
        }));
        EXPECT_EQ(count, clip.par()->height == 1080 ? 6 : 24);
    }
}

// Through the renderer: a padded 1080p D3D11VA slice draws the same picture
// as its software decode.
TEST(FFmpegVideoDecoder, D3D11VAFramesRenderLikeSoftwareFrames) {
    auto device = hardwareDevice();
    if (!device) GTEST_SKIP() << "no hardware Direct3D 11 device";
    Clip clip("h264_high_1080p.mp4");
    ASSERT_TRUE(clip.ok());
    sp::FFmpegVideoDecoder software;
    ASSERT_EQ(setupFor(software, clip), 0);
    const auto reference = decodeAll(software, clip);
    ASSERT_FALSE(reference.empty());
    clip.rewind();
    sp::FFmpegVideoDecoder hardware({device});
    ASSERT_EQ(setupFor(hardware, clip), 0);
    std::vector<Output> decoded;
    ASSERT_TRUE(decodeClip(hardware, clip, [&](Output o) {
        if (decoded.empty()) decoded.push_back(std::move(o));
    }));
    ASSERT_EQ(decoded.size(), 1u);

    auto target = std::make_shared<sp::OffscreenRenderTarget>();
    sp::D3D11Renderer renderer(device, target, 1);
    ASSERT_TRUE(renderer.isReady());
    renderer.setViewportPixelSize(1920, 1080);
    renderer.setColorimetry(AVCOL_PRI_BT709, AVCOL_TRC_BT709, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 0);
    std::atomic<bool> synced{false};
    renderer.synchronizeOutputMode([&](bool) { synced = true; });
    auto waitFor = [](const std::function<bool()> &condition) {
        const auto deadline = steady_clock::now() + seconds(3);
        while (!condition() && steady_clock::now() < deadline) std::this_thread::sleep_for(milliseconds(2));
        return condition();
    };
    ASSERT_TRUE(waitFor([&] { return synced.load(); }));
    auto draw = [&](sp::VideoFrameRef frame) {
        const uint64_t before = renderer.committedFrameCount();
        EXPECT_TRUE(renderer.renderFrame(frame));
        EXPECT_TRUE(waitFor([&] { return renderer.committedFrameCount() > before; }));
        std::lock_guard<std::recursive_mutex> lock(device->mutex());
        return target->readPixels(device->device(), device->context());
    };
    const std::vector<float> fromSoftware = draw(reference.front().frame);
    const std::vector<float> fromHardware = draw(decoded.front().frame);
    ASSERT_EQ(fromSoftware.size(), fromHardware.size());
    float worst = 0;
    for (size_t i = 0; i < fromSoftware.size(); i++) worst = std::max(worst, std::fabs(fromSoftware[i] - fromHardware[i]));
    EXPECT_LE(worst, 1.0f / 255.0f) << "largest channel difference";
    EXPECT_EQ(renderer.hardRenderFailureCount(), 0u);
}
