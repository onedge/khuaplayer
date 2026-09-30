// The device-independent part of the audio output: a single-producer,
// single-consumer PCM ring at 48 kHz, epochs that fence writes across seeks,
// pitch-preserving rate changes that regenerate already-queued audio, a gain
// ramp and zero-lookahead limiter, and the media clock.
//
// A port of the logic in the Mac player's SPAudioOutput.mm (the AudioUnit is
// replaced by a device shell such as SPWasapiAudioSink). Threads:
//   - one writer (the player's audio thread): writePCM, drainStretchAtEOF,
//     servicePendingRateSwitch;
//   - one renderer (the device thread): render;
//   - any thread: the controls and clock reads.
#pragma once

#include "SPTimeStretch.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace sp {

class AudioEngine {
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kMaxChannels = 8;
    static constexpr int kRingFrames = 24000;
    static constexpr int kHistFrames = kSampleRate * 3;
    static constexpr int kGainRampFrames = kSampleRate * 20 / 1000;

    AudioEngine();
    ~AudioEngine();
    AudioEngine(const AudioEngine &) = delete;
    AudioEngine &operator=(const AudioEngine &) = delete;

    // ---- Device shell -------------------------------------------------

    // Fills `frames` interleaved frames of `channels()` channels.
    // `queuedAheadFrames` of the frames handed to the device earlier (rendered
    // or noteSilenceQueued) had not been heard yet at `hostUs` on the spNowUs()
    // clock; they play first, then these. With 0, `hostUs` is when the first
    // of these frames is heard, as with a Core Audio render timestamp.
    // The engine remembers which queued frames are silence, so the clock
    // holds through an underrun instead of counting it.
    void render(float *out, int frames, int64_t hostUs, int64_t queuedAheadFrames = 0);
    // The rest are device thread only, like render.
    // The device was handed `frames` of silence without render.
    void noteSilenceQueued(int frames);
    // The device stopped at `nowUs` and keeps what it holds: hold the clock at
    // what has been heard instead of running through the queued frames.
    void freezeClock(int64_t nowUs);
    // The device discarded everything it had not played (a flush or a new
    // stream): those frames will never be heard, and the clock continues
    // from what it reported at `nowUs`.
    void discardDeviceQueue(int64_t nowUs);
    // Call immediately before starting the device, so changes made while
    // stopped begin from a fresh fade instead of stale render state.
    void noteDeviceStarting() { dspStartGeneration_.fetch_add(1, std::memory_order_release); }
    void setRunning(bool running);
    bool running() const { return running_.load(std::memory_order_acquire); }
    // The device is unusable: writers return false instead of blocking.
    void setDead(bool dead);
    bool dead() const { return dead_.load(); }

    // ---- AudioSink ----------------------------------------------------

    int channels() const { return channels_.load(std::memory_order_acquire); }
    // The negotiated channel count before the device is first set up, with no
    // writer yet: stores it without starting a new epoch.
    void setChannelsBeforeFirstUse(int channels) { channels_.store(channels, std::memory_order_release); }
    int32_t currentEpoch() const { return epoch_.load(); }
    // Starts a new epoch: queued audio is discarded, stale writers fail.
    // channels > 0 also changes the channel count.
    void reset(int channels = 0);
    // Wakes a writer blocked on a full ring and makes it return false.
    void abortWrites();
    void wakeWriters() { spaceCv_.notify_all(); }
    // Writer-thread scratch; call only after the writer was joined.
    void releaseSessionScratch();

    bool writePCM(const float *data, int frames, int channels, double rate, int32_t expectedEpoch);
    void drainStretchAtEOF(int32_t epoch);

    void requestRate(double rate);
    bool rateSwitchPending() const {
        return switchAppliedSeq_.load(std::memory_order_acquire) != switchSeq_.load(std::memory_order_acquire);
    }
    int64_t rateSwitchPlayedFrame() const { return switchPlayedFrame_.load(std::memory_order_acquire); }
    void servicePendingRateSwitch(int32_t epoch);

