#include "SPFFmpegVideoDecoder.hpp"

#include "Player/SPAVFrameRef.hpp"
#include "SPAv1CatchUpSkip.hpp"
#include "SPColorMetadata.hpp"
#include "SPD3D11Device.hpp"
#include "SPRuntimeGates.hpp"
#include "SPSoftwareDecodePolicy.hpp"

#include <windows.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace sp {

static_assert(kHardwareUnavailableError == AVERROR(ENOSYS));

namespace {

// ---- AV1 frame-context memory budget (process-wide, as on the Mac) --------

std::mutex gAv1ClaimMtx;
std::atomic<int64_t> gAv1ClaimedBytes{0};

int64_t av1WallBytes() {
    static const int64_t v = [] {
        MEMORYSTATUSEX status = {sizeof(status)};
        const int64_t physical = GlobalMemoryStatusEx(&status) ? (int64_t)status.ullTotalPhys : 0;
        return spswdec::av1BudgetWallBytes(physical);
    }();
    return v;
}

spswdec::Av1DelayPlan av1Claim(spswdec::Av1DelayInput in, int64_t *claimSlot) {
    std::lock_guard<std::mutex> lock(gAv1ClaimMtx);
    if (*claimSlot > 0) gAv1ClaimedBytes.fetch_sub(*claimSlot);
    *claimSlot = 0;
    in.wallBytes = av1WallBytes();
    in.claimedBytes = gAv1ClaimedBytes.load();
    const spswdec::Av1DelayPlan plan = spswdec::av1FrameDelayPlan(in);
    if (plan.claimBytes > 0) {
        *claimSlot = plan.claimBytes;
        gAv1ClaimedBytes.fetch_add(plan.claimBytes);
    }
    return plan;
}

void av1Release(int64_t *claimSlot) {
    std::lock_guard<std::mutex> lock(gAv1ClaimMtx);
    if (*claimSlot > 0) gAv1ClaimedBytes.fetch_sub(*claimSlot);
    *claimSlot = 0;
}

int parBitDepth(const AVCodecParameters *par) {
    const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get((AVPixelFormat)par->format);
    if (pd && pd->nb_components > 0) return pd->comp[0].depth;
    return par->bits_per_raw_sample > 0 ? par->bits_per_raw_sample : 0;
}

bool parIsProbably10Bit(const AVCodecParameters *par) {
    const int depth = parBitDepth(par);
    return depth <= 0 ? true : depth > 8;
}

bool isYuvjFormat(int format) {
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get((AVPixelFormat)format);
    return d && std::strncmp(d->name, "yuvj", 4) == 0;
}

// Formats the renderer samples as they are.
bool rendererTakesFormat(int format) {
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

// ---- D3D11VA capability ----------------------------------------------------

// One decoder the GPU must offer: any of up to two profile GUIDs, with the
// output format.
struct HardwareRequirement {
    const GUID *guids[2] = {};
    bool tenBit = false;
};

struct HardwareProfile {
    // All must be met. A stream whose bit depth is unknown before decoding
    // needs both the 8-bit and the 10-bit decoder, since either may be used.
    HardwareRequirement required[2];
    int count = 0;
    // Plain H.264 Baseline: decoded with the Constrained Baseline profile.
    bool allowProfileMismatch = false;

    void add(const GUID *guid, bool tenBit, const GUID *alternative = nullptr) {
        required[count++] = {{guid, alternative}, tenBit};
    }
};

bool profileIn(int profile, std::initializer_list<int> allowed) {
    if (profile == AV_PROFILE_UNKNOWN) return true;
    for (int p : allowed)
        if (profile == p) return true;
    return false;
}

// The decoders FFmpeg's D3D11VA hwaccel would use for this stream, with the
// profiles it accepts (libavcodec/dxva2.c), or false when it would refuse the
// stream: setup must fail then, so the core falls back to software.
bool hardwareProfileFor(const AVCodecParameters *par, HardwareProfile *out) {
    const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get((AVPixelFormat)par->format);
    if (pd && (pd->log2_chroma_w != 1 || pd->log2_chroma_h != 1 || (pd->flags & AV_PIX_FMT_FLAG_RGB)))
        return false; // 4:2:0 only
    const int depth = parBitDepth(par);
    if (depth > 10) return false;
    const bool known = depth > 0;
    const bool tenBit = depth == 10;
    auto addByDepth = [&](const GUID *eight, const GUID *ten, bool forceTen) {
        if (forceTen || tenBit) {
            out->add(ten, true);
        } else {
            out->add(eight, false);
            if (!known) out->add(ten, true);
        }
    };
    switch (par->codec_id) {
    case AV_CODEC_ID_H264:
        // No High 10, 4:2:2 or 4:4:4. Plain Baseline is played as
        // Constrained Baseline, as other players do; FMO and ASO are rare.
        if (tenBit) return false;
        if (par->profile == AV_PROFILE_H264_BASELINE) {
            out->allowProfileMismatch = true;
        } else if (!profileIn(par->profile, {AV_PROFILE_H264_CONSTRAINED_BASELINE, AV_PROFILE_H264_MAIN,
                                             AV_PROFILE_H264_HIGH})) {
            return false;
        }
        out->add(&D3D11_DECODER_PROFILE_H264_VLD_NOFGT, false);
        return true;
    case AV_CODEC_ID_HEVC:
        if (!profileIn(par->profile, {AV_PROFILE_HEVC_MAIN, AV_PROFILE_HEVC_MAIN_10})) return false;
        addByDepth(&D3D11_DECODER_PROFILE_HEVC_VLD_MAIN, &D3D11_DECODER_PROFILE_HEVC_VLD_MAIN10,
                   par->profile == AV_PROFILE_HEVC_MAIN_10);
        return true;
    case AV_CODEC_ID_VP9:
        if (!profileIn(par->profile, {AV_PROFILE_VP9_0, AV_PROFILE_VP9_2})) return false;
        addByDepth(&D3D11_DECODER_PROFILE_VP9_VLD_PROFILE0, &D3D11_DECODER_PROFILE_VP9_VLD_10BIT_PROFILE2,
                   par->profile == AV_PROFILE_VP9_2);
        return true;
    case AV_CODEC_ID_AV1:
        if (!profileIn(par->profile, {AV_PROFILE_AV1_MAIN})) return false;
        out->add(&D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, tenBit);
        if (!known) out->add(&D3D11_DECODER_PROFILE_AV1_VLD_PROFILE0, true);
        return true;
    case AV_CODEC_ID_MPEG2VIDEO:
        if (tenBit || !profileIn(par->profile, {AV_PROFILE_MPEG2_SIMPLE, AV_PROFILE_MPEG2_MAIN})) return false;
        out->add(&D3D11_DECODER_PROFILE_MPEG2and1_VLD, false, &D3D11_DECODER_PROFILE_MPEG2_VLD);
        return true;
    case AV_CODEC_ID_VC1:
    case AV_CODEC_ID_WMV3:
        if (tenBit) return false;
        out->add(&D3D11_DECODER_PROFILE_VC1_D2010, false, &D3D11_DECODER_PROFILE_VC1_VLD);
        return true;
    default:
        return false;
    }
}

// ---- The shared device for FFmpeg ------------------------------------------

void lockDevice(void *ctx) { static_cast<D3D11Device *>(ctx)->lock(); }
void unlockDevice(void *ctx) { static_cast<D3D11Device *>(ctx)->unlock(); }

void freeDeviceContext(AVHWDeviceContext *ctx) { delete static_cast<std::shared_ptr<D3D11Device> *>(ctx->user_opaque); }

// FFmpeg's D3D11VA device context over the shared device and its lock. It
// keeps the device alive for as long as frames from it exist.
AVBufferRef *makeHardwareDevice(const std::shared_ptr<D3D11Device> &device) {
    AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    if (!ref) return nullptr;
    auto *hw = reinterpret_cast<AVHWDeviceContext *>(ref->data);
    auto *d3d = static_cast<AVD3D11VADeviceContext *>(hw->hwctx);
    d3d->device = device->device();
    d3d->device->AddRef();
    d3d->lock = lockDevice;
    d3d->unlock = unlockDevice;
    d3d->lock_ctx = device.get();
    hw->user_opaque = new std::shared_ptr<D3D11Device>(device);
    hw->free = freeDeviceContext;
    if (av_hwdevice_ctx_init(ref) < 0) av_buffer_unref(&ref);
    return ref;
}

struct PendingFrame {
    VideoFrameRef frame;
    int64_t ptsUs = AV_NOPTS_VALUE;
    DecodedVideoScanVerdict scanVerdict = DecodedVideoScanVerdict::Unknown;
    bool scanCovered = false;
};

// Software decoding keeps up to 8 outputs; each pending hardware output holds
// a surface of the fixed pool, so it keeps 2.
constexpr size_t kSoftwarePendingLimit = 8;
constexpr size_t kHardwarePendingLimit = 2;
// Surfaces the pool gets beyond the decoder's references and the caller's.
constexpr int kHardwarePoolMargin = 4;
constexpr int kMaxHardwarePool = 64;

} // namespace

#define SPLOG(fmt, ...) std::fprintf(stderr, "[c%u]" fmt "\n", logId, ##__VA_ARGS__)

struct FFmpegVideoDecoder::Impl {
    FFmpegVideoDecoderOptions options;
    unsigned logId = 0;

