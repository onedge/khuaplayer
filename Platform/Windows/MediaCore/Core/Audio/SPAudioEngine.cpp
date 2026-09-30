#include "SPAudioEngine.hpp"

#include "SPAudioPeak.hpp"
#include "SPRuntimeGates.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sp {
namespace {

// A ceiling of 1 preserves the bit-exact unity bypass; this is sample-peak
// protection, not true-peak limiting. Release toward unity takes 100 ms.
constexpr float kLimiterCeiling = 1.0f;
constexpr float kLimiterReleaseStep = 1.0f / (AudioEngine::kSampleRate * 0.100f);

bool pitchShiftResampler() {
    static const bool on = spAutomation() && std::getenv("SP_AUDIO_PITCHSHIFT") != nullptr;
    return on;
}

double validRate(double rate) { return (rate > 0.05 && rate <= 5.0) ? rate : 1.0; }

} // namespace

AudioEngine::AudioEngine() : ring_((size_t)kRingFrames * kMaxChannels, 0.0f) {}

AudioEngine::~AudioEngine() = default;

void AudioEngine::setRunning(bool running) {
    running_.store(running, std::memory_order_release);
    if (!running) spaceCv_.notify_all();
}

void AudioEngine::setDead(bool dead) {
    dead_.store(dead);
    spaceCv_.notify_all();
}

// ---------------------------------------------------------------------------
// Real-time rendering

void AudioEngine::render(float *out, int frames, int64_t hostUs, int64_t queuedAheadFrames) {
    const int ch = channels_.load(std::memory_order_relaxed);
    lastRenderFrames_.store(frames, std::memory_order_relaxed);
    int64_t readIdx = readIdx_.load(std::memory_order_acquire);
    const int64_t writeIdx = writeIdx_.load(std::memory_order_acquire);

    const int64_t disc = discardUpTo_.load(std::memory_order_acquire);
    if (disc > readIdx) {
        readIdx = disc > writeIdx ? writeIdx : disc;
        readIdx_.store(readIdx, std::memory_order_release);
    }
    const int64_t available = writeIdx - readIdx;
    int32_t n = available > frames ? frames : (int32_t)available;
    if (n < 0) n = 0;

    if (n > 0) {
        const int32_t start = (int32_t)(readIdx % kRingFrames);
        const int32_t first = n < kRingFrames - start ? n : kRingFrames - start;
        std::memcpy(out, &ring_[(size_t)start * ch], (size_t)first * ch * sizeof(float));
        if (n > first) std::memcpy(out + (size_t)first * ch, ring_.data(), (size_t)(n - first) * ch * sizeof(float));
    }
    if (n < frames) std::memset(out + (size_t)n * ch, 0, (size_t)(frames - n) * ch * sizeof(float));

    // Process every requested frame so limiter release advances through an
    // underrun, but advance the gain ramp only for actual media frames.
    const int32_t dspEpoch = epoch_.load(std::memory_order_acquire);
    const uint32_t dspStart = dspStartGeneration_.load(std::memory_order_acquire);
    const float requestedGain = targetGain_.load(std::memory_order_relaxed);
    if (dspEpoch != dspEpoch_ || dspStart != dspStartSeen_) {
        dspEpoch_ = dspEpoch;
        dspStartSeen_ = dspStart;
        gainCurrent_ = 0.0f;
        limiterGain_ = 1.0f;
        gainTargetSnapshot_ = requestedGain;
        gainRampRemaining_ = kGainRampFrames;
        gainStep_ = requestedGain / kGainRampFrames;
    } else if (requestedGain != gainTargetSnapshot_) {
        gainTargetSnapshot_ = requestedGain;
        gainRampRemaining_ = kGainRampFrames;
        gainStep_ = (requestedGain - gainCurrent_) / kGainRampFrames;
    }

    // Stable 100% playback is an exact bypass once the limiter is back at
    // unity, but only while nothing exceeds the ceiling: swresample's default
    // downmix is not normalised, so 5.1/7.1 content can exceed full scale.
    bool unityBypass = gainRampRemaining_ == 0 && gainCurrent_ == 1.0f && limiterGain_ == 1.0f;
    if (unityBypass) unityBypass = audioBlockWithinCeiling(out, (size_t)frames * ch, kLimiterCeiling);
    if (!unityBypass) {
        if (gainRampRemaining_ == 0 && gainCurrent_ == 0.0f && limiterGain_ == 1.0f) {
            std::memset(out, 0, (size_t)frames * ch * sizeof(float)); // stable mute
        } else {
            for (int i = 0; i < frames; i++) {
                if (i < n && gainRampRemaining_ > 0) {
                    gainCurrent_ += gainStep_;
                    // Remove accumulated error so unity reaches the exact bypass.
                    if (--gainRampRemaining_ == 0) gainCurrent_ = gainTargetSnapshot_;
                }
                const float gain = gainCurrent_;
                float *fr = out + (size_t)i * ch;
                float peak = 0.0f;
                for (int c = 0; c < ch; c++) {
                    float v = fr[c] * gain;
                    if (!std::isfinite(v)) v = 0.0f;
                    fr[c] = v;
                    peak = std::fmax(peak, std::fabs(v));
                }
                float required = 1.0f;
                if (peak > kLimiterCeiling) required = kLimiterCeiling / peak;
                if (required < limiterGain_) {
                    limiterGain_ = required; // zero-lookahead attack is immediate
                } else if (limiterGain_ < required) {
                    limiterGain_ += kLimiterReleaseStep;
                    if (limiterGain_ > required) limiterGain_ = required;
                }
                const float lg = limiterGain_;
                for (int c = 0; c < ch; c++) fr[c] *= lg;
            }
        }
    }
    readIdx_.store(readIdx + n, std::memory_order_release);

    // At hostUs the device has played all but the queued frames. It plays
    // those, then this block: n real frames and the underrun's silence.
    consumeQueuedTo(queuedAheadFrames);
    queueSpan(n, true);
    queueSpan(frames - n, false);
    // No wake-up from the device thread: a writer waiting for space polls
    // with a short timeout instead, keeping render free of system calls.
    const int64_t played = playedFrames_.fetch_add(n, std::memory_order_relaxed) + n;

    // The clock holds through leading silence, then runs through the real
    // frames up to the next silence; the next render refines it.
    int64_t lead = 0, real = 0;
    int i = 0;
    for (; i < spanCount_ && !spans_[(spanHead_ + i) % kMaxSpans].real; i++)
        lead += spans_[(spanHead_ + i) % kMaxSpans].frames;
    for (; i < spanCount_ && spans_[(spanHead_ + i) % kMaxSpans].real; i++)
        real += spans_[(spanHead_ + i) % kMaxSpans].frames;
    // Never report less than before: a read between renders may have run
    // ahead of the device by timing jitter. Hold until the device catches up.
    int64_t start = heardFrames_;
    if (clkHostUs_.load(std::memory_order_relaxed) > 0) {
        const int64_t reported = clockFrames(hostUs);
        if (reported > start) {
            const int64_t ahead = std::min(reported - start, real);
            start += ahead;
            lead += ahead;
            real -= ahead;
        }
    }
    publishClock(hostUs, start, (int32_t)lead, (int32_t)real, readIdx + n, played);
}

