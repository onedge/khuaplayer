#include "Platform/SPFileSystem.hpp"
#include "SPTrialExecutor.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace {

int currentProcessId() {
#if defined(_WIN32)
    return _getpid();
#else
    return (int)getpid();
#endif
}

// A scratch directory whose name and contents are outside the ANSI code page,
// as Korean and Japanese media libraries commonly are.
class FileSystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Unique per process and test, so parallel ctest runs never share it.
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        fs::path name(u8"khua-spfs-자막-字幕-");
        name += std::to_string(currentProcessId()) + "-" + std::to_string(stamp);
        dir_ = fs::temp_directory_path() / name;
        fs::create_directories(dir_);
    }
    void TearDown() override {
        std::error_code ignored;
        fs::remove_all(dir_, ignored);
    }

    fs::path write(const std::u8string &name, const std::string &contents) {
        const fs::path path = dir_ / fs::path(name);
        std::ofstream(path, std::ios::binary) << contents;
        return path;
    }

    static std::string utf8(const fs::path &path) {
        const std::u8string text = path.u8string();
        return std::string(text.begin(), text.end());
    }

    fs::path dir_;
};

} // namespace

TEST_F(FileSystemTest, StatReportsRegularFileSizeAndIdentity) {
    const fs::path path = write(u8"영화.mkv", "0123456789");

    spfs::FileStat st;
    int error = -1;
    ASSERT_TRUE(spfs::statPath(utf8(path), &st, &error));
    EXPECT_EQ(error, 0);
    EXPECT_TRUE(st.regular);
    EXPECT_EQ(st.size, 10);
    EXPECT_GT(st.mtimeNs, 0);

    spfs::FileStat again;
    ASSERT_TRUE(spfs::statPath(utf8(path), &again));
    EXPECT_EQ(st.identity, again.identity);
}

TEST_F(FileSystemTest, DistinctFilesHaveDistinctIdentities) {
    spfs::FileStat a, b;
    ASSERT_TRUE(spfs::statPath(utf8(write(u8"a.mp4", "a")), &a));
    ASSERT_TRUE(spfs::statPath(utf8(write(u8"b.mp4", "b")), &b));
    EXPECT_FALSE(a.identity == b.identity);
}

TEST_F(FileSystemTest, DirectoryIsNotRegular) {
    spfs::FileStat st;
    ASSERT_TRUE(spfs::statPath(utf8(dir_), &st));
    EXPECT_FALSE(st.regular);
}

TEST_F(FileSystemTest, MissingPathFailsWithError) {
    spfs::FileStat st;
    int error = 0;
    EXPECT_FALSE(spfs::statPath(utf8(dir_ / "missing.mkv"), &st, &error));
    EXPECT_NE(error, 0);
}

TEST_F(FileSystemTest, OpenForReadingHandlesNonAsciiNames) {
    const fs::path path = write(u8"ドラマ 第1話.mp4", "hello");
    std::FILE *file = spfs::openForReading(utf8(path));
    ASSERT_NE(file, nullptr);
    char buffer[8] = {};
    EXPECT_EQ(std::fread(buffer, 1, sizeof buffer, file), 5u);
    std::fclose(file);
    EXPECT_STREQ(buffer, "hello");
}

// A browser renames foo.mkv.crdownload to foo.mkv while we may be reading it.
// On Windows that rename fails unless our handle grants FILE_SHARE_DELETE,
// which plain fopen and _wfsopen never do.
TEST_F(FileSystemTest, OpenFileCanBeRenamedAndAppendedByADownloader) {
    const fs::path partial = write(u8"영화.mkv.crdownload", "abc");
    std::FILE *file = spfs::openForReading(utf8(partial));
    ASSERT_NE(file, nullptr);

    {
        std::ofstream writer(partial, std::ios::binary | std::ios::app);
        ASSERT_TRUE(writer.is_open());
        writer << "def";
    }
    const fs::path finished = dir_ / fs::path(u8"영화.mkv");
    std::error_code error;
    fs::rename(partial, finished, error);
    EXPECT_FALSE(error) << error.message();

    char buffer[8] = {};
    EXPECT_EQ(std::fread(buffer, 1, sizeof buffer, file), 6u);
    std::fclose(file);
    EXPECT_STREQ(buffer, "abcdef");
}

TEST_F(FileSystemTest, TrialSourceIdentityDetectsChanges) {
    const fs::path path = write(u8"트랙.flac", std::string(8192, 'x'));
    sptrial::SourceIdentity id;
    ASSERT_TRUE(sptrial::captureSource(utf8(path), id));
    EXPECT_TRUE(sptrial::sourceUnchanged(id));

    // Same size, different first 4 KiB.
    std::fstream(path, std::ios::binary | std::ios::in | std::ios::out) << 'y';
    EXPECT_FALSE(sptrial::sourceUnchanged(id));
}

