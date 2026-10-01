#include "SPAVFrameRef.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
}

#include <atomic>

namespace sp {
namespace {

struct FrameBox {
    std::atomic<int> refs{1};
    AVFrame *frame = nullptr;
};

void boxRetain(void *native) { static_cast<FrameBox *>(native)->refs.fetch_add(1, std::memory_order_relaxed); }

void boxRelease(void *native) {
    auto *box = static_cast<FrameBox *>(native);
    if (box->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        av_frame_free(&box->frame);
        delete box;
    }
}

// What the frame occupies, for the core's memory budgets: the plane bytes of
// the software format (for a D3D11 frame, the VRAM of its slice).
size_t boxDataSize(void *native) {
    const AVFrame *f = static_cast<FrameBox *>(native)->frame;
    int format = f->format;
    if (f->hw_frames_ctx) format = reinterpret_cast<const AVHWFramesContext *>(f->hw_frames_ctx->data)->sw_format;
    const int size = av_image_get_buffer_size(static_cast<AVPixelFormat>(format), f->width, f->height, 1);
    return size > 0 ? (size_t)size : 0;
}

int boxWidth(void *native) { return static_cast<FrameBox *>(native)->frame->width; }
int boxHeight(void *native) { return static_cast<FrameBox *>(native)->frame->height; }

const VideoFrameOps kAVFrameOps = {boxRetain, boxRelease, boxDataSize, boxWidth, boxHeight};

} // namespace

VideoFrameRef spFrameFromAVFrame(const AVFrame *frame) {
    if (!frame) return {};
    AVFrame *ref = av_frame_alloc();
    if (!ref) return {};
    if (av_frame_ref(ref, frame) < 0) {
        av_frame_free(&ref);
        return {};
    }
    auto *box = new FrameBox;
    box->frame = ref;
    return VideoFrameRef(box, &kAVFrameOps);
}

const AVFrame *spFrameAVFrame(VideoFrameRef handle) {
    if (!handle.native || handle.ops != &kAVFrameOps) return nullptr;
    return static_cast<FrameBox *>(handle.native)->frame;
}

} // namespace sp
