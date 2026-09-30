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

    std::shared_ptr<sp::MainRepeatingTimer> makeRepeatingTimer(int64_t intervalUs,
                                                               std::function<void()> handler) override {
        auto timer = std::make_shared<Timer>(this, intervalUs, std::move(handler));
        timer->schedule(nowUs_ + intervalUs);
        return timer;
    }

    // Advances the clock and runs every task due by then, earliest first,
    // posting order breaking ties. Tasks posted while pumping wait for the
    // next pump if they are not yet due.
    // The clock steps to each task's due time as it runs, so a task that
    // reposts itself (a repeating timer) is scheduled from when it ran.
    void advance(int64_t us) {
        const int64_t target = nowUs_ + us;
        for (;;) {
            auto due = std::min_element(tasks_.begin(), tasks_.end(), [](const Task &a, const Task &b) {
                return a.dueUs != b.dueUs ? a.dueUs < b.dueUs : a.seq < b.seq;
            });
            if (due == tasks_.end() || due->dueUs > target) break;
            nowUs_ = std::max(nowUs_, due->dueUs);
            std::function<void()> task = std::move(due->task);
            tasks_.erase(due);
            task();
        }
        nowUs_ = target;
    }

    int64_t nowUs() const { return nowUs_; }

private:
    // Repeats by posting itself; a generation counter drops firings that
    // suspend, resumeNow or cancel superseded.
    class Timer final : public sp::MainRepeatingTimer, public std::enable_shared_from_this<Timer> {
    public:
        Timer(ManualExecutor *owner, int64_t intervalUs, std::function<void()> handler)
            : owner_(owner), intervalUs_(intervalUs), handler_(std::move(handler)) {}
        void schedule(int64_t dueUs) {
            const uint64_t generation = ++generation_;
            std::weak_ptr<Timer> weak = weak_from_this();
            owner_->postAfter(dueUs - owner_->nowUs(), [weak, generation] {
                auto self = weak.lock();
                if (!self || !self->handler_ || generation != self->generation_) return;
                self->schedule(self->owner_->nowUs() + self->intervalUs_);
                self->handler_();
            });
        }
        void suspend() override { ++generation_; }
        void resumeNow() override { schedule(owner_->nowUs()); }
        void cancel() override {
            ++generation_;
            handler_ = nullptr;
        }

    private:
        ManualExecutor *owner_;
        int64_t intervalUs_;
        std::function<void()> handler_;
        uint64_t generation_ = 0;
    };

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

// The audio-only clock tick: every interval, paused and resumed with the
// session, cancelled at stop. This exercises the contract through the test
// double only; the macOS NSTimer implementation is checked by the Mac
// playback baseline (audio-only file), and Windows will add its own.
TEST(MainThreadExecutor, RepeatingTimerSuspendsResumesAndCancels) {
    executor();
    int ticks = 0;
    auto timer = sp::mainThread().makeRepeatingTimer(250'000, [&] { ++ticks; });
    executor().advance(249'999);
    EXPECT_EQ(ticks, 0);
    executor().advance(1);
    EXPECT_EQ(ticks, 1);
    executor().advance(500'000);
    EXPECT_EQ(ticks, 3);

    timer->suspend();
    executor().advance(1'000'000);
    EXPECT_EQ(ticks, 3);
    timer->resumeNow();
    executor().advance(0);
    EXPECT_EQ(ticks, 4); // fires at once, then every interval from then on
    executor().advance(250'000);
    EXPECT_EQ(ticks, 5);

    timer->cancel();
    executor().advance(1'000'000);
    EXPECT_EQ(ticks, 5);
}