    AVCodecContext *ctx = nullptr;
    AVFrame *frame = nullptr;
    AVBufferRef *hwDevice = nullptr;
    bool hardware = false;
    // getFormat could not give D3D11 output after setup accepted the stream.
    bool hardwareRefused = false;

    SwsContext *sws = nullptr;
    int swsSrcFormat = AV_PIX_FMT_NONE;
    int swsSrcWidth = 0, swsSrcHeight = 0;
    bool swsFullRange = false;

    int streamPrimaries = AVCOL_PRI_UNSPECIFIED;
    int streamTrc = AVCOL_TRC_UNSPECIFIED;
    int streamSpace = AVCOL_SPC_UNSPECIFIED;
    int streamRange = AVCOL_RANGE_UNSPECIFIED;
    bool streamYuvj = false;

    int64_t tbNum = 1, tbDen = 1;
    std::deque<PendingFrame> pending;
    bool eofFlushed = false;
    int lastErr = 0;
    std::atomic<int64_t> catchUpTargetUs{-1};
    bool skipModeOn = false;
    int64_t synthNextPtsUs = 0;

    int64_t av1ClaimBytes = 0;
    spav1::SequenceHeader av1Seq;
    bool av1SkipArmed = false;
    bool av1KeySelfChecked = false;
    AVPacket *av1SubPkt = nullptr;
    std::atomic<int64_t> av1DroppedFrames{0};

