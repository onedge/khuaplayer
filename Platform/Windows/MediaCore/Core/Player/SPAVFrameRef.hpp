// The Windows VideoFrameRef: a reference-counted handle to an FFmpeg AVFrame,
// either software planes or an AV_PIX_FMT_D3D11 texture-array slice. The
// decoders produce these and the renderer samples them; the player core only
// passes the handles around (SPVideoFrame.hpp).
#pragma once

#include "SPVideoFrame.hpp"

struct AVFrame;

namespace sp {

// Returns a new +1 handle holding its own reference to `frame`'s buffers
// (av_frame_ref), or an empty handle on failure. `frame` stays the caller's.
VideoFrameRef spFrameFromAVFrame(const AVFrame *frame);

// The frame behind `handle`, borrowed; null for an empty handle or one that
// does not hold an AVFrame.
const AVFrame *spFrameAVFrame(VideoFrameRef handle);

} // namespace sp
