// Operating-system file access for the media core. SPFileSystemPosix.cpp
// implements it for macOS and SPFileSystemWin32.cpp for Windows; each file
// compiles to nothing on the other platform.
//
// Paths are UTF-8. On Windows they are converted to UTF-16 and opened through
// the wide-character APIs, so names outside the ANSI code page still work.
// Error codes are errno values on every platform (Windows maps its own codes,
// for example ERROR_OPERATION_ABORTED to EINTR), because callers hand them to
// FFmpeg as AVERROR(error) and treat EINTR as an interrupted read.
//
// Handle is a plain value like a POSIX file descriptor: copies refer to the
// same open file and exactly one owner calls close().
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace spfs {

// A POSIX file descriptor, or a Windows HANDLE. Both use -1 for "none"
// (INVALID_HANDLE_VALUE on Windows).
using Handle = intptr_t;
inline constexpr Handle kInvalidHandle = -1;
inline bool valid(Handle handle) noexcept { return handle != kInvalidHandle; }

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

struct VolumeInfo {
    bool local = false;  // MNT_LOCAL on POSIX; a fixed or removable drive on Windows.
    bool remote = false; // SMB, AFP, NFS or WebDAV on POSIX; a network drive or UNC path on Windows.
};

// Opens for reading. Other processes may keep writing, renaming and deleting
// the file, since a downloader may still be producing it. Not inherited by
// child processes. Concurrent readAt calls on one handle, or on handles from
// duplicate, never wait for one another (overlapped I/O on Windows).
Handle openForRead(const std::string &path, int *error = nullptr);
// An independent handle to the same open file. On Windows it is a separate
// file object, so per-handle settings such as lowerReadPriority stay separate.
Handle duplicate(Handle handle, int *error = nullptr);
void close(Handle handle);

// pread(2): reads at `offset` without moving a shared file position. Returns
// the byte count (0 at end of file), or -1 with *error set. A read interrupted
// by BlockingIoInterrupt reports EINTR; the caller decides whether to retry.
int64_t readAt(Handle handle, void *buffer, size_t length, int64_t offset, int *error);

bool stat(Handle handle, FileStat *out, int *error = nullptr);
// Follows symbolic links, like stat(2).
bool statPath(const std::string &path, FileStat *out, int *error = nullptr);
bool pathExists(const std::string &path);

// Current path of the open file, following renames; empty if unknown.
std::string currentPath(Handle handle);
VolumeInfo volume(Handle handle);
VolumeInfo volumeOfPath(const std::string &path);

// The first allocated byte range [*begin, *end) at or after `offset`, for
// sparse files that a downloader preallocates and fills in place. Returns 1
// when found, 0 when only unallocated space remains before end of file, and
// -1 when the query fails or the file system cannot answer it.
int nextAllocatedRange(Handle handle, int64_t offset, int64_t *begin, int64_t *end);

// Hints that reads will be mostly sequential.
void adviseReadAhead(Handle handle);
// Marks reads through this handle as low-priority background I/O (the
// Windows I/O priority hint). The handle must come from openForRead or
// duplicate, which never share it with foreground readers. No-op on POSIX,
// where throttleCurrentThreadDiskIo covers it.
void lowerReadPriority(Handle handle);

// 1 when another process holds the file open for writing, 0 when none does,
// and -1 when that cannot be determined. For diagnostics, *inspected receives
// the number of processes examined and *incomplete whether some could not be.
// Windows answers with a sharing probe instead and reports zero and false:
// for the few microseconds the probe handle exists, a process that tries to
// open the file for writing gets a sharing violation. A downloader normally
// keeps its handle open throughout, which is exactly what the probe detects.
int pathHasOtherWriter(const std::string &path, int *inspected = nullptr,
                       bool *incomplete = nullptr);

// fopen(path, "rb"), with the same sharing as openForRead. The caller closes
// the stream with std::fclose.
std::FILE *openForReading(const std::string &path);

} // namespace spfs
