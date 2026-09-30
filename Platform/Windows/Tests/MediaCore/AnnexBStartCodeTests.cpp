#include "SPAnnexBStartCode.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

using spvideo::findStartCode;
using spvideo::findStartCodeScalar;

namespace {

// Reference: first 00 00 01 at or after `from`, widened to a preceding zero
// when that zero is also at or after `from`.
size_t referenceFind(const std::vector<uint8_t> &d, size_t from) {
    for (size_t i = from; i + 3 <= d.size(); ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            return i > from && d[i - 1] == 0 ? i - 1 : i;
        }
    }
    return SIZE_MAX;
}

} // namespace

TEST(AnnexBStartCode, FindsThreeAndFourBytePrefixes) {
    const std::vector<uint8_t> three = {0x42, 0x00, 0x00, 0x01, 0x65};
    EXPECT_EQ(findStartCode(three.data(), three.size(), 0), 1u);

    const std::vector<uint8_t> four = {0x42, 0x00, 0x00, 0x00, 0x01, 0x65};
    EXPECT_EQ(findStartCode(four.data(), four.size(), 0), 1u);
}

TEST(AnnexBStartCode, NeverReturnsBeforeFrom) {
    const std::vector<uint8_t> four = {0x00, 0x00, 0x00, 0x01};
    EXPECT_EQ(findStartCode(four.data(), four.size(), 1), 1u);
}

TEST(AnnexBStartCode, HandlesShortAndOutOfRangeInput) {
    const std::vector<uint8_t> d = {0x00, 0x00};
    EXPECT_EQ(findStartCode(d.data(), d.size(), 0), SIZE_MAX);
    EXPECT_EQ(findStartCode(d.data(), d.size(), 5), SIZE_MAX);
}

TEST(AnnexBStartCode, MatchesReferenceOnLargeBuffers) {
    // Large enough to take the SIMD path on arm64; scalar elsewhere.
    for (size_t pos : {0u, 1u, 31u, 32u, 33u, 47u, 48u, 255u, 300u, 1021u}) {
        std::vector<uint8_t> d(1024, 0xAB);
        d[pos] = 0x00;
        if (pos + 1 < d.size()) d[pos + 1] = 0x00;
        if (pos + 2 < d.size()) d[pos + 2] = 0x01;
        for (size_t from : {size_t{0}, pos > 0 ? pos - 1 : 0, pos}) {
            EXPECT_EQ(findStartCode(d.data(), d.size(), from), referenceFind(d, from))
                << "pos=" << pos << " from=" << from;
            EXPECT_EQ(findStartCodeScalar(d.data(), d.size(), from), referenceFind(d, from))
                << "pos=" << pos << " from=" << from;
        }
    }
}
