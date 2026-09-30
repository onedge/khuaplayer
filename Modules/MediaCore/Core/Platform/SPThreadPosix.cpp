#if !defined(_WIN32)

#include "SPThread.hpp"

#include "../../Bridge/SPRuntimeGates.hpp"

#include <cerrno>
#include <csignal>
#include <mutex>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <pthread/qos.h>
#include <sys/resource.h>
#endif

namespace spfs {

void setCurrentThreadName(const char *name) {
#if defined(__APPLE__)
    pthread_setname_np(name);
#else
    pthread_setname_np(pthread_self(), name);
#endif
}

#if defined(__APPLE__)
namespace {

qos_class_t toDarwin(ThreadQos qos) {
    switch (qos) {
    case ThreadQos::Background: return QOS_CLASS_BACKGROUND;
    case ThreadQos::Utility: return QOS_CLASS_UTILITY;
    case ThreadQos::Default: return QOS_CLASS_DEFAULT;
    case ThreadQos::UserInitiated: return QOS_CLASS_USER_INITIATED;
    case ThreadQos::UserInteractive: return QOS_CLASS_USER_INTERACTIVE;
    case ThreadQos::Unspecified: break;
    }
    return QOS_CLASS_UNSPECIFIED;
}

} // namespace
#endif

void setCurrentThreadQos(ThreadQos qos) {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(toDarwin(qos), 0);
#else
    (void)qos;
#endif
}

ThreadQos currentThreadQos() {
#if defined(__APPLE__)
    switch (qos_class_self()) {
    case QOS_CLASS_BACKGROUND: return ThreadQos::Background;
    case QOS_CLASS_UTILITY: return ThreadQos::Utility;
    case QOS_CLASS_DEFAULT: return ThreadQos::Default;
    case QOS_CLASS_USER_INITIATED: return ThreadQos::UserInitiated;
    case QOS_CLASS_USER_INTERACTIVE: return ThreadQos::UserInteractive;
    case QOS_CLASS_UNSPECIFIED: break;
    default: break;
    }
#endif
    return ThreadQos::Unspecified;
}

bool throttleCurrentThreadDiskIo(int *error) {
#if defined(__APPLE__)
    if (setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_THREAD, IOPOL_THROTTLE) == 0) return true;
    if (error) *error = errno;
    return false;
#else
    if (error) *error = ENOTSUP;
    return false;
#endif
}

void sleepUninterruptible(int64_t microseconds) {
    const int64_t deadline = spNowUs() + microseconds;
    for (int64_t now = spNowUs(); now < deadline; now = spNowUs()) {
        struct timespec ts { 0, 0 };
        const int64_t rem = deadline - now;
        ts.tv_sec = rem / 1000000;
        ts.tv_nsec = (rem % 1000000) * 1000;
        nanosleep(&ts, nullptr);
    }
}

void sleepMicroseconds(int64_t microseconds) {
    usleep((useconds_t)microseconds);
}

int64_t wallClockNowNs() {
    struct timespec wall {};
    clock_gettime(CLOCK_REALTIME, &wall);
    return (int64_t)wall.tv_sec * 1000000000LL + wall.tv_nsec;
}

ThreadId currentThreadId() {
    return (ThreadId)pthread_self();
}

namespace {
void ignoreSignal(int) {}
} // namespace

void installBlockingIoInterrupt() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction sa {};
        sa.sa_handler = ignoreSignal;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0; // No SA_RESTART: a blocked pread must return EINTR.
        sigaction(SIGUSR2, &sa, nullptr);
    });
}

void interruptBlockingIo(ThreadId thread) {
    if (thread) pthread_kill((pthread_t)thread, SIGUSR2);
}

} // namespace spfs

#endif // !_WIN32