    size_t pendingLimit() const { return hardware ? kHardwarePendingLimit : kSoftwarePendingLimit; }

    AVRational timeBase() const { return AVRational{(int)tbNum, (int)tbDen}; }

    // ---- Setup -------------------------------------------------------------

    int setup(const AVCodecParameters *par, int64_t num, int64_t den) {
        shutdown();
        eofFlushed = false;
        synthNextPtsUs = 0;
        skipModeOn = false;
        catchUpTargetUs.store(-1);
        av1Seq = spav1::SequenceHeader();
        av1SkipArmed = false;
        av1KeySelfChecked = false;
        av1DroppedFrames = 0;
        lastErr = 0;
        hardwareRefused = false;

        tbNum = num > 0 ? num : 1;
        tbDen = den > 0 ? den : 1;
        streamPrimaries = par->color_primaries;
        streamTrc = par->color_trc;
        streamSpace = par->color_space;
        streamRange = par->color_range;
        streamYuvj = isYuvjFormat(par->format);

        hardware = options.device && !options.device->isWarp();
        const bool isAv1 = par->codec_id == AV_CODEC_ID_AV1;
        const AVCodec *codec = nullptr;
        if (hardware) {
            if (!hardwareSupports(*options.device, par)) {
                if (spDebug()) SPLOG("[D3D11VA] %s profile %d is not supported by the GPU",
                                     avcodec_get_name(par->codec_id), par->profile);
                return -1;
            }
            // FFmpeg's AV1 hwaccel lives in its native decoder, not libdav1d.
            codec = isAv1 ? avcodec_find_decoder_by_name("av1") : avcodec_find_decoder(par->codec_id);
        } else {
            if (isAv1) codec = avcodec_find_decoder_by_name("libdav1d");
            if (!codec) codec = avcodec_find_decoder(par->codec_id);
        }
        if (!codec) return -1;

        ctx = avcodec_alloc_context3(codec);
        if (!ctx) return -2;
        if (avcodec_parameters_to_context(ctx, par) < 0) return -3;
        ctx->pkt_timebase = timeBase();

        if (hardware) {
            hwDevice = makeHardwareDevice(options.device);
            if (!hwDevice) return -2;
            ctx->hw_device_ctx = av_buffer_ref(hwDevice);
            ctx->opaque = this;
            ctx->get_format = getFormat;
            ctx->thread_count = 1;
            HardwareProfile profile;
            if (hardwareProfileFor(par, &profile) && profile.allowProfileMismatch)
                ctx->hwaccel_flags |= AV_HWACCEL_FLAG_ALLOW_PROFILE_MISMATCH;
        } else if (options.singleFrameMode) {
            ctx->thread_count = 4;
            ctx->thread_type = FF_THREAD_SLICE;
        } else {
            ctx->thread_count = 0;
            ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        }
        if (const char *threads = getenv("SP_SW_THREADS"); threads && !hardware) ctx->thread_count = atoi(threads);

        bool lowDelay = true;
        spswdec::Av1DelayPlan av1Plan;
        if (isAv1 && !hardware) {
            spswdec::Av1DelayInput in;
            in.width = par->width;
            in.height = par->height;
            in.tenBit = parIsProbably10Bit(par);
            in.singleFrameMode = options.singleFrameMode;
            in.previewMode = options.previewMode;
            av1Plan = av1Claim(in, &av1ClaimBytes);
            if (av1Plan.maxFrameDelay > 0) lowDelay = false;
            if (!lowDelay && av_opt_set_int(ctx->priv_data, "max_frame_delay", av1Plan.maxFrameDelay, 0) < 0) {
                lowDelay = true;
                av1Plan.maxFrameDelay = 0;
            }
        }
        if (const char *v = getenv("SP_SW_LOWDELAY")) lowDelay = atoi(v) != 0;
        if (!isAv1 || hardware || lowDelay || av1Plan.maxFrameDelay <= 0) av1Release(&av1ClaimBytes);
        if (isAv1 && !hardware && spDebug())
            SPLOG("[SW] AV1 frame delay=%d (%lld MB per context, %lld MB claimed of %lld MB)",
                  lowDelay ? 1 : av1Plan.maxFrameDelay, (long long)(av1Plan.perContextBytes >> 20),
                  (long long)(gAv1ClaimedBytes.load() >> 20), (long long)(av1WallBytes() >> 20));

        // The trimming is proven on libdav1d only; FFmpeg's native AV1 decoder
        // behind D3D11VA keeps every frame while catching up.
        if (isAv1 && !hardware && !options.singleFrameMode) {
            av1SkipArmed = getenv("SP_NO_NRDROP") == nullptr;
            if (av1SkipArmed && par->extradata && par->extradata_size > 4)
                spav1::parseAv1CodecConfigRecord(par->extradata, (size_t)par->extradata_size, &av1Seq);
        }

        if (lowDelay && !options.singleFrameMode) {
            switch (par->codec_id) {
            case AV_CODEC_ID_MPEG1VIDEO:
            case AV_CODEC_ID_MPEG2VIDEO:
            case AV_CODEC_ID_MPEG4:
            case AV_CODEC_ID_VC1:
            case AV_CODEC_ID_WMV3:
            case AV_CODEC_ID_VC1IMAGE:
            case AV_CODEC_ID_WMV3IMAGE:
                lowDelay = false;
                if (!hardware) ctx->thread_type = FF_THREAD_SLICE;
                break;
            default:
                break;
            }
        }
        if (lowDelay) ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

        if (avcodec_open2(ctx, codec, nullptr) < 0) return -4;
        frame = av_frame_alloc();
        if (!frame) return -5;
        if (spDebug())
            SPLOG("[%s] %s %dx%d profile %d", hardware ? "D3D11VA" : "SW", codec->name, par->width, par->height,
                  par->profile);
        return 0;
    }

