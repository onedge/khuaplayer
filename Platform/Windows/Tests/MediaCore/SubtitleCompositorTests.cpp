#include "SPSubtitleCompositor.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

using sp::compositeSubtitleBitmap;

namespace {

// libass RGBT: low byte is transparency.
constexpr uint32_t rgbt(uint8_t r, uint8_t g, uint8_t b, uint8_t t) {
    return (uint32_t)r << 24 | (uint32_t)g << 16 | (uint32_t)b << 8 | t;
}

} // namespace

TEST(SubtitleCompositor, FullMaskWritesOpaqueBgra) {
    std::array<uint8_t, 4> canvas{};
    const uint8_t mask = 255;
    compositeSubtitleBitmap(canvas.data(), 4, &mask, 1, 1, 1, rgbt(10, 20, 30, 0));
    EXPECT_EQ(canvas, (std::array<uint8_t, 4>{30, 20, 10, 255}));
}

TEST(SubtitleCompositor, ZeroMaskAndFullTransparencyLeaveCanvas) {
    std::array<uint8_t, 4> canvas{1, 2, 3, 4};
    const uint8_t empty = 0;
    compositeSubtitleBitmap(canvas.data(), 4, &empty, 1, 1, 1, rgbt(255, 255, 255, 0));
    EXPECT_EQ(canvas, (std::array<uint8_t, 4>{1, 2, 3, 4}));

    const uint8_t full = 255;
    compositeSubtitleBitmap(canvas.data(), 4, &full, 1, 1, 1, rgbt(255, 255, 255, 255));
    EXPECT_EQ(canvas, (std::array<uint8_t, 4>{1, 2, 3, 4}));
}

TEST(SubtitleCompositor, PartialCoverageOnEmptyPixelIsPremultiplied) {
    std::array<uint8_t, 4> canvas{};
    const uint8_t mask = 128;
    compositeSubtitleBitmap(canvas.data(), 4, &mask, 1, 1, 1, rgbt(255, 0, 0, 0));
    EXPECT_EQ(canvas[3], 128);
    EXPECT_EQ(canvas[2], 128); // red, premultiplied by alpha
    EXPECT_EQ(canvas[1], 0);
    EXPECT_EQ(canvas[0], 0);
}

TEST(SubtitleCompositor, BlendsOverExistingPixel) {
    std::array<uint8_t, 4> canvas{0, 0, 255, 255}; // opaque red (BGRA)
    const uint8_t mask = 255;
    // 50% transparent blue over it.
    compositeSubtitleBitmap(canvas.data(), 4, &mask, 1, 1, 1, rgbt(0, 0, 255, 127));
    EXPECT_NEAR(canvas[0], 128, 1);
    EXPECT_NEAR(canvas[2], 127, 1);
    EXPECT_EQ(canvas[3], 255);
}

TEST(SubtitleCompositor, RespectsStrides) {
    // 2x2 mask in a 3-wide buffer; canvas rows padded to 12 bytes.
    const std::array<uint8_t, 6> mask{255, 0, 9, 0, 255, 9};
    std::array<uint8_t, 24> canvas{};
    compositeSubtitleBitmap(canvas.data(), 12, mask.data(), 3, 2, 2, rgbt(1, 2, 3, 0));
    EXPECT_EQ(canvas[3], 255);  // (0,0)
    EXPECT_EQ(canvas[7], 0);    // (1,0)
    EXPECT_EQ(canvas[11], 0);   // padding, untouched
    EXPECT_EQ(canvas[15], 0);   // (0,1)
    EXPECT_EQ(canvas[19], 255); // (1,1)
}
