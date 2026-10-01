#include "SPD3D11Renderer.hpp"

#include "Platform/SPThread.hpp"
#include "Player/SPAVFrameRef.hpp"
#include "SPColorMetadata.hpp"
#include "SPDoviReshapeQueue.hpp"
#include "SPRenderPolicy.hpp"
#include "SPRuntimeGates.hpp"

// fxc output: the vertex and pixel shaders of Shaders/Video.hlsl.
#include "SPVideoPS.h"
#include "SPVideoVS.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <type_traits>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace sp {
namespace {

// CPU mirrors of the shader's constant buffers (the Mac's SPShaderUniforms.h).
struct ColorUniforms {
    float uvals[32];
};
static_assert(std::is_standard_layout_v<ColorUniforms> && sizeof(ColorUniforms) == 128,
              "Video.hlsl Uniforms layout mismatch");

struct DoviUniforms {
    float values[kSPDoviFloatCount];
};
static_assert(sizeof(DoviUniforms) == 928 && sizeof(DoviUniforms) % 16 == 0, "Video.hlsl DoviUniforms layout mismatch");

// Windows only: see OutputUniforms in Video.hlsl.
struct OutputUniforms {
    float scrgbScale;
    float unmanaged;
    float visibleU;
    float visibleV;
};
static_assert(sizeof(OutputUniforms) == 16);

enum class SubmitOutcome {
    Committed,
    StaleEpoch,
    ConfigGenRace,
    OutputModeSwitch,
    Transient,
};

constexpr int kSubmitRetryLimit = 50;
constexpr int64_t kSubmitRetryDelayUs = 100000;

// The Mac renderer's serial submit queue with dispatch_after: one thread,
// tasks in order, delayed tasks when due. It owns the render target.
class SubmitThread {
public:
    explicit SubmitThread(unsigned logId) : logId_(logId) {}

    void start(std::function<void()> onExit) {
        thread_ = std::thread([this, onExit = std::move(onExit)] {
            char name[48];
            std::snprintf(name, sizeof(name), "sp.render.submit.c%u", logId_);
            spfs::setCurrentThreadName(name);
            loop();
            onExit();
        });
    }

    void post(std::function<void()> task) { postAt(0, std::move(task)); }
    void postAfter(int64_t delayUs, std::function<void()> task) { postAt(spNowUs() + delayUs, std::move(task)); }

    bool isCurrent() const { return std::this_thread::get_id() == thread_.get_id(); }

    // Stops after the running task; pending tasks are dropped. From the
    // thread itself (a completion releasing the renderer) it detaches.
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        cv_.notify_all();
        if (!thread_.joinable()) return;
        if (isCurrent()) thread_.detach();
        else thread_.join();
    }

private:
    void postAt(int64_t dueUs, std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (quit_) return;
            if (dueUs == 0) ready_.push_back(std::move(task));
            else delayed_.emplace(dueUs, std::move(task));
        }
        cv_.notify_all();
    }

    void loop() {
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            if (quit_) {
                // Dropped tasks may hold the renderer; release them unlocked.
                std::deque<std::function<void()>> ready = std::move(ready_);
                std::multimap<int64_t, std::function<void()>> delayed = std::move(delayed_);
                lock.unlock();
                return;
            }
            const int64_t now = spNowUs();
            while (!delayed_.empty() && delayed_.begin()->first <= now) {
                ready_.push_back(std::move(delayed_.begin()->second));
                delayed_.erase(delayed_.begin());
            }
            if (!ready_.empty()) {
                std::function<void()> task = std::move(ready_.front());
                ready_.pop_front();
                lock.unlock();
                task();
                task = nullptr;
                lock.lock();
                continue;
            }
            if (delayed_.empty()) cv_.wait(lock);
            else cv_.wait_for(lock, std::chrono::microseconds(delayed_.begin()->first - now));
        }
    }

    const unsigned logId_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> ready_;
    std::multimap<int64_t, std::function<void()>> delayed_;
    bool quit_ = false;
};

// The shader's view of one frame: its planes as Texture2DArray slices, and
// what the Mac renderer derives from the pixel format.
struct FramePlanes {
    ComPtr<ID3D11ShaderResourceView> y, uv, v;
    int bits = 8;
    bool planar = false;
    bool fullRange = false;
    float visibleU = 1.0f, visibleV = 1.0f;
};

bool isSoftwareFormatSupported(int format) {
    switch (format) {
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUV420P10LE:
        return true;
    default:
        return false;
    }
}

// Frame colour fields as the Mac renderer reads CoreVideo attachments: a
// field counts only when the decoder attached one CoreVideo can express.
bool frameTransferAttached(int trc) {
    return trc != AVCOL_TRC_RESERVED0 && trc != AVCOL_TRC_UNSPECIFIED && trc != AVCOL_TRC_RESERVED;
}
int frameMatrixIndex(int colorspace) { // -1: not attached
    switch (colorspace) {
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return 1;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M: return 2;
    case AVCOL_SPC_SMPTE240M: return 3;
    case AVCOL_SPC_BT709: return 0;
    default: return -1;
    }
}
bool framePrimariesAttached(int primaries) {
    switch (primaries) {
    case AVCOL_PRI_BT709:
    case AVCOL_PRI_BT2020:
    case AVCOL_PRI_SMPTE170M:
    case AVCOL_PRI_SMPTE240M:
    case AVCOL_PRI_BT470BG:
    case AVCOL_PRI_SMPTE432:
    case AVCOL_PRI_SMPTE431: return true;
    default: return false;
    }
}
int framePrimariesMatrixIndex(int primaries) {
    switch (primaries) {
    case AVCOL_PRI_BT2020: return 1;
    case AVCOL_PRI_SMPTE170M:
    case AVCOL_PRI_SMPTE240M: // SMPTE C
    case AVCOL_PRI_BT470BG: return 2; // EBU 3213
    default: return 0;
    }
}

} // namespace

struct D3D11Renderer::Impl : std::enable_shared_from_this<Impl> {
    Impl(std::shared_ptr<D3D11Device> dev, std::shared_ptr<RenderTarget> tgt, unsigned id)
        : device(std::move(dev)), target(std::move(tgt)), logId(id), submit(id) {
        displayEdrCapable = false;
        {
            std::lock_guard<std::recursive_mutex> dl(device->mutex());
            buildPipeline();
        }
    }

    void startThread() {
        // The thread keeps the renderer's state alive until it exits.
        submit.start([self = shared_from_this()] {});
    }

#define SPLOG(fmt, ...) std::fprintf(stderr, "[c%u]" fmt "\n", logId, ##__VA_ARGS__)

    // ---- Pipeline ---------------------------------------------------------

