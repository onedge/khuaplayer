// FFmpeg video decoding for the Windows player core, in software or on the
// GPU through D3D11VA. The decode loop is the Mac SPFFmpegDecoder's (send and
// receive, the pending queue, pts synthesis, catch-up and drain); the
// hardware backend reuses it instead of porting the VideoToolbox decoder,
// since FFmpeg's hwaccels return frames in presentation order.
//
// Outputs are VideoFrameRefs over AVFrames the renderer accepts as they are:
//   - D3D11VA: AV_PIX_FMT_D3D11 slices of a shader-readable texture array on
//     the shared device.
//   - Software: NV12, P010, yuv420p, yuvj420p and yuv420p10 pass through
//     without a copy (AV1 comes from libdav1d as yuv420p/yuv420p10, which is
//     what the Mac's direct dav1d output achieves); other formats, and any
//     frame with an output size hint, are converted to NV12/P010.
// The stream's colour description wins over the frame's; the resolved values
// are written into the AVFrame, where the renderer reads them.
#pragma once

#include "Player/SPVideoDecoding.hpp"

#include <memory>

namespace sp {

class D3D11Device;

// lastError() of a D3D11VA decoder whose stream turned out, after setup, to
// need something the GPU cannot give (FFmpeg refused D3D11 output). The core
// treats it as "reopen in software", like a failed setup. AVERROR(ENOSYS).
inline constexpr int kHardwareUnavailableError = -40;

struct FFmpegVideoDecoderOptions {
    // Set: decode with D3D11VA on this device (setup fails for streams it
    // cannot decode). Null, or a WARP device: software.
    std::shared_ptr<D3D11Device> device;

    // Frames of a hardware session the caller holds at once (the frame queue
    // and the renderer's current and pending frame). The fixed-size surface
    // pool gets this many on top of what the decoder needs, plus a margin.
    int heldHardwareFrames = 8;

    // Single-frame race: slice threads only, no AV1 frame delay.
    bool singleFrameMode = false;
    // A low-memory preview: one AV1 frame context and no memory claim.
    bool previewMode = false;
    // Software only: when both exceed one, frames are scaled to this size
    // (square pixels; the caller accounts for the sample aspect ratio).
    int outputWidthHint = 0;
    int outputHeightHint = 0;
    // The YCbCr matrix (AVColorSpace) for converting RGB sources; it must
    // match the matrix given to the renderer. YUV sources ignore it.
    int rgbSourceMatrix = 2; // AVCOL_SPC_BT709 (the header stays FFmpeg-free)
};

class FFmpegVideoDecoder final : public VideoDecoding {
public:
    explicit FFmpegVideoDecoder(FFmpegVideoDecoderOptions options = {});
    ~FFmpegVideoDecoder() override;

    FFmpegVideoDecoder(const FFmpegVideoDecoder &) = delete;
    FFmpegVideoDecoder &operator=(const FFmpegVideoDecoder &) = delete;

    int setup(const AVCodecParameters *par, int64_t timeBaseNum, int64_t timeBaseDen) override;
    DecodedVideoOutput decodePacket(const AVPacket *pkt) override;
    void flush() override;
    void shutdown() override;
    void setCatchUpTargetUs(int64_t targetUs) override;
    void setInterpolationScanEnabled(bool enabled, const AVCodecParameters *par) override;
    bool isHardwareDecoding() const override;
    VideoDecodingBackend backend() const override;
    std::string decoderName() const override;
    int lastError() const override;
    void setLogId(unsigned logId) override;

    // Whether the hardware decoder of `device` can take this stream: codec,
    // profile, bit depth, 4:2:0 and size. setup() checks this first.
    static bool hardwareSupports(const D3D11Device &device, const AVCodecParameters *par);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace sp
