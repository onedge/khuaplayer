#include "Audio/SPAudioEngine.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

using sp::AudioEngine;

namespace {

constexpr int kCh = 2;

std::vector<float> ramp(int frames, float scale = 0.5f) {
    std::vector<float> pcm((size_t)frames * kCh);
    for (int i = 0; i < frames; ++i)
        for (int c = 0; c < kCh; ++c) pcm[(size_t)i * kCh + c] = scale * std::sin(0.01f * (float)i + (float)c);
    return pcm;
}

// Renders `frames` and returns them; the host clock is irrelevant here.
std::vector<float> pull(AudioEngine &engine, int frames, int64_t hostUs = 1, int64_t queued = 0) {
    std::vector<float> out((size_t)frames * engine.channels());
    engine.render(out.data(), frames, hostUs, queued);
    return out;
}

} // namespace

TEST(AudioEngine, PassesAudioThroughBitExactAfterTheFadeIn) {
    AudioEngine engine;
    const auto pcm = ramp(4000);
    ASSERT_TRUE(engine.writePCM(pcm.data(), 4000, kCh, 1.0, engine.currentEpoch()));
    EXPECT_EQ(engine.bufferedFrames(), 4000);

    const auto fade = pull(engine, AudioEngine::kGainRampFrames); // 20 ms fade-in
    EXPECT_LT(std::fabs(fade[0]), std::fabs(pcm[0]) + 1e-6f);
    const auto rest = pull(engine, 4000 - AudioEngine::kGainRampFrames);
    for (size_t i = 0; i < rest.size(); ++i)
        ASSERT_EQ(rest[i], pcm[(size_t)AudioEngine::kGainRampFrames * kCh + i]) << i;
    EXPECT_EQ(engine.bufferedFrames(), 0);

    // An underrun renders silence without advancing the clock.
    const auto silence = pull(engine, 100);
    for (float v : silence) EXPECT_EQ(v, 0.0f);
}

TEST(AudioEngine, ResetDiscardsQueuedAudioAndRejectsStaleWriters) {
    AudioEngine engine;
    const int32_t before = engine.currentEpoch();
    const auto pcm = ramp(1000);
    ASSERT_TRUE(engine.writePCM(pcm.data(), 1000, kCh, 1.0, before));
    engine.reset();
    EXPECT_EQ(engine.bufferedFrames(), 0);
    EXPECT_NE(engine.currentEpoch(), before);
    EXPECT_FALSE(engine.writePCM(pcm.data(), 1000, kCh, 1.0, before));
    EXPECT_TRUE(engine.writePCM(pcm.data(), 1000, kCh, 1.0, engine.currentEpoch()));
    // Wrong channel count is refused, like a writer racing a layout change.
    EXPECT_FALSE(engine.writePCM(pcm.data(), 500, 6, 1.0, engine.currentEpoch()));
    engine.reset(6);
    EXPECT_EQ(engine.channels(), 6);
}