void AudioEngine::queueSpan(int64_t frames, bool real) {
    if (frames <= 0) return;
    if (spanCount_ > 0) {
        QueuedSpan &back = spans_[(spanHead_ + spanCount_ - 1) % kMaxSpans];
        if (back.real == real) {
            back.frames += frames;
            return;
        }
    }
    // Only a device holding dozens of underruns fills this; count the oldest
    // as heard rather than lose track of the newest.
    if (spanCount_ == kMaxSpans) {
        if (spans_[spanHead_].real) heardFrames_ += spans_[spanHead_].frames;
        spanHead_ = (spanHead_ + 1) % kMaxSpans;
        spanCount_--;
    }
    spans_[(spanHead_ + spanCount_) % kMaxSpans] = {frames, real};
    spanCount_++;
}

void AudioEngine::consumeQueuedTo(int64_t queuedAheadFrames) {
    int64_t queued = 0;
    for (int i = 0; i < spanCount_; i++) queued += spans_[(spanHead_ + i) % kMaxSpans].frames;
    int64_t played = queued - std::clamp<int64_t>(queuedAheadFrames, 0, queued);
    while (played > 0 && spanCount_ > 0) {
        QueuedSpan &front = spans_[spanHead_];
        const int64_t take = std::min(played, front.frames);
        if (front.real) heardFrames_ += take;
        front.frames -= take;
        played -= take;
        if (front.frames == 0) {
            spanHead_ = (spanHead_ + 1) % kMaxSpans;
            spanCount_--;
        }
    }
}