    // Real (non-silent) frames the device has played, interpolated to `nowUs`.
    int64_t clockFrames(int64_t nowUs) const;
    int64_t clockFrames() const;
    int64_t bufferedFrames() const;
    void setVolume(float volume);

private:
    struct RateSegment {
        int64_t outStart;
        int64_t inStart;
        double rate;
    };

    int adoptWriterChannels(int channels);
    int writerChannels();
    void noteSegmentForRateIfNeeded(int64_t packetInStart);
    void applyPendingRateSwitch(int32_t epoch);
    bool emitInput(const float *data, int frames, int32_t epoch);
    bool writeRaw(const float *data, int frames, int32_t epoch);
    void snapshotReadIdx(int64_t *readIdx, int64_t *played) const;
    void resetWriterForEpoch(int32_t epoch);
    void publishClock(int64_t hostUs, int64_t framesStart, int32_t leadSilence, int32_t framesReal,
                      int64_t readIdx, int64_t playedAtReadIdx);
    void queueSpan(int64_t frames, bool real);
    void consumeQueuedTo(int64_t queuedAheadFrames);

    // Ring.
    std::vector<float> ring_;
    std::atomic<int64_t> readIdx_{0};
    std::atomic<int64_t> writeIdx_{0};
    std::atomic<int64_t> playedFrames_{0};
    std::atomic<int64_t> discardUpTo_{-1};
    std::atomic<int> channels_{2};

    std::atomic<bool> running_{false};
    std::atomic<bool> writeAbort_{false};
    std::atomic<bool> dead_{false};
    std::atomic<int32_t> epoch_{0};
    std::mutex spaceMutex_;
    std::condition_variable spaceCv_;

    // Writer state; publishMutex_ orders it against reset and rate switches.
    std::mutex publishMutex_;
    double rate_ = 1.0;
    std::vector<float> rateScratch_;
    float resamplePos_ = 0.0f;
    std::unique_ptr<SPTimeStretcher> stretch_;
    bool stretchActive_ = false;
    std::vector<float> hist_;
    int64_t histTotal_ = 0;
    std::vector<RateSegment> segments_;
    int writerChannels_ = 0;
    int32_t lastSeenEpoch_ = 0;

    std::atomic<bool> switchPending_{false};
    std::atomic<uint32_t> switchSeq_{0};
    std::atomic<uint32_t> switchAppliedSeq_{0};
    std::atomic<int64_t> switchPlayedFrame_{-1};
    double switchRate_ = 1.0;
    std::atomic<int32_t> lastRenderFrames_{0};

    // Render-thread DSP state.
    std::atomic<float> targetGain_{1.0f};
    float gainCurrent_ = 0.0f; // Fade in the first start as well.
    float gainTargetSnapshot_ = 1.0f;
    float gainStep_ = 1.0f / kGainRampFrames;
    int32_t gainRampRemaining_ = kGainRampFrames;
    float limiterGain_ = 1.0f; // Channel-linked to preserve the image.
    int32_t dspEpoch_ = 0;
    std::atomic<uint32_t> dspStartGeneration_{0};
    uint32_t dspStartSeen_ = 0;

    // What the device holds and has not played, oldest first: runs of real
    // frames and of silence. Device thread only.
    struct QueuedSpan {
        int64_t frames;
        bool real;
    };
    static constexpr int kMaxSpans = 64;
    QueuedSpan spans_[kMaxSpans] = {};
    int spanHead_ = 0;
    int spanCount_ = 0;
    int64_t heardFrames_ = 0; // real frames the device has played

    // Clock seqlock, published by render. From hostUs the clock holds for
    // leadSilence frames, then advances through framesReal frames.
    // playedAtReadIdx is the played count through ring index readIdx.
    std::atomic<uint32_t> clkSeq_{0};
    std::atomic<int64_t> clkHostUs_{0};
    std::atomic<int64_t> clkFramesStart_{0};
    std::atomic<int32_t> clkLeadSilence_{0};
    std::atomic<int32_t> clkFramesReal_{0};
    std::atomic<int64_t> clkReadIdx_{0};
    std::atomic<int64_t> clkPlayedAtReadIdx_{0};
};

} // namespace sp
