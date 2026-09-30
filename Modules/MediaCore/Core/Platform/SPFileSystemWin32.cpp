#if defined(_WIN32)

#include "SPFileSystem.hpp"
#include "SPWin32.hpp"

#include <winioctl.h>

#include <fcntl.h>
#include <io.h>
#include <mutex>
#include <string>
#include <unordered_map>

namespace spfs {
namespace {

using win32::lastErrno;
using win32::widen;

// FILETIME counts 100 ns intervals since 1601-01-01.
constexpr int64_t kUnixEpochAsFileTime = 116444736000000000ll;
constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

void setError(int *error, int value) {
    if (error) *error = value;
}

HANDLE toNative(Handle handle) { return (HANDLE)handle; }

// Full sharing never blocks a downloader that is still writing, renaming
// (for example .crdownload to the final name) or deleting the file.
HANDLE openShared(const std::wstring &path, DWORD access, DWORD flags) {
    return CreateFileW(path.c_str(), access, kShareAll, nullptr, OPEN_EXISTING, flags, nullptr);
}

class ScopedHandle {
public:
    explicit ScopedHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    ScopedHandle(const ScopedHandle &) = delete;
    ScopedHandle &operator=(const ScopedHandle &) = delete;
    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_;
};

// One manual-reset event per thread for waiting on overlapped I/O.
HANDLE threadEvent() {
    struct Event {
        HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~Event() {
            if (handle) CloseHandle(handle);
        }
    };
    thread_local Event event;
    return event.handle;
}

// Reads that interruptBlockingIo may cancel, by thread.
struct PendingRead {
    HANDLE file = INVALID_HANDLE_VALUE;
    OVERLAPPED *overlapped = nullptr;
    bool cancelled = false;
};
std::mutex gPendingMutex;
std::unordered_map<DWORD, PendingRead> gPending;

class PendingReadScope {
public:
    PendingReadScope(HANDLE file, OVERLAPPED *overlapped) : thread_(GetCurrentThreadId()) {
        std::lock_guard<std::mutex> lock(gPendingMutex);
        gPending[thread_] = {file, overlapped, false};
    }
    ~PendingReadScope() {
        std::lock_guard<std::mutex> lock(gPendingMutex);
        gPending.erase(thread_);
    }
    // After the I/O is in flight, honour a cancel that arrived before it was.
    void cancelIfRequested() {
        std::lock_guard<std::mutex> lock(gPendingMutex);
        const auto it = gPending.find(thread_);
        if (it != gPending.end() && it->second.cancelled) CancelIoEx(it->second.file, it->second.overlapped);
    }

private:
    DWORD thread_;
};

// Waits for an overlapped operation that returned `issued`. Synchronous
// handles, such as anonymous pipes, complete inside the call and skip the wait.
bool finishOverlapped(HANDLE file, OVERLAPPED *overlapped, BOOL issued, DWORD *bytes) {
    if (issued) return GetOverlappedResult(file, overlapped, bytes, FALSE) != FALSE;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    return GetOverlappedResult(file, overlapped, bytes, TRUE) != FALSE;
}

// DeviceIoControl on a handle that may be overlapped.
bool control(HANDLE file, DWORD code, void *in, DWORD inSize, void *out, DWORD outSize,
             DWORD *bytes) {
    OVERLAPPED overlapped {};
    overlapped.hEvent = threadEvent();
    const BOOL issued = DeviceIoControl(file, code, in, inSize, out, outSize, bytes, &overlapped);
    return finishOverlapped(file, &overlapped, issued, bytes);
}

// FileIdInfo carries the 128-bit IDs that ReFS needs, but some redirectors and
// FAT-family volumes reject it; the 64-bit file index is unique there.
bool readIdentity(HANDLE file, FileIdentity *out) {
    FILE_ID_INFO id {};
    if (GetFileInformationByHandleEx(file, FileIdInfo, &id, sizeof id)) {
        static_assert(sizeof id.FileId.Identifier == 16);
        uint64_t low = 0, high = 0;
        for (int i = 0; i < 8; ++i) low |= (uint64_t)id.FileId.Identifier[i] << (8 * i);
        for (int i = 0; i < 8; ++i) high |= (uint64_t)id.FileId.Identifier[8 + i] << (8 * i);
        *out = {id.VolumeSerialNumber, low, high};
        return true;
    }
    BY_HANDLE_FILE_INFORMATION info {};
    if (!GetFileInformationByHandle(file, &info)) return false;
    *out = {info.dwVolumeSerialNumber,
            (uint64_t)info.nFileIndexHigh << 32 | info.nFileIndexLow, 0};
    return true;
}

bool statHandle(HANDLE file, FileStat *out, int *error) {
    FILE_BASIC_INFO basic {};
    FILE_STANDARD_INFO standard {};
    FileStat result;
    if (!GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof basic) ||
        !GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof standard) ||
        !readIdentity(file, &result.identity)) {
        setError(error, lastErrno());
        return false;
    }
    result.regular = !standard.Directory && GetFileType(file) == FILE_TYPE_DISK;
    result.size = standard.EndOfFile.QuadPart;
    result.mtimeNs = (basic.LastWriteTime.QuadPart - kUnixEpochAsFileTime) * 100;
    *out = result;
    setError(error, 0);
    return true;
}

