// A decoded video frame as the player core sees it: an opaque, reference-
// counted platform image. On Windows it will be an AVFrame holding a D3D11
// texture.
//
// VideoFrameRef is a plain handle with the ownership rules of a CF type (the
// Mac player's CVPixelBufferRef, which the player logic was written against):
// a function that returns one at +1 transfers a reference, spFrameRetain adds
// one and spFrameRelease drops one. It is trivially copyable, so copies are
// borrowed aliases.
#pragma once

#include <cstddef>

namespace sp {

struct VideoFrameOps {
    void (*retain)(void *native);
    void (*release)(void *native);
    size_t (*dataSize)(void *native);
    int (*width)(void *native);
    int (*height)(void *native);
};

struct VideoFrameRef {
    void *native = nullptr;
    const VideoFrameOps *ops = nullptr;

    VideoFrameRef() = default;
    VideoFrameRef(void *nativeFrame, const VideoFrameOps *frameOps) : native(nativeFrame), ops(frameOps) {}

    explicit operator bool() const { return native != nullptr; }
    friend bool operator==(const VideoFrameRef &a, const VideoFrameRef &b) { return a.native == b.native; }
};

// Returns `frame` after adding a reference; an empty handle stays empty.
inline VideoFrameRef spFrameRetain(VideoFrameRef frame) {
    if (frame.native) frame.ops->retain(frame.native);
    return frame;
}

// Drops one reference; an empty handle is ignored.
inline void spFrameRelease(VideoFrameRef frame) {
    if (frame.native) frame.ops->release(frame.native);
}

inline size_t spFrameDataSize(VideoFrameRef frame) {
    return frame.native ? frame.ops->dataSize(frame.native) : 0;
}
inline int spFrameWidth(VideoFrameRef frame) { return frame.native ? frame.ops->width(frame.native) : 0; }
inline int spFrameHeight(VideoFrameRef frame) { return frame.native ? frame.ops->height(frame.native) : 0; }

} // namespace sp
