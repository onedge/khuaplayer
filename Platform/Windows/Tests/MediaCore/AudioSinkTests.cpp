#include "Player/SPAudioSink.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <vector>

namespace {

// Records calls and returns fixed values.
class FakeSink final : public sp::AudioSink {
public:
    bool setup() override { return true; }
    void markSetupFailed() override { calls.push_back("markSetupFailed"); }
    uint64_t outputChannelMask() override { return 0x3f; }
    int outputChannels() override { return 6; }
    bool outputLayoutChangePending() override { return true; }
    bool applyPendingOutputLayout() override { return true; }
    void setOutputLayoutChangeHandler(std::function<void()> h) override { handler = std::move(h); }
    std::string outputLayoutDescription() override { return "5.1/6"; }
    std::string outputLayoutName() override { return "5.1"; }
    void start() override { calls.push_back("start"); }
    void stop() override { calls.push_back("stop"); }
    void releaseSessionScratch() override {}
    void reset() override {}
    void abortWrites() override {}
    bool writePCM(const float *, int frames, int channels, double, int32_t epoch) override {
        written += frames * channels;
        return epoch == 7;
    }
    int32_t currentEpoch() override { return 7; }
    void requestRate(double r) override { rate = r; }
    bool rateSwitchPending() override { return false; }
    int64_t rateSwitchPlayedFrame() override { return 0; }
    void servicePendingRateSwitch(int32_t) override {}
    void drainStretchAtEOF(int32_t) override {}
    int64_t clockFrames() override { return 48000; }
    int64_t bufferedFrames() override { return 1024; }
    void setVolume(float v) override { volume = v; }
    bool isRunning() override { return true; }

    std::vector<std::string> calls;
    std::function<void()> handler;
    int written = 0;
    double rate = 1.0;
    float volume = 1.0f;
};

} // namespace

// SPPlayerCore messaged a possibly nil SPAudioOutput without checks; the
// handle must answer exactly as nil did.
TEST(AudioSinkRef, EmptyHandleBehavesLikeNil) {
    const sp::AudioSinkRef none;
    EXPECT_FALSE(none);
    EXPECT_FALSE(none.setup());
    EXPECT_EQ(none.outputChannelMask(), 0u);
    EXPECT_EQ(none.outputChannels(), 0);
    EXPECT_FALSE(none.outputLayoutChangePending());
    EXPECT_EQ(none.outputLayoutName(), "");
    EXPECT_FALSE(none.writePCM(nullptr, 10, 2, 1.0, 0));
    EXPECT_EQ(none.currentEpoch(), 0);
    EXPECT_EQ(none.clockFrames(), 0);
    EXPECT_EQ(none.bufferedFrames(), 0);
    EXPECT_FALSE(none.isRunning());
    none.start();
    none.stop();
    none.setVolume(0.5f);
    none.requestRate(2.0);
    none.setOutputLayoutChangeHandler([] {});
}

TEST(AudioSinkRef, ForwardsToTheSink) {
    auto fake = std::make_shared<FakeSink>();
    const sp::AudioSinkRef sink(fake);
    const sp::AudioSinkRef copy = sink; // copies share one sink
    EXPECT_TRUE(copy);
    EXPECT_EQ(copy.get(), fake.get());
    sink.start();
    copy.stop();
    EXPECT_EQ(fake->calls, (std::vector<std::string>{"start", "stop"}));
    EXPECT_TRUE(sink.writePCM(nullptr, 100, 6, 1.0, 7));
    EXPECT_FALSE(sink.writePCM(nullptr, 100, 6, 1.0, 6)); // stale epoch
    EXPECT_EQ(fake->written, 1200);
    sink.setVolume(0.25f);
    sink.requestRate(1.5);
    EXPECT_EQ(fake->volume, 0.25f);
    EXPECT_EQ(fake->rate, 1.5);
    EXPECT_EQ(sink.outputLayoutDescription(), "5.1/6");
    EXPECT_EQ(sink.clockFrames(), 48000);
    EXPECT_TRUE(sink.isRunning());
    bool fired = false;
    sink.setOutputLayoutChangeHandler([&] { fired = true; });
    fake->handler();
    EXPECT_TRUE(fired);
}
