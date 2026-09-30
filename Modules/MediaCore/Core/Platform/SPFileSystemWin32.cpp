#if defined(_WIN32)

#include "SPFileSystem.hpp"

#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <string>

namespace spfs {
namespace {

// FILETIME counts 100 ns intervals since 1601-01-01.
constexpr int64_t kUnixEpochAsFileTime = 116444736000000000ll;

std::wstring widen(const std::string &utf8) {
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           (int)utf8.size(), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide((size_t)length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(),
                        wide.data(), length);
    return wide;
}

class Handle {
public:
    explicit Handle(HANDLE handle) noexcept : handle_(handle) {}
    ~Handle() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE get() const noexcept { return handle_; }
    bool valid() const noexcept { return handle_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE handle_;
};

bool fail(int *error) {
    if (error) *error = (int)GetLastError();
    return false;
}

constexpr DWORD kShareAll = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

// Full sharing never blocks a downloader that is still writing, renaming
// (for example .crdownload to the final name) or deleting the file.
HANDLE openShared(const std::wstring &path, DWORD access, DWORD flags) {
    return CreateFileW(path.c_str(), access, kShareAll, nullptr, OPEN_EXISTING, flags, nullptr);
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

} // namespace

bool statPath(const std::string &path, FileStat *out, int *error) {
    const std::wstring wide = widen(path);
    if (wide.empty()) {
        if (error) *error = ERROR_NO_UNICODE_TRANSLATION;
        return false;
    }
    // Backup semantics allow directories to be opened so they can be reported
    // as non-regular.
    Handle file(openShared(wide, FILE_READ_ATTRIBUTES, FILE_FLAG_BACKUP_SEMANTICS));
    if (!file.valid()) return fail(error);

    FILE_BASIC_INFO basic {};
    FILE_STANDARD_INFO standard {};
    FileStat result;
    if (!GetFileInformationByHandleEx(file.get(), FileBasicInfo, &basic, sizeof basic) ||
        !GetFileInformationByHandleEx(file.get(), FileStandardInfo, &standard, sizeof standard) ||
        !readIdentity(file.get(), &result.identity)) {
        return fail(error);
    }

    result.regular = !standard.Directory &&
                     GetFileType(file.get()) == FILE_TYPE_DISK;
    result.size = standard.EndOfFile.QuadPart;
    result.mtimeNs = (basic.LastWriteTime.QuadPart - kUnixEpochAsFileTime) * 100;
    *out = result;
    if (error) *error = 0;
    return true;
}

std::FILE *openForReading(const std::string &path) {
    const std::wstring wide = widen(path);
    if (wide.empty()) return nullptr;
    // _wfsopen cannot grant FILE_SHARE_DELETE, so open the handle directly and
    // hand it to the C runtime; fclose then closes the handle as well.
    const HANDLE handle = openShared(wide, GENERIC_READ, FILE_ATTRIBUTE_NORMAL);
    if (handle == INVALID_HANDLE_VALUE) return nullptr;
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
