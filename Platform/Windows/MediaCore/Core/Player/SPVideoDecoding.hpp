// The video decoder contract the Windows player core is written against: the
// Mac player's SPVideoDecoding protocol (Modules/MediaCore/Bridge/
// SPVideoDecoding.h) in C++, with VideoFrameRef in place of CVPixelBufferRef.
#pragma once

#include "SPVideoFrame.hpp"

#include <cstdint>
#include <string>

struct AVCodecParameters;
struct AVPacket;

namespace sp {

// Decoder-neutral, per-output scan verdict. Unknown is deliberately
// fail-closed for interpolation. Progressive is usable only when scanCovered
// is also true; Interlaced is a negative session verdict and does not require
// progressive coverage.
enum class DecodedVideoScanVerdict : uint8_t {
    Unknown = 0,
    Progressive = 1,
    Interlaced = 2,
};

// Where the core needs to choose recovery behaviour by implementation. The
// Mac's `== VideoToolbox` checks become "is this a hardware backend".
enum class VideoDecodingBackend : uint8_t {
    FFmpegSoftware = 0,
    FFmpegD3D11VA = 1,
};

inline constexpr bool spIsHardwareBackend(VideoDecodingBackend backend) {
    return backend != VideoDecodingBackend::FFmpegSoftware;
}

// One decoded output. The frame, pts and scan metadata always travel together
// through the decoder's pending queue.
//
// Ownership: a non-empty frame is transferred to the caller at +1; the caller
// releases it exactly once (spFrameRelease) or passes that ownership on.
// Copies of the struct alias the one reference. Callers judge "no output" by
// the frame, not by ptsUs.
struct DecodedVideoOutput {
    VideoFrameRef frame;
    int64_t ptsUs = INT64_MIN;
    DecodedVideoScanVerdict scanVerdict = DecodedVideoScanVerdict::Unknown;
    bool scanCovered = false;
};

class VideoDecoding {
public:
    virtual ~VideoDecoding() = default;

    // 0 on success. A negative value from a hardware decoder means the stream
    // is not supported there, and the core falls back to software.
    virtual int setup(const AVCodecParameters *par, int64_t timeBaseNum, int64_t timeBaseDen) = 0;

    // Sends `pkt` (null to drain at end of stream) and returns the next
    // output, if any. No output with lastError() == 0 means more input is
    // needed; no output with lastError() != 0 is a decode failure.
    virtual DecodedVideoOutput decodePacket(const AVPacket *pkt) = 0;
    virtual void flush() = 0;
    virtual void shutdown() = 0;

    // Outputs before the target are decoded as cheaply as possible and not
    // returned; zero or negative ends catching up. Callable from any thread.
    virtual void setCatchUpTargetUs(int64_t targetUs) = 0;

    // The interpolation scan (Motion+) is not part of v1 on Windows; FFmpeg
    // decoders report the AVFrame's field flag per output either way.
    virtual void setInterpolationScanEnabled(bool enabled, const AVCodecParameters *par) = 0;

    virtual bool isHardwareDecoding() const = 0;
    virtual VideoDecodingBackend backend() const = 0;
    virtual std::string decoderName() const = 0;
    virtual int lastError() const = 0;

    virtual void setLogId(unsigned logId) = 0;
};

} // namespace sp