// Attribute-only access with backup semantics, so directories can be opened
// and reported as non-regular.
HANDLE openAttributes(const std::string &path, int *error) {
    const std::wstring wide = widen(path);
    if (wide.empty()) {
        setError(error, EINVAL);
        return INVALID_HANDLE_VALUE;
    }
    const HANDLE file = openShared(wide, FILE_READ_ATTRIBUTES, FILE_FLAG_BACKUP_SEMANTICS);
    if (file == INVALID_HANDLE_VALUE) setError(error, lastErrno());
    return file;
}

} // namespace

namespace win32 {

void cancelPendingRead(DWORD threadId) {
    std::lock_guard<std::mutex> lock(gPendingMutex);
    const auto it = gPending.find(threadId);
    if (it == gPending.end()) return;
    it->second.cancelled = true;
    // ERROR_NOT_FOUND when the read has not been issued yet or already
    // finished; PendingReadScope::cancelIfRequested covers the former.
    CancelIoEx(it->second.file, it->second.overlapped);
}

} // namespace win32

Handle openForRead(const std::string &path, int *error) {
    const std::wstring wide = widen(path);
    if (wide.empty()) {
        setError(error, EINVAL);
        return kInvalidHandle;
    }
    // Overlapped, so concurrent positional reads do not queue behind one
    // another on the file object. CreateFileW handles are not inheritable
    // without SECURITY_ATTRIBUTES, matching O_CLOEXEC.
    const HANDLE file = openShared(wide, GENERIC_READ, FILE_FLAG_OVERLAPPED);
    if (file == INVALID_HANDLE_VALUE) {
        setError(error, lastErrno());
        return kInvalidHandle;
    }
    setError(error, 0);
    return (Handle)file;
}

Handle duplicate(Handle handle, int *error) {
    // -1 is also GetCurrentProcess()'s pseudo-handle; never reopen it.
    if (!valid(handle)) {
        setError(error, EBADF);
        return kInvalidHandle;
    }
    // A new file object rather than DuplicateHandle's alias, so per-handle
    // state such as the I/O priority hint is not shared.
    const HANDLE copy = ReOpenFile(toNative(handle), GENERIC_READ, kShareAll, FILE_FLAG_OVERLAPPED);
    if (copy == INVALID_HANDLE_VALUE) {
        setError(error, lastErrno());
        return kInvalidHandle;
    }
    setError(error, 0);
    return (Handle)copy;
}

void close(Handle handle) {
    if (valid(handle)) CloseHandle(toNative(handle));
}

int64_t readAt(Handle handle, void *buffer, size_t length, int64_t offset, int *error) {
    const HANDLE file = toNative(handle);
    const DWORD want = length > 0x7fffffffu ? 0x7fffffffu : (DWORD)length;
    OVERLAPPED overlapped {};
    overlapped.Offset = (DWORD)((uint64_t)offset & 0xffffffffu);
    overlapped.OffsetHigh = (DWORD)((uint64_t)offset >> 32);
    overlapped.hEvent = threadEvent();

    PendingReadScope pending(file, &overlapped);
    DWORD got = 0;
    const BOOL issued = ReadFile(file, buffer, want, &got, &overlapped);
    if (!issued && GetLastError() == ERROR_IO_PENDING) pending.cancelIfRequested();
    if (!finishOverlapped(file, &overlapped, issued, &got)) {
        const DWORD code = GetLastError();
        if (code == ERROR_HANDLE_EOF) {
            setError(error, 0);
            return 0;
        }
        setError(error, win32::errnoFromWin32(code));
        return -1;
    }
    // A read that completed although a cancel was requested still returns
    // its data; POSIX pread behaves the same when the signal arrives late.
    setError(error, 0);
    return (int64_t)got;
}

bool stat(Handle handle, FileStat *out, int *error) {
    return statHandle(toNative(handle), out, error);
}