TEST_F(FileSystemTest, HandleReadsAtOffsetsLikePread) {
    const fs::path path = write(u8"자막.srt", "0123456789");
    int error = -1;
    const spfs::Handle file = spfs::openForRead(utf8(path), &error);
    ASSERT_TRUE(spfs::valid(file));
    EXPECT_EQ(error, 0);

    char buffer[4] = {};
    EXPECT_EQ(spfs::readAt(file, buffer, 4, 6, &error), 4);
    EXPECT_EQ(std::string(buffer, 4), "6789");
    // Reads never move a position the next read depends on.
    EXPECT_EQ(spfs::readAt(file, buffer, 2, 0, &error), 2);
    EXPECT_EQ(std::string(buffer, 2), "01");
    // Short read at the end, then 0 at and past end of file.
    EXPECT_EQ(spfs::readAt(file, buffer, 4, 8, &error), 2);
    EXPECT_EQ(spfs::readAt(file, buffer, 4, 10, &error), 0);
    EXPECT_EQ(spfs::readAt(file, buffer, 4, 1000, &error), 0);
    EXPECT_EQ(error, 0);

    spfs::FileStat byHandle, byPath;
    ASSERT_TRUE(spfs::stat(file, &byHandle));
    ASSERT_TRUE(spfs::statPath(utf8(path), &byPath));
    EXPECT_EQ(byHandle.identity, byPath.identity);
    EXPECT_EQ(byHandle.size, 10);
    spfs::close(file);
}

TEST_F(FileSystemTest, MissingFileReportsErrno) {
    int error = 0;
    EXPECT_FALSE(spfs::valid(spfs::openForRead(utf8(dir_ / "missing.mkv"), &error)));
    EXPECT_EQ(error, ENOENT);
    spfs::FileStat st;
    EXPECT_FALSE(spfs::statPath(utf8(dir_ / "missing.mkv"), &st, &error));
    EXPECT_EQ(error, ENOENT);
    EXPECT_FALSE(spfs::pathExists(utf8(dir_ / "missing.mkv")));
}

TEST_F(FileSystemTest, DuplicateOutlivesTheOriginal) {
    const fs::path path = write(u8"a.mkv", "abc");
    const spfs::Handle original = spfs::openForRead(utf8(path));
    ASSERT_TRUE(spfs::valid(original));
    const spfs::Handle copy = spfs::duplicate(original);
    ASSERT_TRUE(spfs::valid(copy));
    spfs::close(original);
    char c = 0;
    int error = 0;
    EXPECT_EQ(spfs::readAt(copy, &c, 1, 2, &error), 1);
    EXPECT_EQ(c, 'c');
    spfs::close(copy);
}

TEST_F(FileSystemTest, CurrentPathFollowsRename) {
    const fs::path partial = write(u8"영화.mkv.part", "abc");
    const spfs::Handle file = spfs::openForRead(utf8(partial));
    ASSERT_TRUE(spfs::valid(file));
    EXPECT_EQ(fs::path(std::u8string(reinterpret_cast<const char8_t *>(spfs::currentPath(file).c_str()))),
              fs::canonical(partial));

    const fs::path finished = dir_ / fs::path(u8"영화.mkv");
    fs::rename(partial, finished);
    EXPECT_EQ(fs::path(std::u8string(reinterpret_cast<const char8_t *>(spfs::currentPath(file).c_str()))),
              fs::canonical(finished));
    spfs::close(file);
}

TEST_F(FileSystemTest, LocalVolumeIsNotRemote) {
    const fs::path path = write(u8"a.mkv", "abc");
    const spfs::Handle file = spfs::openForRead(utf8(path));
    ASSERT_TRUE(spfs::valid(file));
    const spfs::VolumeInfo info = spfs::volume(file);
    EXPECT_TRUE(info.local);
    EXPECT_FALSE(info.remote);
    const spfs::VolumeInfo byPath = spfs::volumeOfPath(utf8(path));
    EXPECT_EQ(byPath.local, info.local);
    EXPECT_EQ(byPath.remote, info.remote);
    spfs::close(file);
}

TEST_F(FileSystemTest, DenseFileIsOneAllocatedRange) {
    const fs::path path = write(u8"dense.mkv", std::string(100000, 'x'));
    const spfs::Handle file = spfs::openForRead(utf8(path));
    ASSERT_TRUE(spfs::valid(file));
    int64_t begin = -1, end = -1;
    const int found = spfs::nextAllocatedRange(file, 0, &begin, &end);
    if (found < 0) {
        spfs::close(file);
        GTEST_SKIP() << "temporary directory cannot report allocated ranges (FAT/exFAT?)";
    }
    ASSERT_EQ(found, 1);
    EXPECT_EQ(begin, 0);
    EXPECT_GE(end, 100000);
    EXPECT_EQ(spfs::nextAllocatedRange(file, 100000, &begin, &end), 0);
    spfs::close(file);
}

