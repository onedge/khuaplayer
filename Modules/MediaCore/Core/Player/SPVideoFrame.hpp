// A decoded video frame as the player core sees it: an opaque, reference-
// counted platform image. On macOS it is a CVPixelBuffer; on Windows it will
// be an AVFrame holding a D3D11 texture.
//
// VideoFrameRef is a plain handle with the ownership rules of a CF type: a
// function that returns one at +1 transfers a reference, spFrameRetain adds
// one and spFrameRelease drops one. It is trivially copyable, so copies are
// borrowed aliases, exactly like copying a CVPixelBufferRef.
//
// While SPPlayerCore.mm still holds frames as CVPixelBufferRef in places, it
// defines SP_VIDEO_FRAME_CV_BRIDGE before including this header. The handle
// then converts implicitly to and from CVPixelBufferRef and compares and
// tests as that pointer does, so the two can be mixed without changing
// behaviour. Pure C++ code, which never sees CoreVideo, gets operator== and a
// truth test instead; any leftover CoreVideo use then fails to compile.
#pragma once

#include <cstddef>

#if defined(SP_VIDEO_FRAME_CV_BRIDGE)
typedef struct __CVBuffer *CVPixelBufferRef;
#endif

namespace sp {

struct VideoFrameOps {
    void (*retain)(void *native);
    void (*release)(void *native);
    size_t (*dataSize)(void *native);
    int (*width)(void *native);
    int (*height)(void *native);
};

#if defined(SP_VIDEO_FRAME_CV_BRIDGE)
// CVPixelBufferRetain/Release and the CoreVideo getters
// (Platform/macOS/Player/SPCVVideoFrame.mm).
extern const VideoFrameOps kSPCVVideoFrameOps;
#endif

struct VideoFrameRef {
    void *native = nullptr;
    const VideoFrameOps *ops = nullptr;

    VideoFrameRef() = default;
    VideoFrameRef(void *nativeFrame, const VideoFrameOps *frameOps) : native(nativeFrame), ops(frameOps) {}

#if defined(SP_VIDEO_FRAME_CV_BRIDGE)
    // Adopts the pointer without retaining it, as assigning a
    // CVPixelBufferRef did. NULL gives an empty handle.
    VideoFrameRef(CVPixelBufferRef buffer)
        : native(buffer), ops(buffer ? &kSPCVVideoFrameOps : nullptr) {}
    operator CVPixelBufferRef() const { return (CVPixelBufferRef)native; }
#else
    explicit operator bool() const { return native != nullptr; }
    friend bool operator==(const VideoFrameRef &a, const VideoFrameRef &b) { return a.native == b.native; }
#endif
};

// Returns `frame` after adding a reference; an empty handle stays empty.
inline VideoFrameRef spFrameRetain(VideoFrameRef frame) {
    if (frame.native) frame.ops->retain(frame.native);
    return frame;
}

// Drops one reference; an empty handle is ignored, like CVPixelBufferRelease(NULL).
inline void spFrameRelease(VideoFrameRef frame) {
    if (frame.native) frame.ops->release(frame.native);
}

inline size_t spFrameDataSize(VideoFrameRef frame) {
    return frame.native ? frame.ops->dataSize(frame.native) : 0;
}
inline int spFrameWidth(VideoFrameRef frame) { return frame.native ? frame.ops->width(frame.native) : 0; }
inline int spFrameHeight(VideoFrameRef frame) { return frame.native ? frame.ops->height(frame.native) : 0; }

} // namespace sp
