#include "SPFrameSelectionPolicy.hpp"

#include <gtest/gtest.h>

using namespace sp;

namespace {

SPFrameCandidateMetadata frame(int64_t ptsUs, int64_t generation = 1,
                               bool synthetic = false, uint64_t epoch = 0) {
    return {ptsUs, generation, synthetic, epoch};
}

SPSyntheticFramePolicy enforcedPolicy(bool acceptsSynthetic, uint64_t epoch) {
    return {true, acceptsSynthetic, epoch};
}

} // namespace

TEST(SyntheticFramePolicy, OnlyEnforcedPolicyMarksSyntheticFramesStale) {
    EXPECT_FALSE(spIsStaleSyntheticFrame(frame(0, 1, true, 7), {}));
    EXPECT_FALSE(spIsStaleSyntheticFrame(frame(0, 1, false, 7), enforcedPolicy(false, 1)));
    EXPECT_TRUE(spIsStaleSyntheticFrame(frame(0, 1, true, 1), enforcedPolicy(false, 1)));
    EXPECT_TRUE(spIsStaleSyntheticFrame(frame(0, 1, true, 2), enforcedPolicy(true, 1)));
    EXPECT_FALSE(spIsStaleSyntheticFrame(frame(0, 1, true, 1), enforcedPolicy(true, 1)));
}

TEST(SteadyFrameSelection, StaleSyntheticIsDroppedEvenInTheFuture) {
    SPSteadyFrameSelectionContext ctx;
    ctx.presentUs = 0;
    ctx.currentGeneration = 1;
    ctx.syntheticPolicy = enforcedPolicy(true, 3);
    EXPECT_EQ(spEvaluateSteadyFrame(frame(5'000'000, 1, true, 2), ctx).action,
              SPFrameSelectionAction::DropStaleInterpolationPolicy);
}

TEST(SteadyFrameSelection, FutureFrameHoldsOrReanchors) {
    SPSteadyFrameSelectionContext ctx;
    ctx.presentUs = 1'000'000;
    ctx.currentGeneration = 1;
    ctx.reanchorThresholdUs = 1'000'000;

    EXPECT_EQ(spEvaluateSteadyFrame(frame(1'500'000), ctx).action,
              SPFrameSelectionAction::HoldFuture);
    // Far ahead with nothing selected yet: move the clock instead of waiting.
    EXPECT_EQ(spEvaluateSteadyFrame(frame(2'000'001), ctx).action,
              SPFrameSelectionAction::ReanchorClock);
    ctx.hasSelectedFrame = true;
    EXPECT_EQ(spEvaluateSteadyFrame(frame(2'000'001), ctx).action,
              SPFrameSelectionAction::HoldFuture);
}

TEST(SteadyFrameSelection, OldGenerationFutureFrameIsStaleNotHeld) {
    SPSteadyFrameSelectionContext ctx;
    ctx.presentUs = 0;
    ctx.currentGeneration = 2;
    EXPECT_EQ(spEvaluateSteadyFrame(frame(9'000'000, 1), ctx).action,
              SPFrameSelectionAction::DropStaleGeneration);
}

TEST(SteadyFrameSelection, SeekTargetFiltersEarlierFramesAndReportsArrival) {
    SPSteadyFrameSelectionContext ctx;
    ctx.presentUs = 0;
    ctx.currentGeneration = 1;
    ctx.seekTargetUs = 4'000'000;

    EXPECT_EQ(spEvaluateSteadyFrame(frame(3'999'999), ctx).action,
              SPFrameSelectionAction::DropBeforeSeekTarget);
    // With a seek target, a future PTS does not hold presentation.
    const auto reached = spEvaluateSteadyFrame(frame(4'000'000), ctx);
    EXPECT_EQ(reached.action, SPFrameSelectionAction::Select);
    EXPECT_TRUE(reached.reachesSeekTarget);
}