bool statPath(const std::string &path, FileStat *out, int *error) {
    ScopedHandle file(openAttributes(path, error));
    return file.valid() && statHandle(file.get(), out, error);
}

bool pathExists(const std::string &path) {
    const std::wstring wide = widen(path);
    return !wide.empty() && GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::string currentPath(Handle handle) {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetFinalPathNameByHandleW(toNative(handle), buffer.data(),
                                                       (DWORD)buffer.size(), VOLUME_NAME_DOS);
        if (length == 0) return {};
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        buffer.resize(length + 1);
    }
    // \\?\C:\dir\file -> C:\dir\file, \\?\UNC\server\share -> \\server\share.
    // Keep the prefix on long paths: without it, later calls such as
    // pathExists(path + ".aria2") would hit MAX_PATH.
    std::wstring_view view(buffer);
    if (view.size() - 4 < MAX_PATH) {
        if (view.starts_with(L"\\\\?\\UNC\\")) {
            const std::wstring unc = L"\\\\" + std::wstring(view.substr(8));
            return win32::narrow(unc.data(), unc.size());
        }
        if (view.starts_with(L"\\\\?\\")) view.remove_prefix(4);
    }
    return win32::narrow(view.data(), view.size());
}

VolumeInfo volume(Handle handle) {
    // FileRemoteProtocolInfo succeeds only for files on a network redirector
    // (SMB, WebDAV, NFS and third-party providers).
    FILE_REMOTE_PROTOCOL_INFO remote {};
    const bool isRemote = GetFileInformationByHandleEx(toNative(handle), FileRemoteProtocolInfo,
                                                       &remote, sizeof remote) != FALSE;
    return {!isRemote, isRemote};
}

VolumeInfo volumeOfPath(const std::string &path) {
    ScopedHandle file(openAttributes(path, nullptr));
    return file.valid() ? volume((Handle)file.get()) : VolumeInfo{};
}

int nextAllocatedRange(Handle handle, int64_t offset, int64_t *begin, int64_t *end) {
    LARGE_INTEGER size {};
    if (!GetFileSizeEx(toNative(handle), &size)) return -1;
    if (offset >= size.QuadPart) return 0;
    FILE_ALLOCATED_RANGE_BUFFER query {};
    query.FileOffset.QuadPart = offset;
    query.Length.QuadPart = size.QuadPart - offset;
    FILE_ALLOCATED_RANGE_BUFFER first {};
    DWORD bytes = 0;
    // One output entry is enough; ERROR_MORE_DATA only means more follow.
    if (!control(toNative(handle), FSCTL_QUERY_ALLOCATED_RANGES, &query, sizeof query, &first,
                 sizeof first, &bytes) &&
        GetLastError() != ERROR_MORE_DATA) {
        return -1;
    }
    if (bytes < sizeof first) return 0;
    *begin = first.FileOffset.QuadPart;
    *end = first.FileOffset.QuadPart + first.Length.QuadPart;
    return 1;
}

void adviseReadAhead(Handle) {
    // Windows' cache manager already detects sequential access.
}

void lowerReadPriority(Handle handle) {
    FILE_IO_PRIORITY_HINT_INFO hint {};
    hint.PriorityHint = IoPriorityHintLow;
    SetFileInformationByHandle(toNative(handle), FileIoPriorityHintInfo, &hint, sizeof hint);
}

int pathHasOtherWriter(const std::string &path, int *inspected, bool *incomplete) {
    if (inspected) *inspected = 0;
    if (incomplete) *incomplete = false;
    const std::wstring wide = widen(path);
    if (wide.empty()) return -1;
    // Ask for read access while refusing to share write access. The open
    // fails with a sharing violation exactly when some handle, in any
    // process, already has the file open for writing. Our own read handles
    // share everything and do not conflict.
    const HANDLE probe = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                                     nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (probe != INVALID_HANDLE_VALUE) {
        CloseHandle(probe);
        return 0;
    }
    return GetLastError() == ERROR_SHARING_VIOLATION ? 1 : -1;
}

std::FILE *openForReading(const std::string &path) {
    const std::wstring wide = widen(path);
    if (wide.empty()) return nullptr;
    // The C runtime reads synchronously, so this handle is not overlapped.
    const HANDLE handle = openShared(wide, GENERIC_READ, FILE_ATTRIBUTE_NORMAL);
    if (handle == INVALID_HANDLE_VALUE) return nullptr;
    // Hand the handle to the C runtime; fclose then closes it as well.
    const int fd = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(handle);
        return nullptr;
    }
    std::FILE *file = _fdopen(fd, "rb");
    if (!file) _close(fd);
    return file;
}

} // namespace spfs

#endif // _WIN32