    void buildPipeline() {
        ID3D11Device *d = device->device();
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader> ps;
        if (FAILED(d->CreateVertexShader(g_videoVS, sizeof(g_videoVS), nullptr, &vs)) ||
            FAILED(d->CreatePixelShader(g_videoPS, sizeof(g_videoPS), nullptr, &ps))) {
            SPLOG("[Renderer] shader creation failed");
            return;
        }
        auto makeBuffer = [&](UINT size, ComPtr<ID3D11Buffer> &out) {
            D3D11_BUFFER_DESC bd = {};
            bd.ByteWidth = size;
            bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            return SUCCEEDED(d->CreateBuffer(&bd, nullptr, &out));
        };
        ComPtr<ID3D11Buffer> ub, db, ob;
        if (!makeBuffer(sizeof(ColorUniforms), ub) || !makeBuffer(sizeof(DoviUniforms), db) ||
            !makeBuffer(sizeof(OutputUniforms), ob))
            return;
        D3D11_SAMPLER_DESC sd = {};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        if (FAILED(d->CreateSamplerState(&sd, &sampler))) return;
        // The full-screen triangle's winding is counter-clockwise.
        D3D11_RASTERIZER_DESC rd = {};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> raster;
        if (FAILED(d->CreateRasterizerState(&rd, &raster))) return;
        vertexShader = vs;
        pixelShader = ps;
        uniformBuffer = ub;
        doviBuffer = db;
        outputBuffer = ob;
        samplerState = sampler;
        rasterizer = raster;
        pipelineReady.store(true);
    }

    // ---- Output mode (the Mac's applyOutputModeOnMain, on the submit thread)

    void applyOutputMode() {
        auto self = shared_from_this();
        submit.post([self] { self->applyOutputModeOnSubmit(); });
    }

    void setDisplayEDRHeadroomLocked(double currentEDR) {
        displayCurrentEDR = currentEDR;
        if (outputMode != 1) return;
        const float peak = (float)(std::max(currentEDR, 1.0) * 100.0);
        if (std::fabs(peak - displayPeakNits) > 1.0f) {
            if (spDebug())
                SPLOG("[Renderer] EDR headroom changed: %.0f -> %.0f nits (headroom %.2f)", displayPeakNits, peak,
                      currentEDR);
            displayPeakNits = peak;
        }
    }

    void applyOutputModeOnSubmit() {
        std::lock_guard<std::mutex> cfgLock(cfgMtx);
        const int mode = spDesiredRendererOutputMode(displayEdrCapable, hasHdr, sdrBoostRequested);
        const double currentEDR = displayCurrentEDR;
        if (mode == outputMode) {
            if (mode == 1) setDisplayEDRHeadroomLocked(currentEDR);
            if (mode == 0) {
                const int key = spSDRLayerTagKey(sourceGamut, transfer, hasHdr);
                if (key != layerTagKey) {
                    layerTagKey = key; // the shader converts; nothing to reconfigure
                    if (spDebug())
                        SPLOG("[Renderer] SDR output tag -> %d (gamut=%d trc=%d hdr=%d)", key, sourceGamut, transfer,
                              hasHdr);
                }
            }
            if (!pipelineReady.load()) {
                std::lock_guard<std::recursive_mutex> dl(device->mutex());
                buildPipeline();
            }
            return;
        }
        outputMode = mode;
        cfgGeneration.fetch_add(1, std::memory_order_relaxed);
        if (mode == 1) {
            displayPeakNits = (float)(std::max(currentEDR, 1.0) * 100.0);
        } else {
            displayPeakNits = 1000.0f;
            layerTagKey = spSDRLayerTagKey(sourceGamut, transfer, hasHdr);
        }
        if (spDebug()) {
            SPLOG("[Renderer] output path -> %s (content HDR=%d boost=%d display EDR=%d current=%.2f peak=%.0fnits)",
                  mode == 1 ? (hasHdr ? "EDR passthrough" : "EDR boost") : "SDR tone map", hasHdr,
                  (int)spEffectiveSdrBoost(sdrBoostRequested, hasHdr, outputMode), (int)displayEdrCapable, currentEDR,
                  displayPeakNits);
        }
        // The target takes the new pixel format before the next image.
    }

    void scheduleFrameOutputModeTransitionLocked() {
        if (outputModeSwitchPending.exchange(true, std::memory_order_acq_rel)) return;
        auto self = shared_from_this();
        submit.post([self] {
            self->applyOutputModeOnSubmit();
            self->outputModeSwitchPending.store(false, std::memory_order_release);
            self->drainSubmits();
        });
    }

    // The Mac's updateColorimetryFromFrame, reading the AVFrame colour fields
    // the decoder resolved (stream first, frame where the stream is silent).
    bool updateColorimetryFromFrame(const AVFrame *frame) {
        const bool trcAttached = frameTransferAttached(frame->color_trc);
        const bool priAttached = !streamDeclaredPrimaries && framePrimariesAttached(frame->color_primaries);
        const int matIndex = streamDeclaredMatrix ? -1 : frameMatrixIndex(frame->colorspace);
        if (spDebug() && !dbgAttachmentsDumped) {
            dbgAttachmentsDumped = true;
            SPLOG("[Renderer] frame colour: pri=%d trc=%d mat=%d", frame->color_primaries, frame->color_trc,
                  frame->colorspace);
        }
        if (!trcAttached && !priAttached && matIndex < 0) {
            const bool needsModeRetry = spOutputConfigNeedsTransition(
                displayEdrCapable, hasHdr, outputMode, sdrBoostRequested, sourceGamut, transfer, layerTagKey);
            if (needsModeRetry) scheduleFrameOutputModeTransitionLocked();
            return needsModeRetry;
        }

        bool changed = false;
        if (trcAttached) {
            const SPRendererTransfer frameTransfer = spRendererTransferForAVTrc(frame->color_trc);
            const int newTrc = frameTransfer.index;
            const int newHdr = frameTransfer.hdr ? 1 : 0;
            if (transfer != newTrc || hasHdr != newHdr) {
                if (newHdr == 1 || (transfer == 0 && hasHdr == 0)) {
                    transfer = newTrc;
                    hasHdr = newHdr;
                    changed = true;
                    if (spDebug()) SPLOG("[Renderer] frame colour: trc=%d hasHdr=%d (from the frame)", newTrc, newHdr);
                }
            }
        }
        if (matIndex >= 0) {
            if (primaries != matIndex) {
                primaries = matIndex;
                changed = true;
            }
        } else if (priAttached && !streamDeclaredPrimaries && !streamDeclaredMatrix) {
            const int newPri = framePrimariesMatrixIndex(frame->color_primaries);
            if (primaries != newPri) {
                primaries = newPri;
                changed = true;
            }
        }
        if (priAttached) {
            const int ng = spRendererSourceGamutForAV(frame->color_primaries, AVCOL_SPC_UNSPECIFIED);
            if (sourceGamut != ng) {
                sourceGamut = ng;
                changed = true;
            }
        }

        const bool needsModeRetry = spOutputConfigNeedsTransition(displayEdrCapable, hasHdr, outputMode,
                                                                  sdrBoostRequested, sourceGamut, transfer, layerTagKey);
        if (needsModeRetry) scheduleFrameOutputModeTransitionLocked();
        else if (changed) applyOutputMode();
        return needsModeRetry;
    }