TEST(AudioEngine, ClockInterpolatesWithinTheLastBlockOnly) {
    AudioEngine engine;
    const auto pcm = ramp(4800);
    ASSERT_TRUE(engine.writePCM(pcm.data(), 4800, kCh, 1.0, engine.currentEpoch()));
    // 480 frames (10 ms) heard from t = 1 s.
    pull(engine, 480, 1'000'000);
    EXPECT_EQ(engine.clockFrames(999'000), 0);      // before it plays
    EXPECT_EQ(engine.clockFrames(1'005'000), 240);  // halfway through
    EXPECT_EQ(engine.clockFrames(1'050'000), 480);  // clamped to the block
    pull(engine, 480, 1'010'000);
    EXPECT_EQ(engine.clockFrames(1'012'500), 600);
}

TEST(AudioEngine, WriterWaitsForSpaceAndCanBeAborted) {
    AudioEngine engine;
    const int frames = AudioEngine::kRingFrames + 4000; // more than the ring holds
    const auto pcm = ramp(frames);
    std::atomic<bool> done{false}, result{false};
    std::thread writer([&] {
        result = engine.writePCM(pcm.data(), frames, kCh, 1.0, engine.currentEpoch());
        done = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(done.load()); // blocked on a full ring
    pull(engine, 8000);        // the device frees space
    for (int i = 0; i < 100 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    writer.join();
    EXPECT_TRUE(result.load());

    // abortWrites releases a writer blocked on a full ring. As in the Mac
    // player's SPAudioOutput, writePCM still reports the call as accepted:
    // false only means it was refused up front (stale epoch, wrong channel
    // count, dead device).
    std::atomic<bool> returned{false};
    std::thread blocked([&] {
        engine.writePCM(pcm.data(), frames, kCh, 1.0, engine.currentEpoch());
        returned = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(returned.load());
    const auto start = std::chrono::steady_clock::now();
    engine.abortWrites();
    blocked.join();
    EXPECT_TRUE(returned.load());
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(300));
}

// Pitch-preserving 2x playback produces about half as many output frames.
TEST(AudioEngine, TimeStretchHalvesTheDurationAtDoubleRate) {
    AudioEngine engine;
    engine.requestRate(2.0);
    const int input = 48000;
    const auto pcm = ramp(input);
    std::atomic<bool> done{false};
    std::thread writer([&] {
        const int32_t epoch = engine.currentEpoch();
        for (int pos = 0; pos < input; pos += 1024) {
            const int n = std::min(1024, input - pos);
            ASSERT_TRUE(engine.writePCM(pcm.data() + (size_t)pos * kCh, n, kCh, 2.0, epoch));
        }
        engine.drainStretchAtEOF(epoch);
        done = true;
    });
    int64_t rendered = 0;
    while (!done || engine.bufferedFrames() > 0) {
        const int64_t before = engine.bufferedFrames();
        pull(engine, 512);
        rendered += std::min<int64_t>(before, 512);
        if (!done && before == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    writer.join();
    EXPECT_NEAR((double)rendered, input / 2.0, input * 0.03);
    EXPECT_FALSE(engine.rateSwitchPending());
}

TEST(AudioEngine, LimiterKeepsOverdrivenAudioWithinFullScale) {
    AudioEngine engine;
    const auto loud = ramp(8000, 2.0f); // twice full scale, as an unnormalised downmix can be
    ASSERT_TRUE(engine.writePCM(loud.data(), 8000, kCh, 1.0, engine.currentEpoch()));
    const auto out = pull(engine, 8000);
    for (float v : out) ASSERT_LE(std::fabs(v), 1.0f + 1e-6f);
}

TEST(AudioEngine, ZeroVolumeFadesToSilence) {
    AudioEngine engine;
    engine.setVolume(0.0f);
    engine.setVolume(std::nanf("")); // ignored
    const auto pcm = ramp(4000);
    ASSERT_TRUE(engine.writePCM(pcm.data(), 4000, kCh, 1.0, engine.currentEpoch()));
    pull(engine, AudioEngine::kGainRampFrames);
    const auto out = pull(engine, 2000);
    for (float v : out) ASSERT_EQ(v, 0.0f);
}

// WASAPI dating: at hostUs the device still holds `queued` frames; they play
// first, then the new block. The clock reports only what has been heard.
TEST(AudioEngine, ClockCountsQueuedFramesAsNotYetHeard) {
    AudioEngine engine;
    const auto pcm = ramp(9600);
    ASSERT_TRUE(engine.writePCM(pcm.data(), 9600, kCh, 1.0, engine.currentEpoch()));
    pull(engine, 1440, 1'000'000, 0);            // buffer filled at t = 1 s
    EXPECT_EQ(engine.clockFrames(1'010'000), 480); // 10 ms heard
    pull(engine, 480, 1'010'000, 960);            // 960 still queued at 1.01 s
    EXPECT_EQ(engine.clockFrames(1'010'000), 480); // no jump ahead
    EXPECT_EQ(engine.clockFrames(1'020'000), 960);
    EXPECT_EQ(engine.clockFrames(1'050'000), 1920); // everything consumed
}

TEST(AudioEngine, FrozenClockHoldsAndDiscardedFramesAreNotCounted) {
    AudioEngine engine;
    const auto pcm = ramp(4800);
    ASSERT_TRUE(engine.writePCM(pcm.data(), 4800, kCh, 1.0, engine.currentEpoch()));
    pull(engine, 1440, 1'000'000, 0);
    engine.freezeClock(1'010'000); // stopped after 10 ms of the 30 ms queued
    EXPECT_EQ(engine.clockFrames(1'100'000), 480);
    engine.discardDeviceQueue(1'100'000); // the endpoint was flushed
    EXPECT_EQ(engine.clockFrames(1'100'000), 480);
    pull(engine, 480, 2'000'000, 0); // restarted with an empty endpoint
    EXPECT_EQ(engine.clockFrames(2'000'000), 480); // continues, no jump
    EXPECT_EQ(engine.clockFrames(2'005'000), 720);
}

// A paused device keeps its queue: on resume the clock picks up exactly
// where the device is, without going back.
TEST(AudioEngine, ResumeWithAKeptQueueContinuesTheClock) {
    AudioEngine engine;
    const auto pcm = ramp(4800);
    ASSERT_TRUE(engine.writePCM(pcm.data(), 4800, kCh, 1.0, engine.currentEpoch()));
    pull(engine, 1440, 1'000'000, 0);
    engine.freezeClock(1'010'000);                  // 480 heard, 960 kept
    pull(engine, 480, 2'000'000, 960 + 480 - 480);  // resumed: nothing played yet
    EXPECT_EQ(engine.clockFrames(2'000'000), 480);
    EXPECT_EQ(engine.clockFrames(2'010'000), 960);
}

// Silence the device holds (an underrun) does not count as heard: the clock
// holds through it instead of running ahead and coming back.
TEST(AudioEngine, ClockHoldsThroughQueuedSilence) {
    AudioEngine engine;
    const auto pcm = ramp(960);
    ASSERT_TRUE(engine.writePCM(pcm.data(), 480, kCh, 1.0, engine.currentEpoch()));
    pull(engine, 960, 1'000'000, 0); // 480 real, then 480 of underrun silence
    EXPECT_EQ(engine.clockFrames(1'015'000), 480);
    ASSERT_TRUE(engine.writePCM(pcm.data(), 480, kCh, 1.0, engine.currentEpoch()));
    pull(engine, 480, 1'005'000, 720); // 240 played; then 240 real, 480 silent, 480 real
    EXPECT_EQ(engine.clockFrames(1'005'000), 240);
    EXPECT_EQ(engine.clockFrames(1'010'000), 480);
    EXPECT_EQ(engine.clockFrames(1'019'000), 480); // through the silence
    pull(engine, 480, 1'020'000, 480);             // the silence is over
    EXPECT_EQ(engine.clockFrames(1'020'000), 480);
    EXPECT_EQ(engine.clockFrames(1'025'000), 720);
    // Silence handed over without render counts the same way.
    engine.noteSilenceQueued(480);
    EXPECT_EQ(engine.clockFrames(1'025'000), 720);
}

// Jitter in the device position never moves the clock backwards.
TEST(AudioEngine, ClockNeverGoesBackwards) {
    AudioEngine engine;
    const auto pcm = ramp(4800);
    ASSERT_TRUE(engine.writePCM(pcm.data(), 4800, kCh, 1.0, engine.currentEpoch()));
    pull(engine, 1440, 1'000'000, 0);
    const int64_t before = engine.clockFrames(1'010'000); // 480
    pull(engine, 480, 1'010'000, 1440);                   // the device reports it played nothing
    EXPECT_GE(engine.clockFrames(1'010'000), before);
    EXPECT_GE(engine.clockFrames(1'012'000), before);
}
