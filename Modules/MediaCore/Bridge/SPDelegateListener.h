// sp::PlayerListener that forwards to an SPPlayerCore's delegate, exactly as the
// core messaged its delegate before: the delegate is read at each call and
// only messaged when it responds to the selector.
#pragma once

#include "Player/SPPlayerListener.hpp"

#include <memory>

@class SPPlayerCore;

// `core` owns the listener and outlives it; the listener does not retain it,
// so events raised while the core deallocates (stop from dealloc) still reach
// the delegate with the core as sender, as before.
std::unique_ptr<sp::PlayerListener> SPMakeDelegateListener(SPPlayerCore *core);
