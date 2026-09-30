// The UI thread, as seen by the media core. Player state that the UI reads,
// delegate callbacks and display-driven presentation all run there.
//
// The executor is process-wide, like the main dispatch queue it replaces:
// posting never captures the caller, so a task keeps exactly the captures it
// was written with. macOS installs a main-dispatch-queue executor
// (Platform/macOS/Player/SPDispatchExecutor.mm); Windows installs one bound to
// the UI thread's DispatcherQueue; tests install a manually pumped one.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace sp {

// A repeating UI-thread timer that keeps firing while the UI is tracking a
// menu or a live resize (NSRunLoopCommonModes on macOS). All methods are
// called on the UI thread.
class MainRepeatingTimer {
public:
    virtual ~MainRepeatingTimer() = default;
    // Stops firing until resumeNow().
    virtual void suspend() = 0;
    // Fires as soon as possible, then every interval from then on.
    virtual void resumeNow() = 0;
    // Stops for good and releases the handler.
    virtual void cancel() = 0;
};

class MainThreadExecutor {
public:
    virtual ~MainThreadExecutor() = default;

    // Runs `task` on the UI thread after everything posted before it, never
    // inline, even when called from the UI thread.
    virtual void post(std::function<void()> task) = 0;
    // Runs `task` on the UI thread no earlier than `delayUs` from now.
    virtual void postAfter(int64_t delayUs, std::function<void()> task) = 0;
    virtual bool isCurrent() const = 0;
    // First fires one interval from now. The timer holds the handler, and so
    // whatever it captures, until cancel().
    virtual std::shared_ptr<MainRepeatingTimer> makeRepeatingTimer(int64_t intervalUs,
                                                                   std::function<void()> handler) = 0;
};

// Installs the process-wide executor. Call once, before the first player is
// created; later calls are ignored so every task keeps one ordering.
void installMainThreadExecutor(std::unique_ptr<MainThreadExecutor> executor);
bool mainThreadExecutorInstalled();
// The installed executor. Using it before installation is a programming error
// and aborts.
MainThreadExecutor &mainThread();

} // namespace sp