    // Decoder surfaces for D3D11VA: a shader-readable texture array sized for
    // the decoder's references plus the frames the player holds. Anything but
    // D3D11 output is refused rather than silently decoded in software.
    static AVPixelFormat getFormat(AVCodecContext *c, const AVPixelFormat *formats) {
        auto *self = static_cast<Impl *>(c->opaque);
        const unsigned logId = self->logId;
        for (const AVPixelFormat *p = formats; *p != AV_PIX_FMT_NONE; p++) {
            if (*p != AV_PIX_FMT_D3D11) continue;
            AVBufferRef *framesRef = nullptr;
            if (avcodec_get_hw_frames_parameters(c, c->hw_device_ctx, AV_PIX_FMT_D3D11, &framesRef) < 0) break;
            auto *frames = reinterpret_cast<AVHWFramesContext *>(framesRef->data);
            if (frames->sw_format != AV_PIX_FMT_NV12 && frames->sw_format != AV_PIX_FMT_P010) {
                av_buffer_unref(&framesRef);
                break;
            }
            auto *d3d = static_cast<AVD3D11VAFramesContext *>(frames->hwctx);
            // Drivers only make NV12/P010 arrays shader-readable together with
            // the decoder flag, which FFmpeg sets.
            d3d->BindFlags |= D3D11_BIND_DECODER | D3D11_BIND_SHADER_RESOURCE;
            frames->initial_pool_size += self->options.heldHardwareFrames + (int)kHardwarePendingLimit +
                                         kHardwarePoolMargin;
            // FFmpeg's D3D11 pool holds at most 64 surfaces.
            if (frames->initial_pool_size > kMaxHardwarePool) {
                if (spDebug()) SPLOG("[D3D11VA] pool of %d surfaces capped at %d", frames->initial_pool_size,
                                     kMaxHardwarePool);
                frames->initial_pool_size = kMaxHardwarePool;
            }
            if (av_hwframe_ctx_init(framesRef) < 0) {
                if (spDebug()) SPLOG("[D3D11VA] cannot create %d surfaces", frames->initial_pool_size);
                av_buffer_unref(&framesRef);
                break;
            }
            if (spDebug())
                SPLOG("[D3D11VA] %dx%d %s pool of %d surfaces", frames->width, frames->height,
                      av_get_pix_fmt_name(frames->sw_format), frames->initial_pool_size);
            av_buffer_unref(&c->hw_frames_ctx);
            c->hw_frames_ctx = framesRef;
            return AV_PIX_FMT_D3D11;
        }
        self->hardwareRefused = true;
        return AV_PIX_FMT_NONE;
    }

