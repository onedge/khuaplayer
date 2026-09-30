// Thread and clock facilities for the media core. SPThreadPosix.cpp implements
// them for macOS and SPThreadWin32.cpp for Windows.
#pragma once

#include <cstdint>

namespace spfs {

// Darwin QoS classes; Windows maps them to thread priorities. Default is the
// class of work submitted without one (QOS_CLASS_DEFAULT) and must round-trip
// unchanged; Unspecified means none was ever assigned.
enum class ThreadQos : uint8_t {
    Unspecified,
    Background,
    Utility,
    Default,
    UserInitiated,
    UserInteractive,
};

void setCurrentThreadName(const char *name);
void setCurrentThreadQos(ThreadQos qos);
ThreadQos currentThreadQos();
// Lowers the current thread's disk I/O priority below foreground reads
// (setiopolicy_np on Darwin). Windows has no thread-wide equivalent that
// leaves memory priority alone, so there it succeeds without effect; use
// spfs::lowerReadPriority on the thread's own handle as well.
// Returns false (with *error, an errno value) when the platform refuses.
bool throttleCurrentThreadDiskIo(int *error = nullptr);

// Sleeps for the full duration even if a BlockingIoInterrupt arrives.
void sleepUninterruptible(int64_t microseconds);
// Plain sleep; may return early on POSIX when a signal arrives.
void sleepMicroseconds(int64_t microseconds);

// CLOCK_REALTIME in nanoseconds since the Unix epoch.
int64_t wallClockNowNs();

// Identifies a thread for BlockingIoInterrupt. Zero means none.
using ThreadId = uintptr_t;
ThreadId currentThreadId();

// Wakes `thread` out of a blocking read so that it returns EINTR: SIGUSR2 on
// POSIX, CancelSynchronousIo on Windows. It is a no-op when the thread is not
// blocked, and harmless when the thread has exited. Call
// installBlockingIoInterrupt() once before the first use; it is idempotent.
void installBlockingIoInterrupt();
void interruptBlockingIo(ThreadId thread);

} // namespace spfs
