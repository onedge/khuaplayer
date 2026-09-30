#include "Player/SPVideoFrame.hpp"

#include <gtest/gtest.h>

#include <type_traits>

namespace {

// A reference-counted stand-in for a platform image.
struct FakeImage {
    int refs = 1;
    int width = 1920;
    int height = 1080;
};

void fakeRetain(void *native) { ++static_cast<FakeImage *>(native)->refs; }
void fakeRelease(void *native) { --static_cast<FakeImage *>(native)->refs; }
size_t fakeDataSize(void *native) {
    auto *image = static_cast<FakeImage *>(native);
    return (size_t)image->width * image->height * 3 / 2;
}
int fakeWidth(void *native) { return static_cast<FakeImage *>(native)->width; }
int fakeHeight(void *native) { return static_cast<FakeImage *>(native)->height; }

const sp::VideoFrameOps kFakeOps = {fakeRetain, fakeRelease, fakeDataSize, fakeWidth, fakeHeight};

} // namespace

// Frames sit in BoundedQueue and are copied as borrowed aliases, like the
// CVPixelBufferRef they replace.
static_assert(std::is_trivially_copyable_v<sp::VideoFrameRef>);

TEST(VideoFrame, RetainAndReleaseFollowCfOwnership) {
    FakeImage image; // created at +1
    sp::VideoFrameRef frame(&image, &kFakeOps);
    const sp::VideoFrameRef alias = frame; // a copy is a borrowed alias
    EXPECT_EQ(image.refs, 1);
    EXPECT_TRUE(sp::spFrameRetain(frame) == alias);
    EXPECT_EQ(image.refs, 2);
    sp::spFrameRelease(alias);
    sp::spFrameRelease(frame);
    EXPECT_EQ(image.refs, 0);
}

TEST(VideoFrame, EmptyHandleIsIgnoredLikeNull) {
    const sp::VideoFrameRef empty;
    EXPECT_FALSE(empty);
    EXPECT_FALSE(sp::spFrameRetain(empty));
    sp::spFrameRelease(empty); // no-op, like CVPixelBufferRelease(NULL)
    EXPECT_EQ(sp::spFrameDataSize(empty), 0u);
    EXPECT_EQ(sp::spFrameWidth(empty), 0);
}

TEST(VideoFrame, IdentityAndGettersGoThroughTheOps) {
    FakeImage a, b;
    const sp::VideoFrameRef fa(&a, &kFakeOps), fb(&b, &kFakeOps);
    EXPECT_TRUE(fa == sp::VideoFrameRef(&a, &kFakeOps));
    EXPECT_FALSE(fa == fb);
    EXPECT_TRUE(fa);
    EXPECT_EQ(sp::spFrameWidth(fa), 1920);
    EXPECT_EQ(sp::spFrameHeight(fa), 1080);
    EXPECT_EQ(sp::spFrameDataSize(fa), 1920u * 1080 * 3 / 2);
}