    // ---- Decoding ----------------------------------------------------------

    void applySkipMode(bool on) {
        if (!ctx || skipModeOn == on) return;
        skipModeOn = on;
        ctx->skip_frame = on ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
    }

    const AVPacket *av1CatchUpPacketFor(const AVPacket *pkt) {
        const spav1::ScanResult scan = spav1::scanTemporalUnit(pkt->data, (size_t)pkt->size, &av1Seq);
        if (!av1KeySelfChecked && (pkt->flags & AV_PKT_FLAG_KEY)) {
            av1KeySelfChecked = true;
            if (!scan.sawKeyFrame) {
                av1SkipArmed = false;
                if (spDebug()) SPLOG("[SW] AV1 catch-up trimming off: the key frame self-check failed");
                return pkt;
            }
        }
        if (scan.action == spav1::Action::DropPacket) {
            av1DroppedFrames += scan.droppedFrames;
            return nullptr;
        }
        if (scan.action != spav1::Action::TruncateToPrefix) return pkt;
        if (!av1SubPkt) {
            av1SubPkt = av_packet_alloc();
            if (!av1SubPkt) {
                av1SkipArmed = false;
                return pkt;
            }
        }
        av_packet_unref(av1SubPkt);
        if (av_packet_ref(av1SubPkt, pkt) < 0) return pkt;
        av1SubPkt->size = (int)scan.keepBytes;
        av1DroppedFrames += scan.droppedFrames;
        return av1SubPkt;
    }

    DecodedVideoOutput decodePacket(const AVPacket *pkt) {
        DecodedVideoOutput out = decodePacketInner(pkt);
        // Tell the core to reopen the stream in software.
        if (hardwareRefused && lastErr != 0) lastErr = kHardwareUnavailableError;
        return out;
    }

    DecodedVideoOutput decodePacketInner(const AVPacket *pkt) {
        if (!ctx || !frame) return {};
        lastErr = 0;
        const int64_t cuTarget = catchUpTargetUs.load();

        if (!pkt) {
            if (pending.empty()) {
                if (eofFlushed) return {};
                eofFlushed = true;
                applySkipMode(false);
                const int sendRet = avcodec_send_packet(ctx, nullptr);
                if (spDebug()) SPLOG("[%s] drain: send(NULL)=%d", hardware ? "D3D11VA" : "SW", sendRet);
                receiveAll(cuTarget, true);
            }
            return popPending();
        }

        const AVPacket *sendPkt = pkt;
        if (cuTarget > 0 && pkt->pts != AV_NOPTS_VALUE) {
            const int64_t pktUs = av_rescale_q(pkt->pts, timeBase(), AV_TIME_BASE_Q);
            const bool inSkipZone = pktUs + 80000 < cuTarget;
            applySkipMode(inSkipZone);
            if (inSkipZone && av1SkipArmed && pkt->data && pkt->size > 0) {
                sendPkt = av1CatchUpPacketFor(pkt);
                if (!sendPkt) return popPending();
            }
        } else {
            applySkipMode(false);
        }

        int sendRet = avcodec_send_packet(ctx, sendPkt);
        // The Mac retries once after receiving. Here the decoder is drained
        // past the pending limit if it has to be, so no packet is ever lost.
        for (int guard = 0; sendRet == AVERROR(EAGAIN) && guard < 16; guard++) {
            receiveAll(cuTarget, false, guard > 0);
            sendRet = avcodec_send_packet(ctx, sendPkt);
        }
        if (sendRet == AVERROR(EAGAIN)) {
            // The decoder would not take the packet: it is lost, so say so.
            if (spDebug()) SPLOG("[%s] packet refused after draining", hardware ? "D3D11VA" : "SW");
            lastErr = sendRet;
            return popPending();
        }
        if (sendRet < 0 && sendRet != AVERROR_EOF) {
            lastErr = sendRet;
            return popPending();
        }
        receiveAll(cuTarget, false);
        return popPending();
    }

