#include "SPExecutor.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace sp {
namespace {

std::once_flag gInstallOnce;
// Written once under gInstallOnce, then only read; never freed, like the main
// dispatch queue.
std::atomic<MainThreadExecutor *> gExecutor{nullptr};

} // namespace

void installMainThreadExecutor(std::unique_ptr<MainThreadExecutor> executor) {
    std::call_once(gInstallOnce, [&] { gExecutor.store(executor.release(), std::memory_order_release); });
}

bool mainThreadExecutorInstalled() {
    return gExecutor.load(std::memory_order_acquire) != nullptr;
}

MainThreadExecutor &mainThread() {
    MainThreadExecutor *executor = gExecutor.load(std::memory_order_acquire);
    if (!executor) {
        std::fputs("sp::mainThread() used before installMainThreadExecutor()\n", stderr);
        std::abort();
    }
    return *executor;
}

} // namespace sp