#if defined(_WIN32)
#include <windows.h>
#include <winioctl.h>

// A downloader that preallocates: 3 MiB sparse file with 4 KiB written at 1 MiB.
TEST_F(FileSystemTest, SparseFileReportsItsAllocatedRange) {
    const fs::path path = dir_ / fs::path(u8"sparse.mkv");
    const HANDLE writer = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(writer, INVALID_HANDLE_VALUE);
    DWORD bytes = 0;
    if (!DeviceIoControl(writer, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes, nullptr)) {
        CloseHandle(writer);
        GTEST_SKIP() << "temporary directory does not support sparse files";
    }
    FILE_END_OF_FILE_INFO eof {};
    eof.EndOfFile.QuadPart = 3 << 20;
    ASSERT_TRUE(SetFileInformationByHandle(writer, FileEndOfFileInfo, &eof, sizeof eof));
    const std::string block(4096, 'x');
    OVERLAPPED at {};
    at.Offset = 1 << 20;
    DWORD written = 0;
    ASSERT_TRUE(WriteFile(writer, block.data(), (DWORD)block.size(), &written, &at));
    CloseHandle(writer);

    const spfs::Handle file = spfs::openForRead(utf8(path));
    ASSERT_TRUE(spfs::valid(file));
    int64_t begin = -1, end = -1;
    ASSERT_EQ(spfs::nextAllocatedRange(file, 0, &begin, &end), 1);
    // NTFS allocates sparse files in 64 KiB units.
    EXPECT_GT(begin, 0);
    EXPECT_LE(begin, 1 << 20);
    EXPECT_GE(end, (1 << 20) + 4096);
    EXPECT_LT(end, 3 << 20);
    EXPECT_EQ(spfs::nextAllocatedRange(file, end, &begin, &end), 0);
    spfs::close(file);
}

TEST_F(FileSystemTest, OtherWriterIsDetectedBySharingProbe) {
    const fs::path path = write(u8"downloading.mkv", "abc");
    EXPECT_EQ(spfs::pathHasOtherWriter(utf8(path)), 0);
    // Our own read handles never count as writers.
    const spfs::Handle reader = spfs::openForRead(utf8(path));
    ASSERT_TRUE(spfs::valid(reader));
    EXPECT_EQ(spfs::pathHasOtherWriter(utf8(path)), 0);
    {
        std::ofstream writer(path, std::ios::binary | std::ios::app);
        ASSERT_TRUE(writer.is_open());
        EXPECT_EQ(spfs::pathHasOtherWriter(utf8(path)), 1);
    }
    EXPECT_EQ(spfs::pathHasOtherWriter(utf8(path)), 0);
    spfs::close(reader);
    EXPECT_EQ(spfs::pathHasOtherWriter(utf8(dir_ / "missing.mkv")), -1);
}
#endif

TEST_F(FileSystemTest, DuplicateRejectsAnInvalidHandle) {
    int error = 0;
    EXPECT_FALSE(spfs::valid(spfs::duplicate(spfs::kInvalidHandle, &error)));
    EXPECT_EQ(error, EBADF);
}

// The demuxer's reader, prefetch and scrub threads read one file at once.
TEST_F(FileSystemTest, ConcurrentReadsOnOneHandleReturnTheirOwnData) {
    std::string contents(1 << 20, '\0');
    for (size_t i = 0; i < contents.size(); ++i) contents[i] = (char)(i * 31 / 7);
    const fs::path path = write(u8"동시.mkv", contents);
    const spfs::Handle file = spfs::openForRead(utf8(path));
    ASSERT_TRUE(spfs::valid(file));
    const spfs::Handle copy = spfs::duplicate(file);
    ASSERT_TRUE(spfs::valid(copy));
    spfs::lowerReadPriority(copy);

    std::atomic<int> mismatches{0};
    auto reader = [&](spfs::Handle handle, int seed) {
        std::vector<char> buffer(4096);
        uint32_t state = (uint32_t)seed;
        for (int i = 0; i < 400; ++i) {
            state = state * 1664525u + 1013904223u;
            const int64_t offset = (int64_t)(state % (contents.size() - buffer.size()));
            int error = 0;
            const int64_t got = spfs::readAt(handle, buffer.data(), buffer.size(), offset, &error);
            if (got != (int64_t)buffer.size() ||
                std::memcmp(buffer.data(), contents.data() + offset, buffer.size()) != 0)
                ++mismatches;
        }
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.emplace_back(reader, t % 2 ? copy : file, t + 1);
    for (std::thread &t : threads) t.join();
    EXPECT_EQ(mismatches.load(), 0);
    spfs::close(copy);
    spfs::close(file);
}
