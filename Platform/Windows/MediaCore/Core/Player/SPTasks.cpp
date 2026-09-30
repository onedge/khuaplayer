#include "SPTasks.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace sp {
namespace {

std::once_flag gInstallOnce;
// Written once under gInstallOnce, then only read; never freed.
std::atomic<BackgroundTasks *> gTasks{nullptr};

} // namespace

void installBackgroundTasks(std::unique_ptr<BackgroundTasks> tasks) {
    std::call_once(gInstallOnce, [&] { gTasks.store(tasks.release(), std::memory_order_release); });
}

bool backgroundTasksInstalled() {
    return gTasks.load(std::memory_order_acquire) != nullptr;
}

BackgroundTasks &backgroundTasks() {
    BackgroundTasks *tasks = gTasks.load(std::memory_order_acquire);
    if (!tasks) {
        std::fputs("sp::backgroundTasks() used before installBackgroundTasks()\n", stderr);
        std::abort();
    }
    return *tasks;
}

} // namespace sp