    void receiveAll(int64_t cuTarget, bool waitForDrain, bool ignoreLimit = false) {
        int eagainRetries = 0;
        for (;;) {
            const int ret = avcodec_receive_frame(ctx, frame);
            if (ret == 0) {
                eagainRetries = 0;
                const int64_t ptsUs = frame->best_effort_timestamp != AV_NOPTS_VALUE
                                          ? av_rescale_q(frame->best_effort_timestamp, timeBase(), AV_TIME_BASE_Q)
                                          : synthNextPtsUs;
                const int64_t durUs =
                    frame->duration > 0 ? av_rescale_q(frame->duration, timeBase(), AV_TIME_BASE_Q) : 40000;
                synthNextPtsUs = ptsUs + (durUs > 0 ? durUs : 40000);

                if (cuTarget > 0 && ptsUs + 40000 < cuTarget) {
                    av_frame_unref(frame);
                    continue;
                }
                const bool interlaced = (frame->flags & AV_FRAME_FLAG_INTERLACED) != 0;
                VideoFrameRef out = wrapFrame(frame);
                av_frame_unref(frame);
                if (!out) continue;
                pending.push_back({out, ptsUs,
                                   interlaced ? DecodedVideoScanVerdict::Interlaced
                                              : DecodedVideoScanVerdict::Progressive,
                                   true});
                if (!waitForDrain && !ignoreLimit && pending.size() >= pendingLimit()) break;
            } else if (ret == AVERROR(EAGAIN)) {
                // Frame threads may still be finishing at end of stream.
                if (waitForDrain && ++eagainRetries < 250) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    continue;
                }
                break;
            } else {
                if (ret != AVERROR_EOF) lastErr = ret;
                break;
            }
        }
    }

    DecodedVideoOutput popPending() {
        if (pending.empty()) return {};
        const PendingFrame p = pending.front();
        pending.pop_front();
        return {p.frame, p.ptsUs, p.scanVerdict, p.scanCovered};
    }

    // The stream's colour description wins; only unspecified fields come
    // from the frame. The renderer reads the result from the AVFrame.
    void resolveColour(AVFrame *f) const {
        f->color_primaries = (AVColorPrimaries)spAuthoritativeColorValue(streamPrimaries, f->color_primaries,
                                                                         AVCOL_PRI_UNSPECIFIED);
        f->color_trc = (AVColorTransferCharacteristic)spAuthoritativeColorValue(streamTrc, f->color_trc,
                                                                                AVCOL_TRC_UNSPECIFIED);
        f->colorspace = (AVColorSpace)spAuthoritativeColorValue(streamSpace, f->colorspace, AVCOL_SPC_UNSPECIFIED);
        int range = spAuthoritativeColorValue(streamRange, f->color_range, AVCOL_RANGE_UNSPECIFIED);
        if (range == AVCOL_RANGE_UNSPECIFIED) range = streamYuvj || isYuvjFormat(f->format) ? AVCOL_RANGE_JPEG
                                                                                             : AVCOL_RANGE_MPEG;
        f->color_range = (AVColorRange)range;
    }

    VideoFrameRef wrapFrame(AVFrame *f) {
        if (f->format == AV_PIX_FMT_D3D11) {
            resolveColour(f);
            return spFrameFromAVFrame(f);
        }
        if (hardware) { // a software frame from a hardware session: refused in getFormat
            lastErr = AVERROR(EINVAL);
            return {};
        }
        const bool resize = options.outputWidthHint > 1 && options.outputHeightHint > 1;
        if (!resize && rendererTakesFormat(f->format)) {
            resolveColour(f);
            return spFrameFromAVFrame(f);
        }
        return convertFrame(f, resize);
    }

