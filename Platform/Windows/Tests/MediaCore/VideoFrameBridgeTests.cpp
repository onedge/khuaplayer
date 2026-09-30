// Compiles the SP_VIDEO_FRAME_CV_BRIDGE mode that SPPlayerCore.mm uses, with a
// stand-in for CoreVideo's opaque buffer type. clang-cl shares Apple clang's
// C++ front end, so the expressions the .mm writes on frames (null checks,
// comparisons with each other and with raw pointers, brace initialisation,
// passing to CVPixelBufferRef and CFTypeRef parameters) are checked here for
// ambiguity before a Mac build sees them.
#define SP_VIDEO_FRAME_CV_BRIDGE 1
#include "Player/SPVideoFrame.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <type_traits>

struct __CVBuffer {
    int refs = 1;
};

namespace {

void cvRetain(void *native) { ++static_cast<__CVBuffer *>(native)->refs; }
void cvRelease(void *native) { --static_cast<__CVBuffer *>(native)->refs; }
size_t cvDataSize(void *) { return 0; }
int cvDim(void *) { return 0; }

// What the .mm passes frames to.
bool takesPixelBuffer(CVPixelBufferRef buffer) { return buffer != nullptr; }
bool takesTypeRef(const void *ref) { return ref != nullptr; }

// The shape of SPPlayerCore.mm's DecodedFrame.
struct DecodedFrame {
    sp::VideoFrameRef buffer;
    int64_t ptsUs = 0;
    int64_t gen = 0;
    bool synthetic = false;
};

} // namespace

// Defined by Platform/macOS/Player/SPCVVideoFrame.mm in the Mac build.
namespace sp {
const VideoFrameOps kSPCVVideoFrameOps = {cvRetain, cvRelease, cvDataSize, cvDim, cvDim};
}

static_assert(std::is_trivially_copyable_v<DecodedFrame>);

TEST(VideoFrameBridge, BehavesLikeTheCVPixelBufferRefItReplaces) {
    __CVBuffer image;
    CVPixelBufferRef raw = &image;

    DecodedFrame f{raw, 40, 1, false}; // brace initialisation from a raw buffer
    DecodedFrame empty;
    empty.buffer = NULL;
    EXPECT_TRUE(!empty.buffer);
    EXPECT_TRUE(empty.buffer == NULL);
    EXPECT_TRUE(f.buffer != NULL);
    if (f.buffer) SUCCEED();

    // Identity, as BoundedQueue peeks compare frames by pointer.
    DecodedFrame alias = f;
    EXPECT_TRUE(alias.buffer == f.buffer);
    EXPECT_TRUE(f.buffer == raw);
    EXPECT_TRUE(raw == f.buffer);

    // Passing to Objective-C and CoreFoundation APIs.
    EXPECT_TRUE(takesPixelBuffer(f.buffer));
    EXPECT_TRUE(takesTypeRef(f.buffer));
    CVPixelBufferRef back = f.buffer;
    EXPECT_EQ(back, raw);

    // Ownership through the CV ops, including a raw pointer argument.
    sp::VideoFrameRef last = sp::spFrameRetain(f.buffer);
    EXPECT_EQ(image.refs, 2);
    sp::spFrameRelease(last);
    sp::spFrameRelease(raw);
    EXPECT_EQ(image.refs, 0);
    sp::spFrameRelease(empty.buffer);
    sp::spFrameRelease(NULL);
}
