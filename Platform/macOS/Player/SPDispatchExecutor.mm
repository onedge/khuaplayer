#import "SPDispatchExecutor.h"

#import <Foundation/Foundation.h>

#include "Player/SPExecutor.hpp"
#include "Player/SPTasks.hpp"

#include <utility>

namespace {

// Exactly the calls the media core made before: dispatch_async and
// dispatch_after on the main queue, so ordering and timing are unchanged.
class DispatchMainThreadExecutor final : public sp::MainThreadExecutor {
public:
    void post(std::function<void()> task) override {
        dispatch_async(dispatch_get_main_queue(), ^{ task(); });
    }

    void postAfter(int64_t delayUs, std::function<void()> task) override {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delayUs * (int64_t)NSEC_PER_USEC),
                       dispatch_get_main_queue(), ^{ task(); });
    }

    bool isCurrent() const override { return [NSThread isMainThread]; }

    std::shared_ptr<sp::MainRepeatingTimer> makeRepeatingTimer(int64_t intervalUs,
                                                               std::function<void()> handler) override;
};

// An NSTimer on the main run loop in NSRunLoopCommonModes, as the audio-only
// clock tick was scheduled before. suspend/resumeNow move its fire date.
class RunLoopRepeatingTimer final : public sp::MainRepeatingTimer {
public:
    RunLoopRepeatingTimer(int64_t intervalUs, std::function<void()> handler) {
        timer_ = [NSTimer timerWithTimeInterval:(NSTimeInterval)intervalUs / 1e6
                                        repeats:YES
                                          block:^(NSTimer *) { handler(); }];
        [NSRunLoop.mainRunLoop addTimer:timer_ forMode:NSRunLoopCommonModes];
    }
    // Only cancel() invalidates, on the UI thread. Like the NSTimer ivar this
    // replaces, the last reference may be dropped on any thread; the run loop
    // keeps a timer that was never cancelled.

    void suspend() override { timer_.fireDate = NSDate.distantFuture; }
    void resumeNow() override { timer_.fireDate = [NSDate date]; }
    void cancel() override { [timer_ invalidate]; }

private:
    NSTimer *timer_;
};

std::shared_ptr<sp::MainRepeatingTimer>
DispatchMainThreadExecutor::makeRepeatingTimer(int64_t intervalUs, std::function<void()> handler) {
    return std::make_shared<RunLoopRepeatingTimer>(intervalUs, std::move(handler));
}

dispatch_qos_class_t darwinQos(sp::TaskQos qos) {
    switch (qos) {
    case sp::TaskQos::Background: return QOS_CLASS_BACKGROUND;
    case sp::TaskQos::Utility: return QOS_CLASS_UTILITY;
    case sp::TaskQos::Default: return QOS_CLASS_DEFAULT;
    case sp::TaskQos::UserInitiated: return QOS_CLASS_USER_INITIATED;
    case sp::TaskQos::UserInteractive: return QOS_CLASS_USER_INTERACTIVE;
    case sp::TaskQos::Unspecified: break;
    }
    return QOS_CLASS_UNSPECIFIED;
}

class DispatchSerialQueue final : public sp::SerialQueue {
public:
    explicit DispatchSerialQueue(dispatch_queue_t queue) : queue_(queue) {}
    void async(std::function<void()> task) override {
        dispatch_async(queue_, ^{ task(); });
    }

private:
    dispatch_queue_t queue_;
};

class DispatchRepeatingTimer final : public sp::RepeatingTimer {
public:
    explicit DispatchRepeatingTimer(dispatch_source_t source) : source_(source) {}
    // Dropping the last reference cancels the source, as the RepeatingTimer
    // contract says. The player always cancels explicitly before releasing it.
    ~DispatchRepeatingTimer() override { dispatch_source_cancel(source_); }
    void cancel() override { dispatch_source_cancel(source_); }

private:
    dispatch_source_t source_;
};

// The global queues, serial queues and dispatch sources the core used before,
// with the same QoS classes, start times, intervals and leeway.
class DispatchBackgroundTasks final : public sp::BackgroundTasks {
public:
    void runAsync(sp::TaskQos qos, std::function<void()> task) override {
        dispatch_async(dispatch_get_global_queue(darwinQos(qos), 0), ^{ task(); });
    }

    void runAfter(sp::TaskQos qos, int64_t delayUs, std::function<void()> task) override {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delayUs * (int64_t)NSEC_PER_USEC),
                       dispatch_get_global_queue(darwinQos(qos), 0), ^{ task(); });
    }

    std::shared_ptr<sp::SerialQueue> makeSerialQueue(const char *label, sp::TaskQos qos) override {
        dispatch_queue_attr_t attr = qos == sp::TaskQos::Unspecified
            ? DISPATCH_QUEUE_SERIAL
            : dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, darwinQos(qos), 0);
        return std::make_shared<DispatchSerialQueue>(dispatch_queue_create(label, attr));
    }

    std::shared_ptr<sp::RepeatingTimer> makeRepeatingTimer(sp::TaskQos qos, int64_t startUs,
                                                           int64_t intervalUs, int64_t leewayUs,
                                                           std::function<void()> handler) override {
        dispatch_source_t source = dispatch_source_create(
            DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_global_queue(darwinQos(qos), 0));
        dispatch_source_set_timer(source,
                                  dispatch_time(DISPATCH_TIME_NOW, startUs * (int64_t)NSEC_PER_USEC),
                                  (uint64_t)intervalUs * NSEC_PER_USEC,
                                  (uint64_t)leewayUs * NSEC_PER_USEC);
        dispatch_source_set_event_handler(source, ^{ handler(); });
        dispatch_resume(source);
        return std::make_shared<DispatchRepeatingTimer>(source);
    }
};

} // namespace

void SPInstallDispatchExecutors(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        if (!sp::mainThreadExecutorInstalled()) {
            sp::installMainThreadExecutor(std::make_unique<DispatchMainThreadExecutor>());
        }
        if (!sp::backgroundTasksInstalled()) {
            sp::installBackgroundTasks(std::make_unique<DispatchBackgroundTasks>());
        }
    });
}
