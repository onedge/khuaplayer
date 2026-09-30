// Background work for the media core: one-off tasks at a QoS, serial queues
// and repeating timers. Like SPExecutor.hpp this is process-wide, so posting
// never captures the caller. macOS installs a GCD implementation that makes
// the calls the core made before (Platform/macOS/Player/SPDispatchExecutor.mm);
// other platforms use SPThreadTasks.cpp.
#pragma once

#include "Platform/SPThread.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace sp {

using TaskQos = spfs::ThreadQos;

// A FIFO queue that runs one task at a time.
class SerialQueue {
public:
    virtual ~SerialQueue() = default;
    virtual void async(std::function<void()> task) = 0;
};

// Fires repeatedly until cancelled or until the last reference is dropped.
// cancel() may be called from any thread, including from inside the handler;
// no call starts after it returns, but a call already running on another
// thread finishes. Cancelling releases the handler.
class RepeatingTimer {
public:
    virtual ~RepeatingTimer() = default;
    virtual void cancel() = 0;
};

// Lets a timer's handler cancel its own timer. The handler holds this by
// shared_ptr; set() is called once the timer exists, and a handler that runs
// earlier finds nothing to cancel.
class TimerSelfRef {
public:
    void set(const std::shared_ptr<RepeatingTimer> &timer) {
        std::lock_guard<std::mutex> lock(mutex_);
        timer_ = timer;
    }
    void cancel() {
        std::shared_ptr<RepeatingTimer> timer;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            timer = timer_.lock();
        }
        if (timer) timer->cancel();
    }

private:
    std::mutex mutex_;
    std::weak_ptr<RepeatingTimer> timer_;
};

class BackgroundTasks {
public:
    virtual ~BackgroundTasks() = default;

    virtual void runAsync(TaskQos qos, std::function<void()> task) = 0;
    virtual void runAfter(TaskQos qos, int64_t delayUs, std::function<void()> task) = 0;
    // qos Unspecified gives a queue without its own QoS, which inherits it
    // from the work submitted to it.
    virtual std::shared_ptr<SerialQueue> makeSerialQueue(const char *label, TaskQos qos) = 0;
    // First fires `startUs` from now, then every `intervalUs`; the platform
    // may defer each firing by up to `leewayUs` to batch wake-ups.
    virtual std::shared_ptr<RepeatingTimer> makeRepeatingTimer(TaskQos qos, int64_t startUs,
                                                               int64_t intervalUs, int64_t leewayUs,
                                                               std::function<void()> handler) = 0;
};

// Installs the process-wide implementation once; later calls are ignored.
void installBackgroundTasks(std::unique_ptr<BackgroundTasks> tasks);
bool backgroundTasksInstalled();
// The installed implementation. Using it before installation aborts.
BackgroundTasks &backgroundTasks();

// The portable std::thread implementation (SPThreadTasks.cpp).
std::unique_ptr<BackgroundTasks> makeThreadBackgroundTasks();

} // namespace sp