TEST(SteadyFrameSelection, LateReplaceThresholdKeepsSelectedFrame) {
    SPSteadyFrameSelectionContext ctx;
    ctx.presentUs = 1'000'000;
    ctx.currentGeneration = 1;
    ctx.hasSelectedFrame = true;
    ctx.lateReplaceThresholdUs = 20'000;

    EXPECT_EQ(spEvaluateSteadyFrame(frame(990'000), ctx).action,
              SPFrameSelectionAction::HoldFuture);
    const auto late = spEvaluateSteadyFrame(frame(980'000), ctx);
    EXPECT_EQ(late.action, SPFrameSelectionAction::Select);
    EXPECT_FALSE(late.reachesSeekTarget);
}

TEST(ImmediateFrameSelection, MinimumPtsIncludesTimeZero) {
    SPImmediateFrameSelectionContext ctx;
    ctx.currentGeneration = 1;
    ctx.minimumPtsUs = 0;
    EXPECT_EQ(spEvaluateImmediateFrame(frame(0), ctx).action, SPFrameSelectionAction::Select);

    ctx.minimumPtsUs = 500;
    EXPECT_EQ(spEvaluateImmediateFrame(frame(499), ctx).action,
              SPFrameSelectionAction::DropBeforeMinimumPTS);
    // Negative candidate PTS bypasses catch-up filtering.
    EXPECT_EQ(spEvaluateImmediateFrame(frame(-1), ctx).action, SPFrameSelectionAction::Select);
    EXPECT_EQ(spEvaluateImmediateFrame(frame(600, 0), ctx).action,
              SPFrameSelectionAction::DropStaleGeneration);
}

TEST(StepFrameSelection, RejectsSyntheticAndNonAdvancingFrames) {
    SPStepFrameSelectionContext ctx;
    ctx.currentGeneration = 1;
    ctx.lastPresentedPtsUs = 1'000;

    EXPECT_EQ(spEvaluateStepFrame(frame(2'000, 1, true), ctx).action,
              SPFrameSelectionAction::DropSyntheticForStep);
    EXPECT_EQ(spEvaluateStepFrame(frame(1'000), ctx).action,
              SPFrameSelectionAction::DropBeforeMinimumPTS);
    EXPECT_EQ(spEvaluateStepFrame(frame(1'001), ctx).action, SPFrameSelectionAction::Select);
    EXPECT_EQ(spEvaluateStepFrame(frame(2'000, 0), ctx).action,
              SPFrameSelectionAction::DropStaleGeneration);
}

TEST(SeekPreviewFrameSelection, DropsFramesFromBeforeTheBurst) {
    SPSeekPreviewSelectionContext ctx;
    ctx.seekBurstBaseGeneration = 5;
    EXPECT_EQ(spEvaluateSeekPreviewFrame(frame(0, 4), ctx).action,
              SPFrameSelectionAction::DropBeforeSeekBurst);
    EXPECT_EQ(spEvaluateSeekPreviewFrame(frame(0, 5), ctx).action,
              SPFrameSelectionAction::Select);
    EXPECT_EQ(spEvaluateSeekPreviewFrame(frame(0, 9), ctx).action,
              SPFrameSelectionAction::Select);
}

TEST(SeekSettlement, PreciseSeekSettlesAtTargetCoarseSeekReanchors) {
    const auto stale = spEvaluateSeekSettlement(frame(10, 1), 2, -1);
    EXPECT_FALSE(stale.settles);

    const auto early = spEvaluateSeekSettlement(frame(999, 2), 2, 1'000);
    EXPECT_FALSE(early.settles);
    const auto precise = spEvaluateSeekSettlement(frame(1'000, 2), 2, 1'000);
    EXPECT_TRUE(precise.settles);
    EXPECT_TRUE(precise.reachesPreciseTarget);
    EXPECT_FALSE(precise.reanchorToCandidate);

    const auto coarse = spEvaluateSeekSettlement(frame(1'234, 2), 2, -1);
    EXPECT_TRUE(coarse.settles);
    EXPECT_FALSE(coarse.reachesPreciseTarget);
    EXPECT_TRUE(coarse.reanchorToCandidate);
}
