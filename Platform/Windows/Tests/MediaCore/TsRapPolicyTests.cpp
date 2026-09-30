#include "TsRapPolicy.hpp"

#include <gtest/gtest.h>

using namespace sp;

TEST(TsRapPolicy, WindowDefaultsAndClampsToGop) {
    EXPECT_EQ(spTsRapWindowUs(0, 0), 2'500'000);
    // Short GOPs keep the 2.5 s floor.
    EXPECT_EQ(spTsRapWindowUs(1'000'000, 0), 2'500'000);
    // gop + gop/4 + 250 ms.
    EXPECT_EQ(spTsRapWindowUs(4'000'000, 0), 5'250'000);
    EXPECT_EQ(spTsRapWindowUs(20'000'000, 0), 12'000'000);
}

TEST(TsRapPolicy, WindowTriplesPerAttemptWithoutOverflow) {
    EXPECT_EQ(spTsRapWindowUs(0, 1), 7'500'000);
    EXPECT_EQ(spTsRapWindowUs(0, 2), 22'500'000);
    const int64_t huge = spTsRapWindowUs(0, 200);
    EXPECT_GT(huge, int64_t{1} << 40);
    EXPECT_LE(huge, (int64_t{1} << 40) * 3);
}

TEST(TsRapPolicy, ExhaustionOnAnyLimit) {
    const SPTsRapLimits lim;
    EXPECT_FALSE(spTsRapExhausted(lim, lim.maxSpanUs, lim.maxBytes, lim.maxVideoPackets));
    EXPECT_TRUE(spTsRapExhausted(lim, lim.maxSpanUs + 1, 0, 0));
    EXPECT_TRUE(spTsRapExhausted(lim, 0, lim.maxBytes + 1, 0));
    EXPECT_TRUE(spTsRapExhausted(lim, 0, 0, lim.maxVideoPackets + 1));
}

TEST(TsRapPolicy, GopBlendIgnoresInvalidSamples) {
    EXPECT_EQ(spTsRapBlendGopUs(2'000'000, 0), 2'000'000);
    EXPECT_EQ(spTsRapBlendGopUs(2'000'000, 30'000'001), 2'000'000);
    EXPECT_EQ(spTsRapBlendGopUs(0, 1'000'000), 1'000'000);
    // Moves a quarter of the way toward the new sample.
    EXPECT_EQ(spTsRapBlendGopUs(2'000'000, 6'000'000), 3'000'000);
}
