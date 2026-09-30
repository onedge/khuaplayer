// Blocking synchronisation used by the player's open path, in place of
// dispatch_group_t and dispatch_semaphore_t. Hold them in a shared_ptr so the
// tasks that signal them keep them alive, as dispatch objects captured by
// blocks were.
#pragma once

#include "SPTasks.hpp"

#include <cassert>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

namespace sp {

// dispatch_group: enter/leave pairs, and wait until every entered task left.
class WaitGroup {
public:
    void enter() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++pending_;
    }
    void leave() {
        std::lock_guard<std::mutex> lock(mutex_);
        assert(pending_ > 0 && "WaitGroup::leave() without enter()"); // GCD would crash
        if (--pending_ == 0) done_.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        done_.wait(lock, [this] { return pending_ == 0; });
    }

private:
    std::mutex mutex_;
    std::condition_variable done_;
    int pending_ = 0;
};

// dispatch_group_async: runs `task` in the background as part of `group`.
inline void runInGroup(const std::shared_ptr<WaitGroup> &group, TaskQos qos,
                       std::function<void()> task) {
    group->enter();
    backgroundTasks().runAsync(qos, [group, task = std::move(task)] {
        task();
        group->leave();
    });
}

// dispatch_semaphore: a counting semaphore that starts at `initial`.
class Semaphore {
public:
    explicit Semaphore(long initial = 0) : count_(initial) {}
    void signal() {
        std::lock_guard<std::mutex> lock(mutex_);
        ++count_;
        available_.notify_one();
    }
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        available_.wait(lock, [this] { return count_ > 0; });
        --count_;
    }

private:
    std::mutex mutex_;
    std::condition_variable available_;
    long count_;
};

} // namespace sp
