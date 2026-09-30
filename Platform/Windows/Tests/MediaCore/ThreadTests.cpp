#include "Platform/SPFileSystem.hpp"
#include "Platform/SPThread.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

using namespace std::chrono;

TEST(Thread, WallClockIsUnixNanoseconds) {
    const int64_t mine = spfs::wallClockNowNs();
    const int64_t system =
        duration_cast<nanoseconds>(system_clock::now().time_since_epoch()).count();
    EXPECT_LT(std::llabs(mine - system), 1'000'000'000);
}

TEST(Thread, SleepUninterruptibleSleepsTheFullDuration) {
    const auto start = steady_clock::now();
    spfs::sleepUninterruptible(20'000);
    EXPECT_GE(steady_clock::now() - start, milliseconds(20));
}

TEST(Thread, QosRoundTrips) {
    std::thread([] {
        spfs::setCurrentThreadName("sp.test.qos");
        spfs::setCurrentThreadQos(spfs::ThreadQos::Utility);
        EXPECT_EQ(spfs::currentThreadQos(), spfs::ThreadQos::Utility);
        spfs::setCurrentThreadQos(spfs::ThreadQos::UserInteractive);
        EXPECT_EQ(spfs::currentThreadQos(), spfs::ThreadQos::UserInteractive);
        spfs::setCurrentThreadQos(spfs::ThreadQos::Background);
        EXPECT_EQ(spfs::currentThreadQos(), spfs::ThreadQos::Background);
        EXPECT_TRUE(spfs::throttleCurrentThreadDiskIo());
    }).join();
}

TEST(Thread, InterruptingAnIdleThreadIsHarmless) {
    spfs::installBlockingIoInterrupt();
    spfs::interruptBlockingIo(0);
    spfs::interruptBlockingIo(spfs::currentThreadId());
#if defined(_WIN32)
    // Thread IDs of finished threads are plain numbers on Windows; a joined
    // pthread_t on POSIX must never be signalled.
    spfs::ThreadId finished = 0;
    std::thread([&] { finished = spfs::currentThreadId(); }).join();
    spfs::interruptBlockingIo(finished);
#endif
}

#if defined(_WIN32)
#include <windows.h>

namespace {

// Interrupts the thread blocked in readAt on `handle` and checks it returns
// EINTR. `unblock` releases the reader if the interrupt never lands, so a
// failure cannot hang the test run.
void expectInterruptible(HANDLE handle, const std::function<void()> &unblock) {
    spfs::installBlockingIoInterrupt();
    std::atomic<spfs::ThreadId> reader{0};
    std::atomic<bool> done{false};
    int error = 0;
    int64_t got = 0;
    std::thread blocked([&] {
        reader = spfs::currentThreadId();
        char c;
        got = spfs::readAt((spfs::Handle)handle, &c, 1, 0, &error);
        done = true;
    });
    // Keep interrupting until the read has actually started and returned.
    const auto deadline = steady_clock::now() + seconds(5);
    while (!done && steady_clock::now() < deadline) {
        std::this_thread::sleep_for(milliseconds(20));
        spfs::interruptBlockingIo(reader);
    }
    const bool interrupted = done;
    unblock();
    blocked.join();
    EXPECT_TRUE(interrupted);
    EXPECT_EQ(got, -1);
    EXPECT_EQ(error, EINTR);
}

} // namespace

// The demuxer's abort path: a reader blocked on slow storage must return
// EINTR when another thread interrupts it. An empty pipe blocks forever.
// spfs handles are overlapped; this is the overlapped wait.
TEST(Thread, InterruptCancelsABlockedOverlappedRead) {
    const std::wstring name = L"\\\\.\\pipe\\khua-spfs-" + std::to_wstring(GetCurrentProcessId()) +
                              L"-" + std::to_wstring(GetTickCount64());
    const HANDLE server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_OUTBOUND | FILE_FLAG_OVERLAPPED,
                                           PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    ASSERT_NE(server, INVALID_HANDLE_VALUE);
    const HANDLE client = CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                      FILE_FLAG_OVERLAPPED, nullptr);
    ASSERT_NE(client, INVALID_HANDLE_VALUE);
    expectInterruptible(client, [&] { CloseHandle(server); });
    CloseHandle(client);
}

// Handles from outside spfs may be synchronous and block inside ReadFile.
TEST(Thread, InterruptCancelsABlockedSynchronousRead) {
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    ASSERT_TRUE(CreatePipe(&readEnd, &writeEnd, nullptr, 0));
    expectInterruptible(readEnd, [&] { CloseHandle(writeEnd); });
    CloseHandle(readEnd);
}
#endif
