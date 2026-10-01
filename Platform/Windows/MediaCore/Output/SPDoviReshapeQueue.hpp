// The Mac renderer's Dolby Vision RPU ring (SPMetalRenderer.mm): metadata is
// queued by packet timestamp on the demux thread and bound by decoded-frame
// timestamp just before rendering. Not thread-safe; the renderer locks it.
#pragma once

#include "SPDoviRPU.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace sp {

inline constexpr int kSPDoviFloatCount = 232;

class DoviReshapeQueue {
public:
    DoviReshapeQueue() { reset(); }

    // The floats the next frame renders with.
    const float *current() const { return current_; }

    // A negative timestamp applies the values at once.
    void queue(const float *data, int64_t ptsUs) {
        if (ptsUs < 0) {
            std::memcpy(current_, data, sizeof(current_));
            return;
        }
        Entry &e = entries_[head_];
        e.ptsUs = ptsUs;
        std::memcpy(e.values, data, sizeof(e.values));
        head_ = (head_ + 1) % kCapacity;
        if (count_ < kCapacity) count_++;
    }

    // Takes the newest entry not later than a quarter frame after `ptsUs`
    // and consumes the older ones; with no match the previous RPU carries on.
    // Returns the bound entry's timestamp, or INT64_MIN.
    int64_t bind(int64_t ptsUs, int64_t frameIntervalUs) {
        if (count_ == 0) return INT64_MIN;
        const int64_t limit = ptsUs + std::max<int64_t>(0, frameIntervalUs / 4);
        int best = -1;
        int64_t bestPts = INT64_MIN;
        for (int i = 0; i < count_; i++) {
            const int idx = (head_ - 1 - i + 2 * kCapacity) % kCapacity;
            if (entries_[idx].ptsUs <= limit && entries_[idx].ptsUs > bestPts) {
                best = idx;
                bestPts = entries_[idx].ptsUs;
            }
        }
        if (best < 0) return INT64_MIN;
        std::memcpy(current_, entries_[best].values, sizeof(current_));
        for (int i = 0; i < count_; i++) {
            const int idx = (head_ - 1 - i + 2 * kCapacity) % kCapacity;
            if (entries_[idx].ptsUs < bestPts) entries_[idx].ptsUs = INT64_MAX;
        }
        return bestPts;
    }

    // Seek: forget queued entries, keep the current values.
    void clear() {
        head_ = 0;
        count_ = 0;
    }

    // Session boundary: also restore the defaults.
    void reset() {
        clear();
        DoviReshape defaults;
        doviToGpuFloats(defaults, current_);
    }

private:
    static constexpr int kCapacity = 32;
    struct Entry {
        int64_t ptsUs;
        float values[kSPDoviFloatCount];
    };
    Entry entries_[kCapacity];
    int head_ = 0;
    int count_ = 0;
    float current_[kSPDoviFloatCount];
};

} // namespace sp
