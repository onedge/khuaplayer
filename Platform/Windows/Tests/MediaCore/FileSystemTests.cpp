#include "Platform/SPFileSystem.hpp"
#include "SPTrialExecutor.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

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
