// Operating-system file access for the media core. SPFileSystemPosix.cpp
// implements it for macOS and SPFileSystemWin32.cpp for Windows; each file
// compiles to nothing on the other platform.
//
// Paths are UTF-8. On Windows they are converted to UTF-16 and opened through
// the wide-character APIs, so names outside the ANSI code page still work.
// Error codes are errno values on POSIX and Win32 error codes on Windows;
// callers only compare them with zero and log them otherwise.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace spfs {

// Stable identity of a file: st_dev/st_ino on POSIX (fileHigh is zero), the
// volume serial number and 128-bit file ID on Windows.
struct FileIdentity {
    uint64_t volume = 0;
    uint64_t fileLow = 0;
    uint64_t fileHigh = 0;

    bool operator==(const FileIdentity &) const = default;
};

struct FileStat {
    bool regular = false;
    int64_t size = 0;
    int64_t mtimeNs = 0; // Nanoseconds since the Unix epoch.
    FileIdentity identity;
};

// Follows symbolic links, like stat(2).
bool statPath(const std::string &path, FileStat *out, int *error = nullptr);

// fopen(path, "rb"). The caller closes the stream with std::fclose.
std::FILE *openForReading(const std::string &path);

} // namespace spfs
