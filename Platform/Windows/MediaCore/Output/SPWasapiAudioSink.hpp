// sp::AudioSink on WASAPI (shared mode, event driven), with the ring, clock,
// rate changes and limiter in sp::AudioEngine.
//
// A dedicated device thread owns every COM object: it renders on the
// endpoint's buffer event and executes control commands, so rendering and
// reconfiguration never race and callers need no particular COM apartment.
//
// The stream is 48 kHz float32 with the layout's channel mask (kSPCh* bits are
// the WAVEFORMATEXTENSIBLE SPEAKER_* bits) and AUTOCONVERTPCM, so Windows
// resamples and remaps to whatever the endpoint mixes at. 5.1 and 7.1 are
// chosen from the default endpoint's mix format with sp::resolveOutputLayout.
//
// When the default endpoint changes, a sink with an unchanged layout moves to
// the new device by itself, as the Mac default-output unit does; a changed
// layout is reported through the layout-change handler, on a thread of its
// own, and applied by applyPendingOutputLayout. A device that cannot be
// reopened is retried every second.
//
// The clock follows IAudioClock's play position, so it includes the latency
// past the endpoint buffer, holds through underruns, and stays put while
// paused: stop keeps what the endpoint holds, and start plays it first.
// reset flushes the endpoint, so a seek is heard, and clocked, at once.
#pragma once

#include "Player/SPAudioSink.hpp"

#include <memory>

namespace sp {

std::shared_ptr<AudioSink> makeWasapiAudioSink();

} // namespace sp
