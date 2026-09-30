// Events the player core reports to its host. Every method is called on the UI
// thread (sp::mainThread()), synchronously and possibly reentrantly: a handler
// may call back into the player. The core never holds a lock while calling.
// On macOS the listener forwards to SPPlayerCoreDelegate
// (Platform/macOS/Player/SPDelegateListener.mm).
#pragma once

#include <cstdint>
#include <string>

namespace sp {

// Same values as SPPlayerState.
enum class PlayerState : int {
    Idle = 0,
    Opening,
    Ready,
    Playing,
    Paused,
    Ended,
    Failed,
};

struct PlayerError {
    std::string domain;      // "SPDemuxerError", "SPDecodeError", "KhuaPlayer"
    int64_t code = 0;
    std::string description; // Localized, UTF-8.
    std::string phase;       // open / decode / read / subtitle
    std::string diagnosis;   // zeroHead, decodeFailed, readWarning, subtitleLoad, ...
    bool terminal = false;
};

class PlayerListener {
public:
    virtual ~PlayerListener() = default;

    virtual void didChangeState(PlayerState state) = 0;
    virtual void didUpdatePosition(double positionSec, double durationSec) = 0;
    virtual void didFail(const PlayerError &error) = 0;
    virtual void didChangeFrameInterpolation() = 0;
    // A new timeline preview or damage snapshot is available.
    virtual void didUpdateTimelinePreview() = 0;
    virtual void didChangeDecoder() = 0;
    virtual void didChangeXDRAvailability() = 0;
    virtual void didChangeSourceGrowth() = 0;
    virtual void didSkipMissingContent(double fromSec, double toSec, bool afterSeek) = 0;
    virtual void waitedSourceBecameReady() = 0;
};

} // namespace sp
