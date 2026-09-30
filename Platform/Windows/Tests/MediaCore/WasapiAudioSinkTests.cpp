// Plays silence through the default endpoint. Skipped where there is none
// (CI machines without audio).
#include "SPWasapiAudioSink.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace std::chrono;

namespace {

std::shared_ptr<sp::AudioSink> openSink() {
    auto sink = sp::makeWasapiAudioSink();
    if (!sink->setup()) return nullptr;
    return sink;
}

} // namespace

TEST(WasapiAudioSink, NegotiatesALayoutForTheDefaultEndpoint) {
    auto sink = openSink();
    if (!sink) GTEST_SKIP() << "no audio endpoint";
    const int channels = sink->outputChannels();
    EXPECT_TRUE(channels == 2 || channels == 6 || channels == 8) << channels;
    EXPECT_NE(sink->outputChannelMask(), 0u);
    EXPECT_FALSE(sink->outputLayoutName().empty());
    std::printf("layout %s, %d channels\n", sink->outputLayoutDescription().c_str(), channels);
}

// The media clock follows the device: about real time while playing, and
// stopped when the stream stops.
TEST(WasapiAudioSink, ClockAdvancesInRealTimeWhilePlaying) {
    auto sink = openSink();
    if (!sink) GTEST_SKIP() << "no audio endpoint";
    const int ch = sink->outputChannels();
    const int32_t epoch = sink->currentEpoch();
    const std::vector<float> silence((size_t)48000 * ch, 0.0f); // one second
    sink->start();
    ASSERT_TRUE(sink->isRunning());
    std::thread writer([&] { sink->writePCM(silence.data(), 48000, ch, 1.0, epoch); });

    std::this_thread::sleep_for(milliseconds(100)); // let the stream settle
    const auto t0 = steady_clock::now();
    const int64_t f0 = sink->clockFrames();
    std::this_thread::sleep_for(milliseconds(300));
    const int64_t f1 = sink->clockFrames();
    const double elapsedMs = duration<double, std::milli>(steady_clock::now() - t0).count();
    const double playedMs = (f1 - f0) / 48.0;
    std::printf("played %.1f ms of audio in %.1f ms\n", playedMs, elapsedMs);
    EXPECT_NEAR(playedMs, elapsedMs, 40.0);

    sink->stop();
    EXPECT_FALSE(sink->isRunning());
    sink->abortWrites();
    writer.join();
    const int64_t stopped = sink->clockFrames();
    std::this_thread::sleep_for(milliseconds(100));
    EXPECT_EQ(sink->clockFrames(), stopped); // held at what was heard
}

TEST(WasapiAudioSink, StartBeforeSetupIsDeferredUntilSetup) {
    auto sink = sp::makeWasapiAudioSink();
    sink->start(); // as the player may do before the device is ready
    EXPECT_FALSE(sink->isRunning());
    if (!sink->setup()) GTEST_SKIP() << "no audio endpoint";
    EXPECT_TRUE(sink->isRunning());
    sink->stop();
    EXPECT_FALSE(sink->isRunning());
}

// Pausing keeps what the endpoint holds, so resuming neither skips audio nor
// moves the clock.
TEST(WasapiAudioSink, PauseAndResumeKeepTheClockContinuous) {
    auto sink = openSink();
    if (!sink) GTEST_SKIP() << "no audio endpoint";
    const int ch = sink->outputChannels();
    const int32_t epoch = sink->currentEpoch();
    const std::vector<float> silence((size_t)48000 * ch, 0.0f);
    std::thread writer([&] { sink->writePCM(silence.data(), 48000, ch, 1.0, epoch); });
    sink->start();
    std::this_thread::sleep_for(milliseconds(200));
    sink->stop();
    const int64_t paused = sink->clockFrames();
    std::this_thread::sleep_for(milliseconds(150));
    EXPECT_EQ(sink->clockFrames(), paused);
    sink->start();
    const int64_t resumed = sink->clockFrames();
    EXPECT_GE(resumed, paused);
    EXPECT_LE(resumed - paused, 480); // within 10 ms of where it stopped
    std::this_thread::sleep_for(milliseconds(200));
    const double playedMs = (sink->clockFrames() - resumed) / 48.0;
    std::printf("after resume played %.1f ms in ~200 ms\n", playedMs);
    EXPECT_NEAR(playedMs, 200.0, 40.0);
    sink->stop();
    sink->abortWrites();
    writer.join();
}

// A seek while playing: nothing of the old epoch is heard or clocked after
// reset, and the new epoch's audio advances the clock from where it stood.
TEST(WasapiAudioSink, ResetWhilePlayingFlushesTheEndpoint) {
    auto sink = openSink();
    if (!sink) GTEST_SKIP() << "no audio endpoint";
    const int ch = sink->outputChannels();
    const std::vector<float> silence((size_t)48000 * ch, 0.0f);
    int32_t epoch = sink->currentEpoch();
    std::thread writer([&] { sink->writePCM(silence.data(), 48000, ch, 1.0, epoch); });
    sink->start();
    std::this_thread::sleep_for(milliseconds(200));
    sink->abortWrites();
    writer.join();
    sink->reset();
    EXPECT_TRUE(sink->isRunning());
    const int64_t base = sink->clockFrames();
    std::this_thread::sleep_for(milliseconds(100));
    EXPECT_EQ(sink->clockFrames(), base); // the old epoch's queue is gone
    epoch = sink->currentEpoch();
    std::thread writer2([&] { sink->writePCM(silence.data(), 48000, ch, 1.0, epoch); });
    std::this_thread::sleep_for(milliseconds(200));
    const double playedMs = (sink->clockFrames() - base) / 48.0;
    std::printf("after reset played %.1f ms in ~200 ms (includes output latency)\n", playedMs);
    EXPECT_GT(playedMs, 100.0);
    EXPECT_LT(playedMs, 220.0);
    sink->stop();
    sink->abortWrites();
    writer2.join();
}
