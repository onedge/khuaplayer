#if defined(_WIN32)

#include "SPThread.hpp"
#include "SPWin32.hpp"

#include <chrono>
#include <thread>

namespace spfs {

void setCurrentThreadName(const char *name) {
    const std::wstring wide = win32::widen(name ? name : "");
    SetThreadDescription(GetCurrentThread(), wide.c_str());
}

void setCurrentThreadQos(ThreadQos qos) {
    int priority = THREAD_PRIORITY_NORMAL;
    switch (qos) {
    case ThreadQos::Background: priority = THREAD_PRIORITY_LOWEST; break;
    case ThreadQos::Utility: priority = THREAD_PRIORITY_BELOW_NORMAL; break;
    case ThreadQos::UserInteractive: priority = THREAD_PRIORITY_ABOVE_NORMAL; break;
    case ThreadQos::Default:
    case ThreadQos::UserInitiated:
    case ThreadQos::Unspecified: break;
    }
    SetThreadPriority(GetCurrentThread(), priority);
}

ThreadQos currentThreadQos() {
    const int priority = GetThreadPriority(GetCurrentThread());
    if (priority == THREAD_PRIORITY_ERROR_RETURN) return ThreadQos::Unspecified;
    if (priority <= THREAD_PRIORITY_LOWEST) return ThreadQos::Background;
    if (priority < THREAD_PRIORITY_NORMAL) return ThreadQos::Utility;
    if (priority > THREAD_PRIORITY_NORMAL) return ThreadQos::UserInteractive;
    return ThreadQos::UserInitiated;
}

bool throttleCurrentThreadDiskIo(int *) {
    // THREAD_MODE_BACKGROUND_BEGIN would also lower memory priority, so pages
    // a cache-warming thread reads would be evicted first. Callers set the
    // I/O priority hint on their own handle with spfs::lowerReadPriority.
    return true;
}

void sleepUninterruptible(int64_t microseconds) {
    // Windows sleeps are not cut short by CancelSynchronousIo.
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
}

void sleepMicroseconds(int64_t microseconds) {
    std::this_thread::sleep_for(std::chrono::microseconds(microseconds));
}

int64_t wallClockNowNs() {
    FILETIME now {};
    GetSystemTimePreciseAsFileTime(&now);
    const int64_t ticks = (int64_t)((uint64_t)now.dwHighDateTime << 32 | now.dwLowDateTime);
    return (ticks - 116444736000000000ll) * 100;
}

ThreadId currentThreadId() {
    return (ThreadId)GetCurrentThreadId();
}

void installBlockingIoInterrupt() {}

void interruptBlockingIo(ThreadId thread) {
    if (!thread) return;
    // spfs handles are overlapped: cancel the read registered by readAt.
    win32::cancelPendingRead((DWORD)thread);
    // Synchronous handles (pipes, handles from elsewhere) block inside the
    // call instead. THREAD_TERMINATE is the right CancelSynchronousIo needs.
    const HANDLE target = OpenThread(THREAD_TERMINATE, FALSE, (DWORD)thread);
    if (!target) return;
    CancelSynchronousIo(target);
    CloseHandle(target);
}

} // namespace spfs

#endif // _WIN32
