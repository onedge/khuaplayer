#include "Player/SPExecutor.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <thread>
#include <vector>

namespace {

// A UI thread for tests: tasks run only when the test pumps them, on the
// thread that pumps, in the order the executor contract requires.
class ManualExecutor final : public sp::MainThreadExecutor {
public:
    void post(std::function<void()> task) override { postAfter(0, std::move(task)); }
    void postAfter(int64_t delayUs, std::function<void()> task) override {
        tasks_.push_back({nowUs_ + std::max<int64_t>(0, delayUs), seq_++, std::move(task)});
    }
    bool isCurrent() const override { return std::this_thread::get_id() == owner_; }

    // Advances the clock and runs every task due by then, earliest first,
    // posting order breaking ties. Tasks posted while pumping wait for the
    // next pump if they are not yet due.
    void advance(int64_t us) {
        nowUs_ += us;
        for (;;) {
            auto due = std::min_element(tasks_.begin(), tasks_.end(), [](const Task &a, const Task &b) {
                return a.dueUs != b.dueUs ? a.dueUs < b.dueUs : a.seq < b.seq;
            });
            if (due == tasks_.end() || due->dueUs > nowUs_) return;
            std::function<void()> task = std::move(due->task);
            tasks_.erase(due);
            task();
        }
    }

private:
    struct Task {
        int64_t dueUs;
        uint64_t seq;
        std::function<void()> task;
    };
    std::vector<Task> tasks_;
    int64_t nowUs_ = 0;
    uint64_t seq_ = 0;
    std::thread::id owner_ = std::this_thread::get_id();
};

// One executor per process, as in the app; installed on first use here.
ManualExecutor &executor() {
    static ManualExecutor *installed = [] {
        auto owned = std::make_unique<ManualExecutor>();
        ManualExecutor *raw = owned.get();
        sp::installMainThreadExecutor(std::move(owned));
        return raw;
    }();
    return *installed;
}

} // namespace

TEST(MainThreadExecutor, InstallsOnceAndIgnoresLaterInstalls) {
    ManualExecutor &first = executor();
    EXPECT_TRUE(sp::mainThreadExecutorInstalled());
    EXPECT_EQ(&sp::mainThread(), &first);
    sp::installMainThreadExecutor(std::make_unique<ManualExecutor>());
    EXPECT_EQ(&sp::mainThread(), &first);
}

TEST(MainThreadExecutor, PostRunsLaterInPostingOrder) {
    executor();
    std::vector<int> order;
    sp::mainThread().post([&] { order.push_back(1); });
    sp::mainThread().post([&] {
        order.push_back(2);
        // Posting from the UI thread never runs inline.
        sp::mainThread().post([&] { order.push_back(4); });
        order.push_back(3);
    });
    EXPECT_TRUE(order.empty());
    executor().advance(0);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 4}));
}

TEST(MainThreadExecutor, PostAfterWaitsForItsDelay) {
    executor();
    std::vector<int> order;
    sp::mainThread().postAfter(50'000, [&] { order.push_back(50); });
    sp::mainThread().postAfter(10'000, [&] { order.push_back(10); });
    sp::mainThread().post([&] { order.push_back(0); });
    executor().advance(9'999);
    EXPECT_EQ(order, (std::vector<int>{0}));
    executor().advance(1);
    EXPECT_EQ(order, (std::vector<int>{0, 10}));
    executor().advance(40'000);
    EXPECT_EQ(order, (std::vector<int>{0, 10, 50}));
}

TEST(MainThreadExecutor, IsCurrentOnlyOnTheUiThread) {
    executor();
    EXPECT_TRUE(sp::mainThread().isCurrent());
    bool other = true;
    std::thread([&] { other = sp::mainThread().isCurrent(); }).join();
    EXPECT_FALSE(other);
}
