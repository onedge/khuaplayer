// Portable BackgroundTasks on std::thread: a worker per task, a worker per
// serial queue, and one scheduler thread for delayed tasks and timers.
// Background work in the player is coarse (probes, warm-ups, scans, the open
// path), so a thread per task is acceptable; a pooled implementation can
// replace this without changing callers.
#include "SPTasks.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace sp {
namespace {

using Clock = std::chrono::steady_clock;

void runDetached(TaskQos qos, std::function<void()> task) {
    std::thread([qos, task = std::move(task)] {
        if (qos != TaskQos::Unspecified) spfs::setCurrentThreadQos(qos);
        task();
    }).detach();
}

class ThreadSerialQueue final : public SerialQueue, public std::enable_shared_from_this<ThreadSerialQueue> {
public:
    ThreadSerialQueue(std::string label, TaskQos qos) : label_(std::move(label)), qos_(qos) {}

    void async(std::function<void()> task) override {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push_back(std::move(task));
        if (running_) return;
        running_ = true;
        // The worker holds the queue alive until the queue drains, as a
        // dispatch queue retains itself while it has work.
        std::thread([self = shared_from_this()] { self->drain(); }).detach();
    }

private:
    void drain() {
        spfs::setCurrentThreadName(label_.c_str());
        if (qos_ != TaskQos::Unspecified) spfs::setCurrentThreadQos(qos_);
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (tasks_.empty()) {
                    running_ = false;
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

    std::string label_;
    TaskQos qos_;
    std::mutex mutex_;
    std::deque<std::function<void()>> tasks_;
    bool running_ = false;
};

// One thread that waits for the next due time and hands work to runDetached.
class Scheduler {
public:
    using Id = uint64_t;

    Id schedule(Clock::time_point due, std::function<void()> fire) {
        std::lock_guard<std::mutex> lock(mutex_);
        const Id id = nextId_++;
        queue_.emplace(std::make_pair(due, id), std::move(fire));
        ensureThread();
        wake_.notify_one();
        return id;
    }

private:
    void ensureThread() {
        if (started_) return;
        started_ = true;
        std::thread([this] { loop(); }).detach(); // lives for the process
    }

    void loop() {
        spfs::setCurrentThreadName("sp.tasks.scheduler");
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            if (queue_.empty()) {
                wake_.wait(lock);
                continue;
            }
            const auto first = queue_.begin();
            if (Clock::now() < first->first.first) {
                wake_.wait_until(lock, first->first.first);
                continue;
            }
            std::function<void()> fire = std::move(first->second);
            queue_.erase(first);
            lock.unlock();
            fire();
            lock.lock();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::map<std::pair<Clock::time_point, Id>, std::function<void()>> queue_;
    Id nextId_ = 0;
    bool started_ = false;
};

Scheduler &scheduler() {
    static Scheduler *instance = new Scheduler; // never destroyed; its thread outlives main
    return *instance;
}

class ThreadRepeatingTimer final : public RepeatingTimer,
                                   public std::enable_shared_from_this<ThreadRepeatingTimer> {
public:
    ThreadRepeatingTimer(TaskQos qos, int64_t intervalUs, std::function<void()> handler)
        : qos_(qos), interval_(std::chrono::microseconds(std::max<int64_t>(1, intervalUs))),
          handler_(std::move(handler)) {}

    void start(Clock::time_point first) { arm(first); }

    // Like dispatch_source_cancel, cancelling releases the handler, which
    // breaks cycles through a handler that holds its own timer. A running
    // handler is released when it returns.
    void cancel() override {
        std::function<void()> released;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            cancelled_ = true;
            if (!running_) released = std::move(handler_);
        }
    }

private:
    void arm(Clock::time_point due) {
        std::weak_ptr<ThreadRepeatingTimer> weak = weak_from_this();
        scheduler().schedule(due, [weak, due] {
            if (auto self = weak.lock()) self->fire(due);
        });
    }

    // Handlers run one at a time, in order, like a dispatch source's.
    void fire(Clock::time_point due) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (cancelled_) return;
        }
        auto self = shared_from_this();
        runDetached(qos_, [self, due] {
            {
                std::lock_guard<std::mutex> lock(self->mutex_);
                if (self->cancelled_) return;
                self->running_ = true;
            }
            self->handler_();
            std::function<void()> released;
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->running_ = false;
            if (self->cancelled_) released = std::move(self->handler_);
            else self->arm(std::max(due + self->interval_, Clock::now()));
        });
    }

    TaskQos qos_;
    Clock::duration interval_;
    std::function<void()> handler_;
    std::mutex mutex_;
    bool cancelled_ = false;
    bool running_ = false;
};

class ThreadBackgroundTasks final : public BackgroundTasks {
public:
    void runAsync(TaskQos qos, std::function<void()> task) override { runDetached(qos, std::move(task)); }

    void runAfter(TaskQos qos, int64_t delayUs, std::function<void()> task) override {
        scheduler().schedule(Clock::now() + std::chrono::microseconds(std::max<int64_t>(0, delayUs)),
                             [qos, task = std::move(task)]() mutable { runDetached(qos, std::move(task)); });
    }

    std::shared_ptr<SerialQueue> makeSerialQueue(const char *label, TaskQos qos) override {
        return std::make_shared<ThreadSerialQueue>(label ? label : "sp.serial", qos);
    }

    std::shared_ptr<RepeatingTimer> makeRepeatingTimer(TaskQos qos, int64_t startUs, int64_t intervalUs,
                                                       int64_t, std::function<void()> handler) override {
        auto timer = std::make_shared<ThreadRepeatingTimer>(qos, intervalUs, std::move(handler));
        timer->start(Clock::now() + std::chrono::microseconds(std::max<int64_t>(0, startUs)));
        // Like a resumed dispatch source, the timer keeps firing while the
        // caller holds it; dropping the last reference stops it.
        return timer;
    }
};

} // namespace

std::unique_ptr<BackgroundTasks> makeThreadBackgroundTasks() {
    return std::make_unique<ThreadBackgroundTasks>();
}

} // namespace sp