    // Formats the renderer does not take (4:2:2, 4:4:4, RGB, ...) and resized
    // thumbnails: swscale to NV12 or P010, as the Mac does for everything.
    VideoFrameRef convertFrame(AVFrame *f, bool resize) {
        const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get((AVPixelFormat)f->format);
        if (!desc || f->width <= 0 || f->height <= 0) {
            lastErr = AVERROR(EINVAL);
            return {};
        }
        resolveColour(f);
        const bool tenBit = desc->comp[0].depth > 8;
        const bool fullRange = f->color_range == AVCOL_RANGE_JPEG;
        const int ow = resize ? std::max(options.outputWidthHint & ~1, 2) : f->width;
        const int oh = resize ? std::max(options.outputHeightHint & ~1, 2) : f->height;
        const AVPixelFormat dstFormat = tenBit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
        if (!sws || f->format != swsSrcFormat || f->width != swsSrcWidth || f->height != swsSrcHeight ||
            fullRange != swsFullRange) {
            sws_freeContext(sws);
            sws = sws_getContext(f->width, f->height, (AVPixelFormat)f->format, ow, oh, dstFormat,
                                 resize ? SWS_AREA : SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!sws) {
                lastErr = AVERROR(EINVAL);
                return {};
            }
            configureSwsColour(desc, fullRange);
            swsSrcFormat = f->format;
            swsSrcWidth = f->width;
            swsSrcHeight = f->height;
            swsFullRange = fullRange;
            if (spDebug())
                SPLOG("[SW] converting %s %dx%d to %s %dx%d", desc->name, f->width, f->height,
                      av_get_pix_fmt_name(dstFormat), ow, oh);
        }
        AVFrame *dst = av_frame_alloc();
        if (!dst) {
            lastErr = AVERROR(ENOMEM);
            return {};
        }
        dst->format = dstFormat;
        dst->width = ow;
        dst->height = oh;
        if (av_frame_get_buffer(dst, 0) < 0) {
            av_frame_free(&dst);
            lastErr = AVERROR(ENOMEM);
            return {};
        }
        sws_scale(sws, f->data, f->linesize, 0, f->height, dst->data, dst->linesize);
        av_frame_copy_props(dst, f);
        // RGB sources become YCbCr with the configured matrix and keep their range.
        if (desc->flags & AV_PIX_FMT_FLAG_RGB) dst->colorspace = (AVColorSpace)options.rgbSourceMatrix;
        VideoFrameRef out = spFrameFromAVFrame(dst);
        av_frame_free(&dst);
        if (!out) lastErr = AVERROR(ENOMEM);
        return out;
    }

    void configureSwsColour(const AVPixFmtDescriptor *desc, bool fullRange) {
        const int range = fullRange ? 1 : 0;
        const bool rgbSource = (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0;
        int cs = SWS_CS_ITU709;
        if (rgbSource) {
            switch (options.rgbSourceMatrix) {
            case AVCOL_SPC_BT470BG:
            case AVCOL_SPC_SMPTE170M: cs = SWS_CS_ITU601; break;
            case AVCOL_SPC_SMPTE240M: cs = SWS_CS_SMPTE240M; break;
            case AVCOL_SPC_BT2020_NCL:
            case AVCOL_SPC_BT2020_CL: cs = SWS_CS_BT2020; break;
            default: cs = SWS_CS_ITU709; break;
            }
        }
        const int *coefs = sws_getCoefficients(cs);
        int *invTable = nullptr, *table = nullptr;
        int srcRange = 0, dstRange = 0, brightness = 0, contrast = 0, saturation = 0;
        if (!rgbSource && sws_getColorspaceDetails(sws, &invTable, &srcRange, &table, &dstRange, &brightness,
                                                   &contrast, &saturation) >= 0) {
            sws_setColorspaceDetails(sws, invTable, range, table, range, brightness, contrast, saturation);
        } else {
            sws_setColorspaceDetails(sws, coefs, range, coefs, range, 0, 1 << 16, 1 << 16);
        }
    }

    void releasePending() {
        for (const PendingFrame &p : pending) spFrameRelease(p.frame);
        pending.clear();
    }

    void flush() {
        if (ctx) avcodec_flush_buffers(ctx);
        eofFlushed = false;
        releasePending();
        synthNextPtsUs = 0;
    }

    void shutdown() {
        av1Release(&av1ClaimBytes);
        if (av1SubPkt) av_packet_free(&av1SubPkt);
        releasePending();
        sws_freeContext(sws);
        sws = nullptr;
        swsSrcFormat = AV_PIX_FMT_NONE;
        if (frame) av_frame_free(&frame);
        if (ctx) avcodec_free_context(&ctx);
        av_buffer_unref(&hwDevice);
    }
};

#undef SPLOG

FFmpegVideoDecoder::FFmpegVideoDecoder(FFmpegVideoDecoderOptions options) : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
}