    // ---- Frame textures ---------------------------------------------------

    // Views of one D3D11VA texture-array slice, cached like the Mac's
    // CVMetalTextureCache; discardPendingSubmits flushes the cache.
    struct SliceViews {
        ComPtr<ID3D11ShaderResourceView> y, uv;
    };

    bool makeSliceViews(ID3D11Texture2D *texture, UINT slice, DXGI_FORMAT format, SliceViews *out) {
        const bool tenBit = format == DXGI_FORMAT_P010;
        D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
        vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        vd.Texture2DArray.MipLevels = 1;
        vd.Texture2DArray.FirstArraySlice = slice;
        vd.Texture2DArray.ArraySize = 1;
        vd.Format = tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        if (FAILED(device->device()->CreateShaderResourceView(texture, &vd, &out->y))) return false;
        vd.Format = tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
        return SUCCEEDED(device->device()->CreateShaderResourceView(texture, &vd, &out->uv));
    }

    bool resolveHardwareFrame(const AVFrame *frame, FramePlanes *planes) {
        auto *texture = reinterpret_cast<ID3D11Texture2D *>(frame->data[0]);
        const UINT slice = (UINT)(intptr_t)frame->data[1];
        if (!texture) return false;
        ComPtr<ID3D11Device> owner;
        texture->GetDevice(&owner);
        if (owner.Get() != device->device()) return false; // a decoder on another device
        D3D11_TEXTURE2D_DESC desc;
        texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_NV12 && desc.Format != DXGI_FORMAT_P010) return false;
        planes->bits = desc.Format == DXGI_FORMAT_P010 ? 10 : 8;
        planes->planar = false;
        planes->visibleU = (float)frame->width / (float)desc.Width;
        planes->visibleV = (float)frame->height / (float)desc.Height;

