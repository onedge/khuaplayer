// The player core's audio output: a PCM ring with a device clock, epochs that
// fence writes across seeks, and pitch-preserving rate changes. macOS wraps
// SPAudioOutput (CoreAudio; Platform/macOS/Player/SPCoreAudioSink.mm);
// Windows will implement it on WASAPI.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace sp {

class AudioSink {
public:
    virtual ~AudioSink() = default;

    // Opens the device. false leaves the sink usable but silent.
    virtual bool setup() = 0;
    virtual void markSetupFailed() = 0;

    virtual uint64_t outputChannelMask() = 0; // kSPCh* bits, SPAudioChannelMap.hpp
    virtual int outputChannels() = 0;
    virtual bool outputLayoutChangePending() = 0;
    virtual bool applyPendingOutputLayout() = 0;
    // Called when the device layout changes, on any thread but the one that
    // renders audio. It may call back into the sink and wait.
    virtual void setOutputLayoutChangeHandler(std::function<void()> handler) = 0;
    virtual std::string outputLayoutDescription() = 0;
    virtual std::string outputLayoutName() = 0;

    virtual void start() = 0;
    virtual void stop() = 0;
    // Full media close only, after the writer was joined and stop called.
    virtual void releaseSessionScratch() = 0;
    virtual void reset() = 0;
    virtual void abortWrites() = 0;

    // Interleaved float PCM. Returns false when `epoch` is stale (a seek or
    // reset superseded the writer) or writes were aborted.
    virtual bool writePCM(const float *data, int frames, int channels, double rate, int32_t epoch) = 0;
    virtual int32_t currentEpoch() = 0;

    virtual void requestRate(double rate) = 0;
    virtual bool rateSwitchPending() = 0;
    virtual int64_t rateSwitchPlayedFrame() = 0;
    virtual void servicePendingRateSwitch(int32_t epoch) = 0;
    virtual void drainStretchAtEOF(int32_t epoch) = 0;

    // Frames played by the device (the media clock) and frames still queued.
    virtual int64_t clockFrames() = 0;
    virtual int64_t bufferedFrames() = 0;
    // Software gain 0...5 (1 = 100%).
    virtual void setVolume(float volume) = 0;
    virtual bool isRunning() = 0;
};

// How the core holds its sink: calls on an empty handle do nothing and return
// zero, false or an empty string, exactly like messaging a nil SPAudioOutput,
// so call sites keep their meaning without null checks.
class AudioSinkRef {
public:
    AudioSinkRef() = default;
    explicit AudioSinkRef(std::shared_ptr<AudioSink> sink) : sink_(std::move(sink)) {}

    explicit operator bool() const { return sink_ != nullptr; }
    AudioSink *get() const { return sink_.get(); }

    bool setup() const { return sink_ && sink_->setup(); }
    void markSetupFailed() const { if (sink_) sink_->markSetupFailed(); }
    uint64_t outputChannelMask() const { return sink_ ? sink_->outputChannelMask() : 0; }
    int outputChannels() const { return sink_ ? sink_->outputChannels() : 0; }
    bool outputLayoutChangePending() const { return sink_ && sink_->outputLayoutChangePending(); }
    bool applyPendingOutputLayout() const { return sink_ && sink_->applyPendingOutputLayout(); }
    void setOutputLayoutChangeHandler(std::function<void()> handler) const {
        if (sink_) sink_->setOutputLayoutChangeHandler(std::move(handler));
    }
    std::string outputLayoutDescription() const { return sink_ ? sink_->outputLayoutDescription() : std::string(); }
    std::string outputLayoutName() const { return sink_ ? sink_->outputLayoutName() : std::string(); }
    void start() const { if (sink_) sink_->start(); }
    void stop() const { if (sink_) sink_->stop(); }
    void releaseSessionScratch() const { if (sink_) sink_->releaseSessionScratch(); }
    void reset() const { if (sink_) sink_->reset(); }
    void abortWrites() const { if (sink_) sink_->abortWrites(); }
    bool writePCM(const float *data, int frames, int channels, double rate, int32_t epoch) const {
        return sink_ && sink_->writePCM(data, frames, channels, rate, epoch);
    }
    int32_t currentEpoch() const { return sink_ ? sink_->currentEpoch() : 0; }
    void requestRate(double rate) const { if (sink_) sink_->requestRate(rate); }
    bool rateSwitchPending() const { return sink_ && sink_->rateSwitchPending(); }
    int64_t rateSwitchPlayedFrame() const { return sink_ ? sink_->rateSwitchPlayedFrame() : 0; }
    void servicePendingRateSwitch(int32_t epoch) const { if (sink_) sink_->servicePendingRateSwitch(epoch); }
    void drainStretchAtEOF(int32_t epoch) const { if (sink_) sink_->drainStretchAtEOF(epoch); }
    int64_t clockFrames() const { return sink_ ? sink_->clockFrames() : 0; }
    int64_t bufferedFrames() const { return sink_ ? sink_->bufferedFrames() : 0; }
    void setVolume(float volume) const { if (sink_) sink_->setVolume(volume); }
    bool isRunning() const { return sink_ && sink_->isRunning(); }

private:
    std::shared_ptr<AudioSink> sink_;
};

} // namespace sp