FFmpegVideoDecoder::~FFmpegVideoDecoder() { impl_->shutdown(); }

int FFmpegVideoDecoder::setup(const AVCodecParameters *par, int64_t timeBaseNum, int64_t timeBaseDen) {
    if (!par) return -1;
    const int r = impl_->setup(par, timeBaseNum, timeBaseDen);
    if (r != 0) impl_->shutdown();
    return r;
}

DecodedVideoOutput FFmpegVideoDecoder::decodePacket(const AVPacket *pkt) { return impl_->decodePacket(pkt); }
void FFmpegVideoDecoder::flush() { impl_->flush(); }
void FFmpegVideoDecoder::shutdown() { impl_->shutdown(); }

void FFmpegVideoDecoder::setCatchUpTargetUs(int64_t targetUs) {
    Impl &m = *impl_;
    const int64_t previous = m.catchUpTargetUs.exchange(targetUs);
    if (previous <= 0 && targetUs > 0) {
        m.av1DroppedFrames = 0;
    } else if (previous > 0 && targetUs <= 0) {
        const int64_t dropped = m.av1DroppedFrames.exchange(0);
        if (dropped > 0 && spDebug())
            std::fprintf(stderr, "[c%u][SW] AV1 catch-up dropped %lld non-reference frames\n", m.logId,
                         (long long)dropped);
    }
}

void FFmpegVideoDecoder::setInterpolationScanEnabled(bool, const AVCodecParameters *) {
    // The AVFrame's field flag already travels with each output.
}

bool FFmpegVideoDecoder::isHardwareDecoding() const { return impl_->hardware && impl_->ctx; }

VideoDecodingBackend FFmpegVideoDecoder::backend() const {
    return isHardwareDecoding() ? VideoDecodingBackend::FFmpegD3D11VA : VideoDecodingBackend::FFmpegSoftware;
}

std::string FFmpegVideoDecoder::decoderName() const {
    // Localisation keys, as the Mac's decoderName; the app shows the text.
    return isHardwareDecoding() ? "renderer.decoder.d3d11va" : "renderer.decoder.ffmpegSW";
}

int FFmpegVideoDecoder::lastError() const { return impl_->lastErr; }
void FFmpegVideoDecoder::setLogId(unsigned logId) { impl_->logId = logId; }

bool FFmpegVideoDecoder::hardwareSupports(const D3D11Device &device, const AVCodecParameters *par) {
    if (!par || device.isWarp()) return false;
    HardwareProfile profile;
    if (!hardwareProfileFor(par, &profile)) return false;
    ComPtr<ID3D11VideoDevice> video;
    if (FAILED(device.device()->QueryInterface(IID_PPV_ARGS(&video)))) return false;
    for (int i = 0; i < profile.count; i++) {
        const HardwareRequirement &req = profile.required[i];
        const DXGI_FORMAT format = req.tenBit ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
        // The surface pool is a decoder target the renderer samples.
        constexpr UINT kNeeded =
            D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_DECODER_OUTPUT | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
        UINT support = 0;
        if (FAILED(device.device()->CheckFormatSupport(format, &support)) || (support & kNeeded) != kNeeded)
            return false;
        bool met = false;
        for (const GUID *guid : req.guids) {
            if (!guid) break;
            BOOL formatOk = FALSE;
            if (FAILED(video->CheckVideoDecoderFormat(guid, format, &formatOk)) || !formatOk) continue;
            D3D11_VIDEO_DECODER_DESC desc = {*guid, (UINT)std::max(par->width, 16),
                                             (UINT)std::max(par->height, 16), format};
            UINT configs = 0;
            if (SUCCEEDED(video->GetVideoDecoderConfigCount(&desc, &configs)) && configs > 0) {
                met = true;
                break;
            }
        }
        if (!met) return false;
    }
    return true;
}

} // namespace sp