        SliceViews views;
        if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
            const auto key = std::make_pair(texture, slice);
            auto it = sliceCache.find(key);
            if (it == sliceCache.end()) {
                // A pool of another shape is a decoder reinit: let the old one go.
                if (!sliceCache.empty()) {
                    D3D11_TEXTURE2D_DESC cached;
                    sliceCache.begin()->first.first->GetDesc(&cached);
                    if (cached.Width != desc.Width || cached.Height != desc.Height ||
                        cached.Format != desc.Format || cached.ArraySize != desc.ArraySize)
                        sliceCache.clear();
                }
                if (sliceCache.size() >= 64) sliceCache.clear();
                if (!makeSliceViews(texture, slice, desc.Format, &views)) return false;
                it = sliceCache.emplace(key, views).first;
            }
            views = it->second;
        } else {
            // A pool the driver would not make shader-readable: copy the slice.
            D3D11_TEXTURE2D_DESC cd = desc;
            if (!copyTexture || copyDesc.Width != desc.Width || copyDesc.Height != desc.Height ||
                copyDesc.Format != desc.Format) {
                cd.ArraySize = 1;
                cd.MipLevels = 1;
                cd.Usage = D3D11_USAGE_DEFAULT;
                cd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                cd.CPUAccessFlags = 0;
                cd.MiscFlags = 0;
                copyTexture.Reset();
                copyViews = {};
                if (FAILED(device->device()->CreateTexture2D(&cd, nullptr, &copyTexture)) ||
                    !makeSliceViews(copyTexture.Get(), 0, desc.Format, &copyViews)) {
                    copyTexture.Reset();
                    return false;
                }
                copyDesc = cd;
            }
            device->context()->CopySubresourceRegion(copyTexture.Get(), 0, 0, 0, 0, texture,
                                                     D3D11CalcSubresource(0, slice, desc.MipLevels), nullptr);
            views = copyViews;
        }
        planes->y = views.y;
        planes->uv = views.uv;
        return true;
    }

    // Uploaded software planes, one texture per plane like the Mac's
    // per-plane Metal textures.
    struct UploadPlane {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        int width = 0, height = 0;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    };

    bool uploadPlane(UploadPlane &plane, DXGI_FORMAT format, int width, int height, const uint8_t *data, int pitch) {
        if (!plane.texture || plane.width != width || plane.height != height || plane.format != format) {
            plane = {};
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = (UINT)width;
            td.Height = (UINT)height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = format;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
            vd.Format = format;
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray.MipLevels = 1;
            vd.Texture2DArray.ArraySize = 1;
            if (FAILED(device->device()->CreateTexture2D(&td, nullptr, &plane.texture)) ||
                FAILED(device->device()->CreateShaderResourceView(plane.texture.Get(), &vd, &plane.view))) {
                plane = {};
                return false;
            }
            plane.width = width;
            plane.height = height;
            plane.format = format;
        }
        device->context()->UpdateSubresource(plane.texture.Get(), 0, nullptr, data, (UINT)pitch, 0);
        return true;
    }

    bool resolveSoftwareFrame(const AVFrame *frame, FramePlanes *planes) {
        const int w = frame->width, h = frame->height;
        const int cw = (w + 1) / 2, ch = (h + 1) / 2;
        switch (frame->format) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_P010LE: {
            const bool tenBit = frame->format == AV_PIX_FMT_P010LE;
            planes->bits = tenBit ? 10 : 8;
            planes->planar = false;
            if (!uploadPlane(upload[0], tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM, w, h, frame->data[0],
                             frame->linesize[0]) ||
                !uploadPlane(upload[1], tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM, cw, ch,
                             frame->data[1], frame->linesize[1]))
                return false;
            planes->y = upload[0].view;
            planes->uv = upload[1].view;
            return true;
        }
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_YUV420P10LE: {
            // 16-bit containers of LSB-justified 10-bit values, as the Mac's
            // Y3LV/Y3LF formats: the shader scales them by 65535/1023.
            const bool tenBit = frame->format == AV_PIX_FMT_YUV420P10LE;
            const DXGI_FORMAT f = tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
            planes->bits = tenBit ? 10 : 8;
            planes->planar = true;
            if (!uploadPlane(upload[0], f, w, h, frame->data[0], frame->linesize[0]) ||
                !uploadPlane(upload[1], f, cw, ch, frame->data[1], frame->linesize[1]) ||
                !uploadPlane(upload[2], f, cw, ch, frame->data[2], frame->linesize[2]))
                return false;
            planes->y = upload[0].view;
            planes->uv = upload[1].view;
            planes->v = upload[2].view;
            return true;
        }
        default:
            return false;
        }
    }

    // ---- Submission (the Mac's spAcceptSubmit / spDrainSubmits) ------------

    bool acceptSubmit(VideoFrameRef frame, bool speculative) {
        if (!pipelineReady.load() || !frame) return false;
        bool wakeNow = false;
        {
            std::lock_guard<std::mutex> lk(submitMtx);
            if (speculative && submitAcceptedInEpoch != 0) return false;
            spFrameRelease(submitPendingFrame);
            submitPendingFrame = spFrameRetain(frame);
            submitPendingClear = false;
            submitPendingEpoch = submitSessionEpoch.load(std::memory_order_relaxed);
            submitPendingSpeculative = speculative;
            submitAcceptedInEpoch++;
            submitRetryCount = 0;
            submitParked = false;

            // The overlay and metadata stay paired with this frame even if the
            // drain runs later.
            submitPendingSubTex = subtitleTexture;
            submitPendingSubRect = subtitleRect;
            submitPendingDoviValid = false;
            if (doviIPT.load()) {
                std::lock_guard<std::mutex> dk(doviMtx);
                std::memcpy(submitPendingDovi.values, doviQueue.current(), sizeof(submitPendingDovi.values));
                submitPendingDoviValid = true;
            }
            if (submitsSuspended.load(std::memory_order_relaxed)) {
                wakeNow = false;
            } else if (submitDrainScheduled) {
                wakeNow = submitRetryDelayed;
                submitRetryDelayed = false;
            } else {
                submitDrainScheduled = true;
                wakeNow = true;
            }
        }
        if (wakeNow) postDrain();
        return true;
    }

    void postDrain() {
        auto self = shared_from_this();
        submit.post([self] { self->drainSubmits(); });
    }

    void clearToBlack() {
        bool wakeNow = false;
        {
            std::lock_guard<std::mutex> lk(submitMtx);
            spFrameRelease(submitPendingFrame);
            submitPendingFrame = {};
            submitPendingSubTex.Reset();
            submitPendingClear = true;
            submitPendingEpoch = submitSessionEpoch.load(std::memory_order_relaxed);
            submitPendingSpeculative = false;
            submitRetryCount = 0;
            submitParked = false;
            if (submitDrainScheduled) {
                wakeNow = submitRetryDelayed;
                submitRetryDelayed = false;
            } else {
                submitDrainScheduled = true;
                wakeNow = true;
            }
        }
        if (wakeNow) postDrain();
    }

    void discardPendingSubmits() {
        {
            std::lock_guard<std::mutex> lk(submitMtx);
            submitSessionEpoch.fetch_add(1, std::memory_order_relaxed);
            spFrameRelease(submitPendingFrame);
            submitPendingFrame = {};
            submitPendingClear = false;
            submitPendingSubTex.Reset();
            submitPendingDoviValid = false;
            submitPendingSpeculative = false;
            submitAcceptedInEpoch = 0;
            submitParked = false;
            submitRetryCount = 0;
        }
        // The Mac flushes its texture cache here. The slice views and the last
        // frame belong to the submit thread; dropping them there lets the old
        // session's decoder surface pool go.
        auto self = shared_from_this();
        submit.post([self] { self->releaseDecoderSurfaces(); });
    }

    void kickSubmitDrain() {
        bool schedule = false;
        {
            std::lock_guard<std::mutex> lk(submitMtx);
            if ((submitPendingFrame || submitPendingClear) && !submitDrainScheduled) {
                submitRetryCount = 0;
                submitParked = false;
                submitDrainScheduled = true;
                schedule = true;
            }
        }
        if (schedule) postDrain();
    }

    void drainSubmits() {
        for (;;) {
            VideoFrameRef frame;
            bool clear = false;
            ComPtr<ID3D11ShaderResourceView> subTex;
            RenderRect subRect;
            DoviUniforms dovi;
            bool doviValid = false;
            bool speculative = false;
            uint64_t taskEpoch = 0;
            {
                std::lock_guard<std::mutex> lk(submitMtx);
                submitRetryDelayed = false;
                if (submitParked) {
                    submitDrainScheduled = false;
                    return;
                }
                if (submitsSuspended.load(std::memory_order_relaxed) && !submitPendingClear) {
                    submitDrainScheduled = false;
                    return;
                }
                frame = submitPendingFrame;
                submitPendingFrame = {};
                clear = submitPendingClear;
                submitPendingClear = false;
                subTex = submitPendingSubTex;
                subRect = submitPendingSubRect;
                taskEpoch = submitPendingEpoch;
                speculative = submitPendingSpeculative;
                doviValid = submitPendingDoviValid;
                if (doviValid) dovi = submitPendingDovi;
                if (!frame && !clear) {
                    submitDrainScheduled = false;
                    return;
                }
            }
            SubmitOutcome outcome;
            if (clear && !frame) {
                outcome = clearToBlackOutcome(taskEpoch);
            } else {
                outcome = renderFrameOutcome(frame, subTex.Get(), subRect, doviValid ? &dovi : nullptr, taskEpoch,
                                             speculative);
            }
            if (outcome != SubmitOutcome::Committed) {
                const bool waitsForOutputMode = outcome == SubmitOutcome::OutputModeSwitch;
                if (outcome == SubmitOutcome::StaleEpoch) {
                    spFrameRelease(frame);
                    continue;
                }
                std::lock_guard<std::mutex> lk(submitMtx);
                if (!submitPendingFrame && !submitPendingClear) {
                    // Retained intent: nothing newer arrived.
                    submitPendingFrame = frame;
                    submitPendingClear = clear;
                    submitPendingSubTex = subTex;
                    submitPendingSubRect = subRect;
                    submitPendingEpoch = taskEpoch;
                    submitPendingSpeculative = speculative;
                    submitPendingDovi = dovi;
                    submitPendingDoviValid = doviValid;
                    if (waitsForOutputMode) return; // the transition drains again
                    if (submitRetryCount < kSubmitRetryLimit) {
                        submitRetryCount++;
                        auto self = shared_from_this();
                        if (outcome == SubmitOutcome::ConfigGenRace) {
                            submit.post([self] { self->drainSubmits(); });
                        } else {
                            submitRetryDelayed = true;
                            submit.postAfter(kSubmitRetryDelayUs, [self] { self->drainSubmits(); });
                        }
                        return;
                    }
                    SPLOG("[Renderer] submit retries exhausted; parked until a new frame or the window is visible");
                    submitParked = true;
                    submitDrainScheduled = false;
                    return;
                }
                spFrameRelease(frame);
                if (waitsForOutputMode) return;
                continue;
            }
            spFrameRelease(frame);
        }
    }

    // Brings the target to the output mode and viewport; submit thread.
    bool ensureTarget(int mode, int width, int height) {
        if (targetMode == mode && targetWidth == width && targetHeight == height) return true;
        std::lock_guard<std::recursive_mutex> dl(device->mutex());
        device->context()->OMSetRenderTargets(0, nullptr, nullptr);
        if (!target->configure(device->device(), mode, width, height)) return false;
        targetMode = mode;
        targetWidth = width;
        targetHeight = height;
        return true;
    }

    SubmitOutcome clearToBlackOutcome(uint64_t taskEpoch) {
        int mode, w, h;
        {
            std::lock_guard<std::mutex> cfgLock(cfgMtx);
            mode = outputMode;
            w = viewportWidth;
            h = viewportHeight;
        }
        if (!ensureTarget(mode, w, h)) return SubmitOutcome::Transient;
        ID3D11RenderTargetView *rtv = target->acquire();
        if (!rtv) return SubmitOutcome::Transient;
        if (taskEpoch != submitSessionEpoch.load(std::memory_order_relaxed)) return SubmitOutcome::StaleEpoch;
        std::lock_guard<std::recursive_mutex> dl(device->mutex());
        const float black[4] = {0, 0, 0, 1};
        device->context()->ClearRenderTargetView(rtv, black);
        target->present(device->context());
        releaseDecoderSurfaces();
        return SubmitOutcome::Committed;
    }

    // On the submit thread: nothing on screen samples a decoder surface any
    // more, so stop holding the last frame and the views into its pool.
    void releaseDecoderSurfaces() {
        spFrameRelease(lastRendered);
        lastRendered = {};
        sliceCache.clear();
    }

    SubmitOutcome renderFrameOutcome(VideoFrameRef handle, ID3D11ShaderResourceView *subTex, RenderRect subRect,
                                     const DoviUniforms *pairedDovi, uint64_t taskEpoch, bool speculative) {
        if (taskEpoch != submitSessionEpoch.load(std::memory_order_relaxed)) return SubmitOutcome::StaleEpoch;

        std::unique_lock<std::mutex> cfgLock(cfgMtx);
        if (!pipelineReady.load()) {
            scheduleFrameOutputModeTransitionLocked();
            return SubmitOutcome::Transient;
        }
        if (taskEpoch != submitSessionEpoch.load(std::memory_order_relaxed)) return SubmitOutcome::StaleEpoch;

        const AVFrame *frame = spFrameAVFrame(handle);
        if (!frame || (frame->format != AV_PIX_FMT_D3D11 && !isSoftwareFormatSupported(frame->format))) {
            SPLOG("[Renderer] unsupported frame format %d", frame ? frame->format : -1);
            hardRenderFailures.fetch_add(1, std::memory_order_relaxed);
            return SubmitOutcome::Transient;
        }
        if (!doviIPT.load()) {
            if (updateColorimetryFromFrame(frame)) return SubmitOutcome::OutputModeSwitch;
        }

        FramePlanes planes;
        {
            std::lock_guard<std::recursive_mutex> dl(device->mutex());
            const bool ok = frame->format == AV_PIX_FMT_D3D11 ? resolveHardwareFrame(frame, &planes)
                                                              : resolveSoftwareFrame(frame, &planes);
            if (!ok) {
                hardRenderFailures.fetch_add(1, std::memory_order_relaxed);
                return SubmitOutcome::Transient;
            }
        }
        // Range and depth come from the frame, as the Mac takes them from the
        // pixel format.
        planes.fullRange = frame->color_range == AVCOL_RANGE_JPEG || frame->format == AV_PIX_FMT_YUVJ420P;
        bits = planes.bits;
        planar = planes.planar ? 1 : 0;
        range = planes.fullRange ? 1 : 0;
        if (spDebug() && frame->format != dbgLoggedFormat) {
            dbgLoggedFormat = frame->format;
            SPLOG("[Renderer] frame format %d (%d bit, range=%s, %s)", frame->format, bits, range ? "full" : "video",
                  planar ? "3 planes" : "2 planes");
        }

        const uint64_t cfgGenAtAcquire = cfgGeneration.load(std::memory_order_relaxed);
        const int mode = outputMode, vw = viewportWidth, vh = viewportHeight;
        cfgLock.unlock();
        if (!ensureTarget(mode, vw, vh)) return SubmitOutcome::Transient;
        ID3D11RenderTargetView *rtv = target->acquire();
        if (!rtv) return SubmitOutcome::Transient;
        cfgLock.lock();
        if (taskEpoch != submitSessionEpoch.load(std::memory_order_relaxed)) return SubmitOutcome::StaleEpoch;
        if (cfgGeneration.load(std::memory_order_relaxed) != cfgGenAtAcquire) return SubmitOutcome::ConfigGenRace;

        ColorUniforms uniforms;
        std::memset(uniforms.uvals, 0, sizeof(uniforms.uvals));
        uniforms.uvals[0] = (float)frame->width;
        uniforms.uvals[1] = (float)frame->height;
        uniforms.uvals[2] = (float)viewportWidth;
        uniforms.uvals[3] = (float)viewportHeight;
        uniforms.uvals[4] = (float)primaries;
        uniforms.uvals[5] = (float)transfer;
        uniforms.uvals[6] = (float)range;
        uniforms.uvals[7] = (float)bits;
        uniforms.uvals[8] = (float)hasHdr;
        uniforms.uvals[9] = peakNits;
        uniforms.uvals[10] = sampleAspect;
        {
            const SPAspectModel am = spAspectModel(uniforms.uvals[0], uniforms.uvals[1], sampleAspect, forcedAspect,
                                                   cropAspect);
            uniforms.uvals[27] = am.eff;
            uniforms.uvals[28] = am.cw;
            uniforms.uvals[29] = am.ch;
        }
        uniforms.uvals[11] = doviIPT.load() ? 1.0f : 0.0f;
        uniforms.uvals[12] = subRect.x;
        uniforms.uvals[13] = subRect.y;
        uniforms.uvals[14] = subRect.width;
        uniforms.uvals[15] = subRect.height;
        uniforms.uvals[16] = subTex ? 1.0f : 0.0f;
        uniforms.uvals[17] = (float)aspectMode;
        uniforms.uvals[18] = (float)rotation;
        uniforms.uvals[19] = (float)mirror;
        uniforms.uvals[20] = brightness;
        uniforms.uvals[21] = contrast;
        uniforms.uvals[22] = saturation;
        uniforms.uvals[23] = gamma;
        uniforms.uvals[24] = displayPeakNits;
        uniforms.uvals[25] = (float)outputMode;
        uniforms.uvals[26] = (float)sourceGamut;
        uniforms.uvals[30] = (float)planar;
        if (spEffectiveSdrBoost(sdrBoostRequested, hasHdr, outputMode)) {
            const float targetHeadroom = (float)std::max(displayCurrentEDR * 0.85, 1.0);
            const int64_t nowUs = spNowUs();
            if (xdrHeadroomSmoothed <= 0.0f || xdrHeadroomAtUs == 0 || targetHeadroom <= xdrHeadroomSmoothed) {
                xdrHeadroomSmoothed = targetHeadroom;
            } else {
                const float dt = (float)std::max<int64_t>(nowUs - xdrHeadroomAtUs, 0) / 1e6f;
                const float a = 1.0f - std::exp(-dt / 0.3f);
                xdrHeadroomSmoothed += (targetHeadroom - xdrHeadroomSmoothed) * a;
                if (std::fabs(targetHeadroom - xdrHeadroomSmoothed) < 0.005f) xdrHeadroomSmoothed = targetHeadroom;
            }
            xdrHeadroomAtUs = nowUs;
            uniforms.uvals[31] = xdrHeadroomSmoothed;
        } else {
            uniforms.uvals[31] = 0.0f;
            xdrHeadroomSmoothed = 0.0f;
            xdrHeadroomAtUs = 0;
        }

        DoviUniforms doviSnapshot = {};
        if (doviIPT.load()) {
            if (pairedDovi) {
                doviSnapshot = *pairedDovi;
            } else {
                std::lock_guard<std::mutex> lock(doviMtx);
                std::memcpy(doviSnapshot.values, doviQueue.current(), sizeof(doviSnapshot.values));
            }
        }

        static const bool unmanaged = getenv("SP_NO_COLORMATCH") != nullptr;
        OutputUniforms output = {outputMode == 1 ? sdrWhiteNits / 80.0f : 1.0f, unmanaged ? 1.0f : 0.0f,
                                 planes.visibleU, planes.visibleV};

        {
            std::lock_guard<std::recursive_mutex> dl(device->mutex());
            ID3D11DeviceContext *ctx = device->context();
            auto upload = [&](ID3D11Buffer *buffer, const void *data, size_t size) {
                D3D11_MAPPED_SUBRESOURCE mapped;
                if (FAILED(ctx->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
                std::memcpy(mapped.pData, data, size);
                ctx->Unmap(buffer, 0);
                return true;
            };
            if (!upload(uniformBuffer.Get(), &uniforms, sizeof(uniforms)) ||
                !upload(doviBuffer.Get(), &doviSnapshot, sizeof(doviSnapshot)) ||
                !upload(outputBuffer.Get(), &output, sizeof(output)))
                return SubmitOutcome::Transient;

            const float black[4] = {0, 0, 0, 1};
            ctx->ClearRenderTargetView(rtv, black);
            ctx->OMSetRenderTargets(1, &rtv, nullptr);
            const D3D11_VIEWPORT vp = {0, 0, (float)targetWidth, (float)targetHeight, 0, 1};
            ctx->RSSetViewports(1, &vp);
            ctx->RSSetState(rasterizer.Get());
            // The context is shared with the decoder and other users of the
            // device; set every stage this draw depends on.
            ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            ctx->OMSetDepthStencilState(nullptr, 0);
            ctx->HSSetShader(nullptr, nullptr, 0);
            ctx->DSSetShader(nullptr, nullptr, 0);
            ctx->GSSetShader(nullptr, nullptr, 0);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(vertexShader.Get(), nullptr, 0);
            ctx->PSSetShader(pixelShader.Get(), nullptr, 0);
            ID3D11Buffer *vsBuffers[] = {uniformBuffer.Get()};
            ctx->VSSetConstantBuffers(0, 1, vsBuffers);
            ID3D11Buffer *psBuffers[] = {uniformBuffer.Get(), doviBuffer.Get(), outputBuffer.Get()};
            ctx->PSSetConstantBuffers(0, 3, psBuffers);
            ID3D11SamplerState *samplers[] = {samplerState.Get()};
            ctx->PSSetSamplers(0, 1, samplers);
            // t3 carries UV for bi-planar frames, as on the Mac.
            ID3D11ShaderResourceView *views[] = {planes.y.Get(), planes.uv.Get(), subTex,
                                                 planes.v ? planes.v.Get() : planes.uv.Get()};
            ctx->PSSetShaderResources(0, 4, views);
            ctx->Draw(3, 0);
            ID3D11ShaderResourceView *none[4] = {};
            ctx->PSSetShaderResources(0, 4, none);
            target->present(ctx);
        }
        // Keep the frame until the next one is drawn, so its decoder surface
        // is not reused while the GPU may still read it.
        spFrameRelease(lastRendered);
        lastRendered = spFrameRetain(handle);

        if (!speculative && taskEpoch == submitSessionEpoch.load(std::memory_order_relaxed))
            committedFrames.fetch_add(1, std::memory_order_relaxed);
        return SubmitOutcome::Committed;
    }

    ~Impl() {
        spFrameRelease(submitPendingFrame);
        spFrameRelease(lastRendered);
    }

#undef SPLOG

    std::shared_ptr<D3D11Device> device;
    std::shared_ptr<RenderTarget> target;
    const unsigned logId;
    SubmitThread submit;

    // Pipeline, created once: a D3D11 pipeline does not depend on the
    // target's pixel format, unlike a Metal PSO.
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11Buffer> uniformBuffer, doviBuffer, outputBuffer;
    ComPtr<ID3D11SamplerState> samplerState;
    ComPtr<ID3D11RasterizerState> rasterizer;
    std::atomic<bool> pipelineReady{false};

    // Submit thread only.
    std::map<std::pair<ID3D11Texture2D *, UINT>, SliceViews> sliceCache;
    ComPtr<ID3D11Texture2D> copyTexture;
    D3D11_TEXTURE2D_DESC copyDesc = {};
    SliceViews copyViews;
    UploadPlane upload[3];
    int targetMode = -1, targetWidth = 0, targetHeight = 0;
    VideoFrameRef lastRendered;
    bool dbgAttachmentsDumped = false;
    int dbgLoggedFormat = -1;

    // Configuration, under cfgMtx.
    std::mutex cfgMtx;
    int primaries = 0, transfer = 0, range = 0, bits = 8, hasHdr = 0;
    int planar = 0;
    int sourceGamut = kSPGamutBT709;
    int layerTagKey = kSPTag709;
    bool streamDeclaredMatrix = false;
    bool streamDeclaredPrimaries = false;
    bool displayEdrCapable = false;
    double displayCurrentEDR = 1.0;
    bool sdrBoostRequested = false;
    float xdrHeadroomSmoothed = 0.0f;
    int64_t xdrHeadroomAtUs = 0;
    float peakNits = 1000.0f;
    int outputMode = 0;
    float displayPeakNits = 1000.0f;
    float sdrWhiteNits = 80.0f;
    int viewportWidth = 1920, viewportHeight = 1080;
    float sampleAspect = 1.0f;
    float forcedAspect = 0.0f;
    float cropAspect = 0.0f;
    int aspectMode = 0, rotation = 0, mirror = 0;
    float brightness = 0, contrast = 1, saturation = 1, gamma = 1;
    std::atomic<uint64_t> cfgGeneration{0};
    std::atomic<bool> outputModeSwitchPending{false};
    std::atomic<bool> doviIPT{false};

    // Dolby Vision, under doviMtx.
    std::mutex doviMtx;
    DoviReshapeQueue doviQueue;
    bool dbgDoviBindLogged = false;

    // Submission, under submitMtx.
    std::mutex submitMtx;
    VideoFrameRef submitPendingFrame;
    bool submitPendingClear = false;
    bool submitDrainScheduled = false;
    int submitRetryCount = 0;
    bool submitRetryDelayed = false;
    bool submitParked = false;
    ComPtr<ID3D11ShaderResourceView> submitPendingSubTex;
    RenderRect submitPendingSubRect;
    DoviUniforms submitPendingDovi;
    bool submitPendingDoviValid = false;
    bool submitPendingSpeculative = false;
    uint64_t submitAcceptedInEpoch = 0;
    std::atomic<uint64_t> submitSessionEpoch{0};
    uint64_t submitPendingEpoch = 0;
    std::atomic<bool> submitsSuspended{false};
    ComPtr<ID3D11ShaderResourceView> subtitleTexture;
    RenderRect subtitleRect;

    std::atomic<uint64_t> committedFrames{0};
    std::atomic<uint64_t> hardRenderFailures{0};
};

// ---- D3D11Renderer ---------------------------------------------------------

D3D11Renderer::D3D11Renderer(std::shared_ptr<D3D11Device> device, std::shared_ptr<RenderTarget> target,
                             unsigned logId)
    : impl_(std::make_shared<Impl>(std::move(device), std::move(target), logId)) {
    impl_->startThread();
}

D3D11Renderer::~D3D11Renderer() { impl_->submit.stop(); }

unsigned D3D11Renderer::logId() const { return impl_->logId; }
const std::shared_ptr<D3D11Device> &D3D11Renderer::device() const { return impl_->device; }

bool D3D11Renderer::renderFrame(VideoFrameRef frame) { return impl_->acceptSubmit(frame, false); }
bool D3D11Renderer::renderSpeculativeFirstFrame(VideoFrameRef frame) { return impl_->acceptSubmit(frame, true); }
void D3D11Renderer::clearToBlack() { impl_->clearToBlack(); }
void D3D11Renderer::kickSubmitDrain() { impl_->kickSubmitDrain(); }
void D3D11Renderer::discardPendingSubmits() { impl_->discardPendingSubmits(); }
void D3D11Renderer::setSubmitsSuspended(bool suspended) { impl_->submitsSuspended.store(suspended); }
uint64_t D3D11Renderer::committedFrameCount() const { return impl_->committedFrames.load(); }
uint64_t D3D11Renderer::hardRenderFailureCount() const { return impl_->hardRenderFailures.load(); }
bool D3D11Renderer::isReady() const { return impl_->pipelineReady.load(); }

std::string D3D11Renderer::outputModeDescription() {
    Impl &m = *impl_;
    std::lock_guard<std::mutex> lock(m.cfgMtx);
    char text[64];
    if (!m.hasHdr) {
        if (spEffectiveSdrBoost(m.sdrBoostRequested, m.hasHdr, m.outputMode)) {
            std::snprintf(text, sizeof(text), "SDR boost %.1fx", std::max(m.displayCurrentEDR, 1.0));
            return text;
        }
        return "SDR";
    }
    if (m.outputMode == 1) {
        std::snprintf(text, sizeof(text), "HDR %.1fx", std::max(m.displayCurrentEDR, 1.0));
        return text;
    }
    return "HDR tone-mapped to SDR";
}

void D3D11Renderer::setColorimetry(int primaries, int trc, int colorspace, int range, float peakNits) {
    Impl &m = *impl_;
    {
        std::lock_guard<std::mutex> cfgLock(m.cfgMtx);
        switch (colorspace) {
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: m.primaries = 1; break;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: m.primaries = 2; break; // 601
        case AVCOL_SPC_SMPTE240M: m.primaries = 3; break;
        case AVCOL_SPC_BT709: m.primaries = 0; break;
        default:
            switch (primaries) {
            case AVCOL_PRI_BT2020: m.primaries = 1; break;
            case AVCOL_PRI_SMPTE170M: m.primaries = 2; break; // 601
            case AVCOL_PRI_BT470BG: m.primaries = 2; break;   // 601
            case AVCOL_PRI_SMPTE240M: m.primaries = 3; break;
            default: m.primaries = 0; break;
            }
            break;
        }
        const SPRendererTransfer streamTransfer = spRendererTransferForAVTrc(trc);
        m.transfer = streamTransfer.index;
        m.hasHdr = streamTransfer.hdr ? 1 : 0;
        m.sourceGamut = spRendererSourceGamutForAV(primaries, colorspace);
        m.streamDeclaredPrimaries = primaries > 0 && primaries != AVCOL_PRI_UNSPECIFIED;
        m.streamDeclaredMatrix = colorspace == AVCOL_SPC_BT709 || colorspace == AVCOL_SPC_BT470BG ||
                                 colorspace == AVCOL_SPC_SMPTE170M || colorspace == AVCOL_SPC_SMPTE240M ||
                                 colorspace == AVCOL_SPC_BT2020_NCL || colorspace == AVCOL_SPC_BT2020_CL;
        m.range = range == AVCOL_RANGE_JPEG ? 1 : 0;
        m.bits = 8;
        m.peakNits = peakNits > 0 ? peakNits : 1000.0f;
    }
    m.applyOutputMode();
}

void D3D11Renderer::synchronizeOutputMode(std::function<void(bool ready)> completion) {
    Impl &m = *impl_;
    bool readyInline = false;
    {
        std::lock_guard<std::mutex> cfgLock(m.cfgMtx);
        const int desiredMode = spDesiredRendererOutputMode(m.displayEdrCapable, m.hasHdr, m.sdrBoostRequested);
        readyInline = !spOutputConfigNeedsTransition(m.displayEdrCapable, m.hasHdr, m.outputMode, m.sdrBoostRequested,
                                                     m.sourceGamut, m.transfer, m.layerTagKey) &&
                      m.pipelineReady.load();
        if (readyInline && desiredMode == 1) m.setDisplayEDRHeadroomLocked(m.displayCurrentEDR);
    }
    // Never call client code with the configuration lock held.
    if (readyInline) {
        if (completion) completion(true);
        return;
    }
    // FIFO after the setters' queued applies; applying once more is idempotent.
    // A task dropped by shutdown still answers, with false, as the Mac's
    // queue would have run it.
    struct Answer {
        std::function<void(bool)> completion;
        ~Answer() {
            if (completion) completion(false);
        }
        void operator()(bool ready) {
            auto c = std::move(completion);
            completion = nullptr;
            if (c) c(ready);
        }
    };
    auto answer = std::make_shared<Answer>();
    answer->completion = std::move(completion);
    auto impl = impl_;
    m.submit.post([impl, answer] {
        impl->applyOutputModeOnSubmit();
        (*answer)(impl->pipelineReady.load());
    });
}

void D3D11Renderer::setViewportPixelSize(int width, int height) {
    if (width <= 0 || height <= 0) return;
    Impl &m = *impl_;
    std::lock_guard<std::mutex> lock(m.cfgMtx);
    m.viewportWidth = width;
    m.viewportHeight = height;
    m.cfgGeneration.fetch_add(1, std::memory_order_relaxed);
}

void D3D11Renderer::updateOutputMode(double currentEDR, double potentialEDR) {
    if (getenv("SP_FORCE_EDR")) {
        currentEDR = 10.0;
        potentialEDR = 10.0;
    }
    if (getenv("SP_NO_EDR")) potentialEDR = 1.0;
    Impl &m = *impl_;
    {
        std::lock_guard<std::mutex> lock(m.cfgMtx);
        m.displayEdrCapable = potentialEDR > 1.01;
        m.displayCurrentEDR = currentEDR;
    }
    m.applyOutputMode();
}

void D3D11Renderer::setDisplayEDRHeadroom(double currentEDR) {
    std::lock_guard<std::mutex> lock(impl_->cfgMtx);
    impl_->setDisplayEDRHeadroomLocked(currentEDR);
}

void D3D11Renderer::setSdrWhiteLevelNits(float nits) {
    std::lock_guard<std::mutex> lock(impl_->cfgMtx);
    impl_->sdrWhiteNits = nits > 0 ? nits : 80.0f;
}

void D3D11Renderer::setSDRBoostEnabled(bool enabled) {
    {
        std::lock_guard<std::mutex> lock(impl_->cfgMtx);
        if (impl_->sdrBoostRequested == enabled) return;
        impl_->sdrBoostRequested = enabled;
    }
    impl_->applyOutputMode();
}
bool D3D11Renderer::sdrBoostEnabled() {
    std::lock_guard<std::mutex> lock(impl_->cfgMtx);
    return impl_->sdrBoostRequested;
}
bool D3D11Renderer::displayEdrCapable() {
    std::lock_guard<std::mutex> lock(impl_->cfgMtx);
    return impl_->displayEdrCapable;
}
double D3D11Renderer::displayEDRHeadroom() {
    std::lock_guard<std::mutex> lock(impl_->cfgMtx);
    return std::max(impl_->displayCurrentEDR, 1.0);
}

void D3D11Renderer::setSampleAspect(float sar) {
    std::lock_guard<std::mutex> lock(impl_->cfgMtx);
    impl_->sampleAspect = (sar > 0.01f && sar < 100.0f) ? sar : 1.0f;
}

void D3D11Renderer::setDoviIPT(bool on) {
    Impl &m = *impl_;
    {
        std::lock_guard<std::mutex> lock(m.cfgMtx);
        m.doviIPT.store(on);
        if (on) {
            m.hasHdr = 1;
            m.transfer = 1;
            m.primaries = 1; // BT.2020
            m.sourceGamut = kSPGamutBT2020;
        }
    }
    m.applyOutputMode();
}

void D3D11Renderer::queueDoviReshapeFloats(const float *data, int64_t ptsUs) {
    if (!data) return;
    std::lock_guard<std::mutex> lock(impl_->doviMtx);
    impl_->doviQueue.queue(data, ptsUs);
}

void D3D11Renderer::bindDoviReshape(int64_t ptsUs, int64_t frameIntervalUs) {
    Impl &m = *impl_;
    std::lock_guard<std::mutex> lock(m.doviMtx);
    const int64_t bound = m.doviQueue.bind(ptsUs, frameIntervalUs);
    if (bound != INT64_MIN && spDebug() && !m.dbgDoviBindLogged) {
        m.dbgDoviBindLogged = true;
        std::fprintf(stderr, "[c%u][DoVi] per-frame bind: frame=%.3fs RPU=%.3fs (delta=%.1fms)\n", m.logId,
                     ptsUs / 1e6, bound / 1e6, (ptsUs - bound) / 1000.0);
    }
}

void D3D11Renderer::clearDoviReshapeQueue() {
    std::lock_guard<std::mutex> lock(impl_->doviMtx);
    impl_->doviQueue.clear();
}

void D3D11Renderer::resetDoviSessionState() {
    std::lock_guard<std::mutex> lock(impl_->doviMtx);
    impl_->doviQueue.reset();
}

void D3D11Renderer::setSubtitleTexture(ID3D11ShaderResourceView *texture) {
    std::lock_guard<std::mutex> lock(impl_->submitMtx);
    impl_->subtitleTexture = texture;
}
void D3D11Renderer::setSubtitleRect(RenderRect rect) {
    std::lock_guard<std::mutex> lock(impl_->submitMtx);
    impl_->subtitleRect = rect;
}

void D3D11Renderer::setAspectMode(int mode) {
    std::lock_guard<std::mutex> lock(impl_->cfgMtx);
    impl_->aspectMode = mode;
}
void D3D11Renderer::setForcedAspect(float ratio) {
    std::lock_guard<std::mutex> lock(impl_->cfgMtx);
    impl_->forcedAspect = (ratio > 0.1f && ratio < 10.0f) ? ratio : 0.0f;
}
void D3D11Renderer::setCropAspect(float ratio) {
    std::lock_guard<std::mutex> lock(impl_->cfgMtx);
    impl_->cropAspect = (ratio > 0.1f && ratio < 10.0f) ? ratio : 0.0f;
}
void D3D11Renderer::setRotation(int degrees) {
    std::lock_guard<std::mutex> lock(impl_->cfgMtx);
    impl_->rotation = ((degrees % 360) + 360) % 360 / 90;
}
void D3D11Renderer::setMirror(int mirror) {
    std::lock_guard<std::mutex> lock(impl_->cfgMtx);
    impl_->mirror = mirror;
}
// Geometry is media-session state; the colour adjustments are not.
void D3D11Renderer::resetPictureTransform() {
    std::lock_guard<std::mutex> lock(impl_->cfgMtx);
    impl_->forcedAspect = 0.0f;
    impl_->cropAspect = 0.0f;
    impl_->rotation = 0;
    impl_->mirror = 0;
}
void D3D11Renderer::setBrightness(float v) {
    std::lock_guard<std::mutex> lock(impl_->cfgMtx);
    impl_->brightness = v;
}
void D3D11Renderer::setContrast(float v) {
    std::lock_guard<std::mutex> lock(impl_->cfgMtx);
    impl_->contrast = v;
}
void D3D11Renderer::setSaturation(float v) {
    std::lock_guard<std::mutex> lock(impl_->cfgMtx);
    impl_->saturation = v;
}
void D3D11Renderer::setGamma(float v) {
    std::lock_guard<std::mutex> lock(impl_->cfgMtx);
    impl_->gamma = v;
}

} // namespace sp
