#include "Player/SPSync.hpp"
#include "Player/SPTasks.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono;

namespace {

void installTasks() {
    static std::once_flag once;
    std::call_once(once, [] { sp::installBackgroundTasks(sp::makeThreadBackgroundTasks()); });
}

// Waits until `done` holds or a generous deadline passes.
template <class Pred> bool eventually(Pred done, milliseconds limit = milliseconds(5000)) {
    const auto deadline = steady_clock::now() + limit;
    while (!done()) {
        if (steady_clock::now() > deadline) return false;
        std::this_thread::sleep_for(milliseconds(2));
    }
    return true;
}

} // namespace

TEST(BackgroundTasks, RunAsyncRunsOffTheCallingThread) {
    installTasks();
    std::atomic<bool> ran{false};
    std::thread::id where;
    sp::backgroundTasks().runAsync(sp::TaskQos::Utility, [&] {
        where = std::this_thread::get_id();
        ran = true;
    });
    ASSERT_TRUE(eventually([&] { return ran.load(); }));
    EXPECT_NE(where, std::this_thread::get_id());
}

TEST(BackgroundTasks, RunAfterWaitsForTheDelay) {
    installTasks();
    std::atomic<bool> ran{false};
    const auto start = steady_clock::now();
    steady_clock::time_point at;
    sp::backgroundTasks().runAfter(sp::TaskQos::UserInitiated, 50'000, [&] {
        at = steady_clock::now();
        ran = true;
    });
    ASSERT_TRUE(eventually([&] { return ran.load(); }));
    EXPECT_GE(at - start, milliseconds(50));
}

TEST(BackgroundTasks, SerialQueueRunsInOrderOneAtATime) {
    installTasks();
    auto queue = sp::backgroundTasks().makeSerialQueue("sp.test.serial", sp::TaskQos::Utility);
    std::mutex mutex;
    std::vector<int> order;
    std::atomic<int> active{0}, maxActive{0};
    for (int i = 0; i < 50; ++i) {
        queue->async([&, i] {
            const int now = ++active;
            maxActive = std::max(maxActive.load(), now);
            std::this_thread::sleep_for(microseconds(200));
            {
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(i);
            }
            --active;
        });
    }
    ASSERT_TRUE(eventually([&] {
        std::lock_guard<std::mutex> lock(mutex);
        return order.size() == 50;
    }));
    EXPECT_EQ(maxActive.load(), 1);
    for (int i = 0; i < 50; ++i) EXPECT_EQ(order[(size_t)i], i);
}

// The index-wait timer: fires repeatedly and cancels itself from its handler.
TEST(BackgroundTasks, RepeatingTimerCanCancelItselfAndReleasesItsHandler) {
    installTasks();
    std::atomic<int> fired{0};
    auto token = std::make_shared<int>(0); // captured by the handler
    std::weak_ptr<int> handlerAlive = token;
    const auto self = std::make_shared<sp::TimerSelfRef>();
    auto timer = sp::backgroundTasks().makeRepeatingTimer(
        sp::TaskQos::Utility, 10'000, 10'000, 1'000, [&fired, self, token] {
            if (++fired == 3) self->cancel();
        });
    self->set(timer);
    token.reset();
    ASSERT_TRUE(eventually([&] { return fired.load() >= 3; }));
    std::this_thread::sleep_for(milliseconds(60));
    EXPECT_EQ(fired.load(), 3);
    EXPECT_TRUE(eventually([&] { return handlerAlive.expired(); }));
}

TEST(BackgroundTasks, SerialQueueRestartsAfterDraining) {
    installTasks();
    auto queue = sp::backgroundTasks().makeSerialQueue("sp.test.restart", sp::TaskQos::Unspecified);
    std::atomic<int> ran{0};
    queue->async([&] { ++ran; });
    ASSERT_TRUE(eventually([&] { return ran.load() == 1; }));
    std::this_thread::sleep_for(milliseconds(20)); // let the worker go idle
    queue->async([&] { ++ran; });
    EXPECT_TRUE(eventually([&] { return ran.load() == 2; }));
}

TEST(BackgroundTasks, DroppingTheLastReferenceStopsATimer) {
    installTasks();
    std::atomic<int> fired{0};
    auto timer = sp::backgroundTasks().makeRepeatingTimer(sp::TaskQos::Utility, 10'000, 10'000, 0,
                                                          [&] { ++fired; });
    ASSERT_TRUE(eventually([&] { return fired.load() >= 1; }));
    timer.reset();
    std::this_thread::sleep_for(milliseconds(30)); // a firing already under way may finish
    const int afterDrop = fired.load();
    std::this_thread::sleep_for(milliseconds(80));
    EXPECT_EQ(fired.load(), afterDrop);
}

TEST(BackgroundTasks, CancelledTimerNeverFires) {
    installTasks();
    std::atomic<int> fired{0};
    auto timer = sp::backgroundTasks().makeRepeatingTimer(sp::TaskQos::Utility, 30'000, 30'000, 0,
                                                          [&] { ++fired; });
    timer->cancel();
    std::this_thread::sleep_for(milliseconds(100));
    EXPECT_EQ(fired.load(), 0);
}

// The open path: a group entered by the renderer callback and two background
// tasks, and a semaphore that gates the speculative first frame.
TEST(Sync, WaitGroupWaitsForManualAndBackgroundMembers) {
    installTasks();
    auto group = std::make_shared<sp::WaitGroup>();
    std::atomic<int> done{0};
    group->enter();
    sp::runInGroup(group, sp::TaskQos::UserInitiated, [&] {
        std::this_thread::sleep_for(milliseconds(20));
        ++done;
    });
    sp::runInGroup(group, sp::TaskQos::UserInitiated, [&] { ++done; });
    std::thread([group, &done] {
        std::this_thread::sleep_for(milliseconds(30));
        ++done;
        group->leave();
    }).detach();
    group->wait();
    EXPECT_EQ(done.load(), 3);
}

TEST(Sync, SemaphoreCountsSignalsBeforeAndAfterWaiting) {
    sp::Semaphore gate(0);
    gate.signal();
    gate.wait(); // a signal before the wait is not lost
    std::atomic<bool> released{false};
    std::thread waiter([&] {
        gate.wait();
        released = true;
    });
    std::this_thread::sleep_for(milliseconds(20));
    EXPECT_FALSE(released.load());
    gate.signal();
    waiter.join();
    EXPECT_TRUE(released.load());
}