void AudioEngine::publishClock(int64_t hostUs, int64_t framesStart, int32_t leadSilence, int32_t framesReal,
                               int64_t readIdx, int64_t playedAtReadIdx) {
    const uint32_t s = clkSeq_.load(std::memory_order_relaxed);
    clkSeq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    clkHostUs_.store(hostUs, std::memory_order_relaxed);
    clkFramesStart_.store(framesStart, std::memory_order_relaxed);
    clkLeadSilence_.store(leadSilence, std::memory_order_relaxed);
    clkFramesReal_.store(framesReal, std::memory_order_relaxed);
    clkReadIdx_.store(readIdx, std::memory_order_relaxed);
    clkPlayedAtReadIdx_.store(playedAtReadIdx, std::memory_order_relaxed);
    clkSeq_.store(s + 2, std::memory_order_release);
}

void AudioEngine::noteSilenceQueued(int frames) { queueSpan(frames, false); }

void AudioEngine::freezeClock(int64_t nowUs) {
    const int64_t heard = clockFrames(nowUs);
    publishClock(nowUs, heard, 0, 0, clkReadIdx_.load(std::memory_order_relaxed),
                 clkPlayedAtReadIdx_.load(std::memory_order_relaxed));
}

void AudioEngine::discardDeviceQueue(int64_t nowUs) {
    // What the clock reported counts as heard; the rest is gone.
    const int64_t heard = std::min(clockFrames(nowUs), playedFrames_.load(std::memory_order_relaxed));
    spanHead_ = 0;
    spanCount_ = 0;
    heardFrames_ = heard;
    playedFrames_.store(heard, std::memory_order_relaxed);
    publishClock(nowUs, heard, 0, 0, readIdx_.load(std::memory_order_relaxed), heard);
}

// ---------------------------------------------------------------------------
// Controls

void AudioEngine::reset(int channels) {
    {
        std::lock_guard<std::mutex> lock(publishMutex_);
        epoch_.fetch_add(1);
        writeAbort_.store(false);
        discardUpTo_.store(writeIdx_.load(std::memory_order_acquire), std::memory_order_release);
        segments_.clear();
        switchPending_.store(false);
        switchAppliedSeq_.store(switchSeq_.load());
        switchPlayedFrame_.store(-1);
        if (channels > 0) channels_.store(channels, std::memory_order_release);
    }
    spaceCv_.notify_all();
}

void AudioEngine::abortWrites() {
    writeAbort_.store(true);
    spaceCv_.notify_all();
}

void AudioEngine::releaseSessionScratch() {
    std::vector<float>().swap(rateScratch_);
    std::vector<float>().swap(hist_);
    stretch_.reset();
    stretchActive_ = false;
    histTotal_ = 0;
    resamplePos_ = 0;
    writerChannels_ = 0;
    // The next open's reset owns epoch and segment invalidation.
}

void AudioEngine::requestRate(double rate) {
    const double newRate = validRate(rate);
    {
        std::lock_guard<std::mutex> lock(publishMutex_);
        switchRate_ = newRate;
        switchSeq_.fetch_add(1, std::memory_order_acq_rel);
        switchPending_.store(true, std::memory_order_release);
    }
    spaceCv_.notify_all();
}

void AudioEngine::resetWriterForEpoch(int32_t epoch) {
    if (epoch == lastSeenEpoch_) return;
    lastSeenEpoch_ = epoch;
    resamplePos_ = 0;
    if (stretch_) stretch_->reset();
    stretchActive_ = false;
    histTotal_ = 0;
}

void AudioEngine::servicePendingRateSwitch(int32_t epoch) {
    if (dead_.load() || !switchPending_.load(std::memory_order_acquire)) return;
    if (epoch != epoch_.load()) return;
    resetWriterForEpoch(epoch);
    applyPendingRateSwitch(epoch);
}

int64_t AudioEngine::clockFrames(int64_t nowUs) const {
    for (int i = 0; i < 8; i++) {
        const uint32_t s1 = clkSeq_.load(std::memory_order_acquire);
        if (s1 & 1) continue;
        const int64_t hostUs = clkHostUs_.load(std::memory_order_relaxed);
        const int64_t start = clkFramesStart_.load(std::memory_order_relaxed);
        const int32_t lead = clkLeadSilence_.load(std::memory_order_relaxed);
        const int32_t real = clkFramesReal_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (clkSeq_.load(std::memory_order_relaxed) != s1) continue;
        if (hostUs <= 0) break;
        int64_t adv = (nowUs - hostUs) * kSampleRate / 1000000 - lead;
        if (adv < 0) adv = 0;
        if (adv > real) adv = real;
        return start + adv;
    }
    return playedFrames_.load();
}

