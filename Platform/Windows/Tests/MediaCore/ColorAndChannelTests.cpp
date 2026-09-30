#include "SPAudioChannelMap.hpp"
#include "SPColorMetadata.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace sp;

TEST(ColorMetadata, StreamValueWinsUnlessUnspecified) {
    EXPECT_EQ(spAuthoritativeColorValue(AVCOL_PRI_BT2020, AVCOL_PRI_BT709, AVCOL_PRI_UNSPECIFIED),
              AVCOL_PRI_BT2020);
    EXPECT_EQ(spAuthoritativeColorValue(AVCOL_PRI_UNSPECIFIED, AVCOL_PRI_BT709, AVCOL_PRI_UNSPECIFIED),
              AVCOL_PRI_BT709);
}

TEST(ColorMetadata, TransferMapsHdrAndSdrCurves) {
    EXPECT_EQ(spRendererTransferForAVTrc(AVCOL_TRC_SMPTE2084), (SPRendererTransfer{kSPTransferPQ, true}));
    EXPECT_EQ(spRendererTransferForAVTrc(AVCOL_TRC_ARIB_STD_B67), (SPRendererTransfer{kSPTransferHLG, true}));
    EXPECT_EQ(spRendererTransferForAVTrc(AVCOL_TRC_IEC61966_2_1), (SPRendererTransfer{kSPTransferSRGB, false}));
    EXPECT_EQ(spRendererTransferForAVTrc(AVCOL_TRC_GAMMA28), (SPRendererTransfer{kSPTransferGamma28, false}));
    // Unknown and BT.709-family curves render as BT.709.
    EXPECT_EQ(spRendererTransferForAVTrc(AVCOL_TRC_UNSPECIFIED), (SPRendererTransfer{kSPTransferBT709, false}));
    EXPECT_EQ(spRendererTransferForAVTrc(AVCOL_TRC_BT2020_10), (SPRendererTransfer{kSPTransferBT709, false}));
    EXPECT_EQ(spCVGammaLevelForTransfer(kSPTransferGamma22), 2.2);
    EXPECT_EQ(spCVGammaLevelForTransfer(kSPTransferPQ), 0.0);
}

TEST(ColorMetadata, GamutFallsBackToTheMatrixOnlyWhenPrimariesAreUndeclared) {
    EXPECT_EQ(spRendererSourceGamutForAV(AVCOL_PRI_BT2020, AVCOL_SPC_BT709), kSPGamutBT2020);
    EXPECT_EQ(spRendererSourceGamutForAV(AVCOL_PRI_SMPTE432, AVCOL_SPC_BT709), kSPGamutP3);
    EXPECT_EQ(spRendererSourceGamutForAV(AVCOL_PRI_UNSPECIFIED, AVCOL_SPC_BT2020_NCL), kSPGamutBT2020);
    // Declared BT.709 primaries are not overridden by a BT.2020 matrix.
    EXPECT_EQ(spRendererSourceGamutForAV(AVCOL_PRI_BT709, AVCOL_SPC_BT2020_NCL), kSPGamutBT709);
}

namespace {

AudioOutputLayout resolve(const std::vector<uint32_t> &labels) {
    AudioOutputLayout layout;
    resolveOutputLayout(labels.data(), (int)labels.size(), &layout);
    return layout;
}

} // namespace

TEST(AudioChannelMap, StereoDeviceStaysStereo) {
    AudioOutputLayout layout;
    const uint32_t labels[] = {kSPSpeakerLeft, kSPSpeakerRight};
    EXPECT_FALSE(resolveOutputLayout(labels, 2, &layout));
    EXPECT_EQ(layout.channels, 2);
    EXPECT_FALSE(layout.useMap);
}

TEST(AudioChannelMap, FivePointOneMapsBackPairToSurround) {
    const AudioOutputLayout layout = resolve({kSPSpeakerLeft, kSPSpeakerRight, kSPSpeakerCenter,
                                              kSPSpeakerLFE, kSPSpeakerLeftSurround,
                                              kSPSpeakerRightSurround});
    EXPECT_EQ(layout.channels, 6);
    EXPECT_EQ(layout.mask, (uint64_t)kSPMask5Point1);
    ASSERT_TRUE(layout.useMap);
    // Device index -> index within the 5.1 mask order FL FR FC LFE BL BR.
    for (int i = 0; i < 6; ++i) EXPECT_EQ(layout.map[(size_t)i], i) << i;
    EXPECT_STREQ(layout.name(), "5.1");
}

TEST(AudioChannelMap, SevenPointOneUsesSideAndRearPairs) {
    // Device order as some USB interfaces report it: rear pair before the side pair.
    const AudioOutputLayout layout = resolve({kSPSpeakerLeft, kSPSpeakerRight, kSPSpeakerCenter,
                                              kSPSpeakerLFE, kSPSpeakerRearSurroundLeft,
                                              kSPSpeakerRearSurroundRight,
                                              kSPSpeakerLeftSurroundDirect,
                                              kSPSpeakerRightSurroundDirect});
    EXPECT_EQ(layout.channels, 8);
    EXPECT_EQ(layout.mask, (uint64_t)kSPMask7Point1);
    // 7.1 mask order: FL FR FC LFE BL BR SL SR.
    const int expected[] = {0, 1, 2, 3, 4, 5, 6, 7};
    for (int i = 0; i < 8; ++i) EXPECT_EQ(layout.map[(size_t)i], expected[i]) << i;
    EXPECT_STREQ(layout.name(), "7.1");
}

TEST(AudioChannelMap, MissingLfeFallsBackToStereo) {
    const AudioOutputLayout layout = resolve({kSPSpeakerLeft, kSPSpeakerRight, kSPSpeakerCenter,
                                              99, kSPSpeakerLeftSurround, kSPSpeakerRightSurround});
    EXPECT_EQ(layout.channels, 2);
    EXPECT_FALSE(layout.useMap);
}

TEST(AudioChannelMap, PositionalLayoutNeedsEnoughDeviceChannels) {
    AudioOutputLayout layout;
    EXPECT_TRUE(resolvePositionalLayout("5.1", 6, &layout));
    EXPECT_EQ(layout.channels, 6);
    EXPECT_FALSE(resolvePositionalLayout("7.1", 6, &layout));
    EXPECT_FALSE(resolvePositionalLayout("quad", 8, &layout));
}