int64_t AudioEngine::clockFrames() const { return clockFrames(spNowUs()); }

void AudioEngine::snapshotReadIdx(int64_t *readIdx, int64_t *played) const {
    for (int i = 0; i < 8; i++) {
        const uint32_t s1 = clkSeq_.load(std::memory_order_acquire);
        if (s1 & 1) continue;
        const int64_t r = clkReadIdx_.load(std::memory_order_relaxed);
        const int64_t p = clkPlayedAtReadIdx_.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (clkSeq_.load(std::memory_order_relaxed) != s1) continue;
        *readIdx = r;
        *played = p;
        return;
    }
    *readIdx = readIdx_.load(std::memory_order_acquire);
    *played = playedFrames_.load(std::memory_order_relaxed);
}

int64_t AudioEngine::bufferedFrames() const {
    int64_t r = readIdx_.load(std::memory_order_acquire);
    const int64_t disc = discardUpTo_.load(std::memory_order_acquire);
    if (disc > r) r = disc;
    const int64_t d = writeIdx_.load(std::memory_order_acquire) - r;
    return d > 0 ? d : 0;
}

void AudioEngine::setVolume(float volume) {
    // Keep the last valid value rather than publishing a non-finite gain.
    if (!std::isfinite(volume)) return;
    volume = std::clamp(volume, 0.0f, 5.0f);
    targetGain_.store(volume, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Blocking writer

int AudioEngine::adoptWriterChannels(int channels) {
    if (writerChannels_ == channels) return channels;
    std::vector<float>().swap(hist_);
    std::vector<float>().swap(rateScratch_);
    stretch_.reset();
    stretchActive_ = false;
    histTotal_ = 0;
    resamplePos_ = 0;
    writerChannels_ = channels;
    return channels;
}

int AudioEngine::writerChannels() {
    return writerChannels_ ? writerChannels_ : adoptWriterChannels(channels_.load(std::memory_order_acquire));
}

bool AudioEngine::writePCM(const float *data, int count, int channels, double rate, int32_t expected) {
    if (count <= 0 || dead_.load()) return false;
    if (channels != channels_.load(std::memory_order_acquire)) return false;
    const int32_t epoch = epoch_.load();
    if (epoch != expected) return false;
    const int ch = adoptWriterChannels(channels);
    resetWriterForEpoch(epoch);

    if (switchPending_.load(std::memory_order_acquire)) {
        applyPendingRateSwitch(epoch);
        if (epoch_.load() != epoch) return false;
    }
    const int64_t packetInStart = histTotal_;

    // Keep the last three seconds of input so a rate change can regenerate
    // audio that is already queued at the old rate.
    if (hist_.empty()) hist_.assign((size_t)kHistFrames * ch, 0.0f);
    {
        const float *src = data;
        int keep = count;
        if (keep > kHistFrames) {
            src += (size_t)(keep - kHistFrames) * ch;
            keep = kHistFrames;
        }
        const int64_t pos = (histTotal_ + (count - keep)) % kHistFrames;
        const int first = (int)std::min<int64_t>(keep, kHistFrames - pos);
        std::memcpy(hist_.data() + pos * ch, src, (size_t)first * ch * sizeof(float));
        if (keep > first) std::memcpy(hist_.data(), src + (size_t)first * ch, (size_t)(keep - first) * ch * sizeof(float));
        histTotal_ += count;
    }
    {
        std::lock_guard<std::mutex> lock(publishMutex_);
        if (switchSeq_.load(std::memory_order_acquire) != 0) rate = switchRate_;
    }
    rate_ = validRate(rate);
    noteSegmentForRateIfNeeded(packetInStart);
    if (!emitInput(data, count, epoch)) applyPendingRateSwitch(epoch);
    return true;
}

void AudioEngine::noteSegmentForRateIfNeeded(int64_t packetInStart) {
    std::lock_guard<std::mutex> lock(publishMutex_);
    if (!segments_.empty() && segments_.back().rate == rate_) return;
    const int64_t pending = (stretchActive_ && stretch_) ? stretch_->pendingInputFrames() : 0;
    segments_.push_back({writeIdx_.load(std::memory_order_relaxed), packetInStart - pending, rate_});
    while (segments_.size() > 64) segments_.erase(segments_.begin());
}

void AudioEngine::drainStretchAtEOF(int32_t epoch) {
    if (dead_.load() || epoch != epoch_.load() || !stretchActive_ || !stretch_) return;
    stretch_->finish([&](const float *out, int n) { return writeRaw(out, n, epoch); });
    stretchActive_ = false;
}

void AudioEngine::applyPendingRateSwitch(int32_t epoch) {
    while (switchPending_.exchange(false, std::memory_order_acq_rel)) {
        double newRate;
        int64_t inPos = -1, outPos = 0, r = 0;
        {
            std::lock_guard<std::mutex> lock(publishMutex_);
            if (epoch_.load() != epoch) {
                if (switchAppliedSeq_.load(std::memory_order_acquire) != switchSeq_.load(std::memory_order_acquire))
                    switchPending_.store(true, std::memory_order_release);
                return;
            }
            newRate = switchRate_;
            const uint32_t seq = switchSeq_.load(std::memory_order_acquire);
            const int64_t w = writeIdx_.load(std::memory_order_acquire);

            int64_t clkR = 0, clkPlayed = 0;
            snapshotReadIdx(&clkR, &clkPlayed);
            const int64_t disc = discardUpTo_.load(std::memory_order_acquire);
            int64_t base = std::max(clkR, disc);
            if (base > w) base = w;

            r = std::max(readIdx_.load(std::memory_order_acquire), base);
            if (r > w) r = w;
            int64_t margin = 0;
            if (running_.load(std::memory_order_acquire)) {
                int32_t blk = lastRenderFrames_.load(std::memory_order_relaxed);
                if (blk <= 0) blk = 1024;
                margin = 2 * (int64_t)blk + 256;
            }
            outPos = r + margin;
            if (outPos > w) outPos = w;

            // Keep what the device is about to play; cut the rest and
            // regenerate it at the new rate from the input history.
            if (outPos < w) writeIdx_.store(outPos, std::memory_order_release);
            for (size_t i = segments_.size(); i > 0; i--) {
                const RateSegment &seg = segments_[i - 1];
                if (seg.outStart <= outPos) {
                    inPos = seg.inStart + (int64_t)std::llround((double)(outPos - seg.outStart) * seg.rate);
                    break;
                }
            }
            while (!segments_.empty() && segments_.back().outStart >= outPos && outPos < w) segments_.pop_back();

            switchPlayedFrame_.store(clkPlayed + (outPos - base), std::memory_order_release);
            switchAppliedSeq_.store(seq, std::memory_order_release);
        }
        rate_ = newRate;
        resamplePos_ = 0;
        if (stretch_) stretch_->reset();
        stretchActive_ = false;
        if (inPos < 0) {
            if (spDebug()) std::fprintf(stderr, "[Audio] rate %.2fx: nothing queued to regenerate\n", rate_);
            noteSegmentForRateIfNeeded(histTotal_);
            continue;
        }
        const int64_t oldest = std::max<int64_t>(0, histTotal_ - kHistFrames);
        inPos = std::clamp(inPos, oldest, histTotal_);

        const int ch = writerChannels();
        if (newRate != 1.0 && !pitchShiftResampler()) {
            if (!stretch_) stretch_ = std::make_unique<SPTimeStretcher>(ch, kSampleRate);
            if (inPos > oldest && !hist_.empty()) {
                const int ov = stretch_->overlapFrames();
                const int64_t from = std::max<int64_t>(oldest, inPos - ov);
                const int64_t n = inPos - from;
                const int64_t pos = from % kHistFrames;
                const int64_t first = std::min<int64_t>(n, kHistFrames - pos);
                stretch_->primeTail(hist_.data() + pos * ch, (int)first);
                if (n > first) stretch_->primeTail(hist_.data(), (int)(n - first));
            }
        }
        {
            std::lock_guard<std::mutex> lock(publishMutex_);
            if (epoch_.load() != epoch) return;
            segments_.push_back({outPos, inPos, rate_});
        }
        if (spDebug()) {
            std::fprintf(stderr, "[Audio] rate %.2fx: margin=%lld regenerate %lld input frames (cut %lld, read %lld, write %lld)\n",
                         rate_, (long long)(outPos - r), (long long)(histTotal_ - inPos), (long long)outPos,
                         (long long)readIdx_.load(), (long long)writeIdx_.load());
        }
        bool ok = !hist_.empty();
        for (int64_t pos = inPos; pos < histTotal_ && ok;) {
            const int64_t off = pos % kHistFrames;
            const int n = (int)std::min<int64_t>({(int64_t)4096, histTotal_ - pos, kHistFrames - off});
            ok = emitInput(hist_.data() + off * ch, n, epoch);
            pos += n;
            if (switchPending_.load(std::memory_order_acquire)) break;
        }
    }
}

bool AudioEngine::emitInput(const float *data, int count, int32_t epoch) {
    const int ch = writerChannels();
    if (rate_ == 1.0) {
        if (stretchActive_) {
            bool ok = true;
            stretch_->flushRaw([&](const float *out, int n) {
                ok = ok && writeRaw(out, n, epoch);
                return ok;
            });
            stretchActive_ = false;
            if (!ok) return false;
        }
        const bool ok = writeRaw(data, count, epoch);
        if (stretch_) stretch_->primeTail(data, count);
        return ok;
    }
    if (!pitchShiftResampler()) {
        if (!stretch_) stretch_ = std::make_unique<SPTimeStretcher>(ch, kSampleRate);
        stretch_->setRate(rate_);
        stretchActive_ = true;
        bool ok = true;
        stretch_->process(data, count, [&](const float *out, int n) {
            ok = writeRaw(out, n, epoch);
            return ok;
        });
        return ok;
    }

    // Debug-only linear resampler (SP_AUDIO_PITCHSHIFT): pitch follows rate.
    const int outCount = (int)std::ceil((count - resamplePos_) / rate_);
    if (outCount <= 0) {
        resamplePos_ -= count;
        return true;
    }
    constexpr int kChunk = 4096;
    if (rateScratch_.empty()) rateScratch_.assign((size_t)kChunk * ch, 0.0f);
    float *tmp = rateScratch_.data();
    int produced = 0;
    while (produced < outCount) {
        const int n = std::min(kChunk, outCount - produced);
        for (int i = 0; i < n; i++) {
            const float pos = (float)(resamplePos_ + (produced + i) * rate_);
            const int p0 = std::clamp((int)pos, 0, count - 1);
            const int p1 = std::clamp((int)pos + 1, 0, count - 1);
            const float frac = pos - (float)(int)pos;
            for (int c = 0; c < ch; c++) {
                const float v0 = data[(size_t)p0 * ch + c];
                const float v1 = data[(size_t)p1 * ch + c];
                tmp[(size_t)i * ch + c] = v0 + (v1 - v0) * frac;
            }
        }
        if (!writeRaw(tmp, n, epoch)) return false;
        produced += n;
    }
    resamplePos_ = (float)(resamplePos_ + outCount * rate_ - count);
    return true;
}

bool AudioEngine::writeRaw(const float *data, int count, int32_t epoch) {
    int written = 0;
    const int ch = writerChannels();
    if (ch != channels_.load(std::memory_order_acquire)) return false;
    while (written < count) {
        if (dead_.load() || writeAbort_.load() || epoch_.load() != epoch) return false;
        if (switchPending_.load(std::memory_order_acquire)) return false;
        const int64_t w = writeIdx_.load(std::memory_order_relaxed);
        int64_t r = readIdx_.load(std::memory_order_acquire);
        const int64_t disc = discardUpTo_.load(std::memory_order_acquire);
        if (disc > r && !running_.load(std::memory_order_acquire)) r = disc;
        int64_t occ = w - r;
        occ = std::clamp<int64_t>(occ, 0, kRingFrames);
        const int32_t space = kRingFrames - 1 - (int32_t)occ;
        if (space <= 0) {
            int32_t need = std::min(count - written, kRingFrames / 2);
            const int64_t waitMs = std::clamp<int64_t>(need / 48 + 1, 10, 250);
            std::unique_lock<std::mutex> lock(spaceMutex_);
            spaceCv_.wait_for(lock, std::chrono::milliseconds(waitMs));
            continue;
        }
        const int32_t n = std::min(count - written, space);
        const int32_t start = (int32_t)(w % kRingFrames);
        const int32_t first = n < kRingFrames - start ? n : kRingFrames - start;
        std::memcpy(&ring_[(size_t)start * ch], data + (size_t)written * ch, (size_t)first * ch * sizeof(float));
        if (n > first) std::memcpy(ring_.data(), data + (size_t)(written + first) * ch, (size_t)(n - first) * ch * sizeof(float));
        {
            std::lock_guard<std::mutex> lock(publishMutex_);
            if (epoch_.load() != epoch) return false;
            if (ch != channels_.load(std::memory_order_relaxed)) return false;
            if (switchPending_.load(std::memory_order_acquire)) return false;
            writeIdx_.store(w + n, std::memory_order_release);
        }
        written += n;
    }
    return true;
}

} // namespace sp
