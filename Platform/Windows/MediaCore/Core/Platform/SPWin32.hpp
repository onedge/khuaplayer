// Helpers shared by the Win32 implementations in this directory. Not part of
// the spfs interface; include only from *Win32.cpp files.
#pragma once

#if defined(_WIN32)

#include <windows.h>

#include <cerrno>
#include <string>

namespace spfs::win32 {

inline std::wstring widen(const std::string &utf8) {
    if (utf8.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                           (int)utf8.size(), nullptr, 0);
    if (length <= 0) return {};
    std::wstring wide((size_t)length, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), (int)utf8.size(),
                        wide.data(), length);
    return wide;
}

inline std::string narrow(const wchar_t *wide, size_t length) {
    if (length == 0) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, (int)length, nullptr, 0,
                                         nullptr, nullptr);
    if (size <= 0) return {};
    std::string utf8((size_t)size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, (int)length, utf8.data(), size, nullptr, nullptr);
    return utf8;
}

// spfs reports errno values on every platform.
inline int errnoFromWin32(DWORD code) {
    switch (code) {
    case ERROR_SUCCESS: return 0;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
        return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return EACCES;
    case ERROR_OPERATION_ABORTED:
        return EINTR;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM;
    case ERROR_INVALID_HANDLE:
        return EBADF;
    case ERROR_INVALID_PARAMETER:
    case ERROR_NO_UNICODE_TRANSLATION:
        return EINVAL;
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION:
        return ENOTSUP;
    case ERROR_FILENAME_EXCED_RANGE:
        return ENAMETOOLONG;
    default:
        return EIO;
    }
}

inline int lastErrno() { return errnoFromWin32(GetLastError()); }

// Cancels the read that `threadId` is performing inside spfs::readAt, so that
// it fails with EINTR. A request that lands after readAt registered the read
// but before the I/O was issued still cancels it; a thread outside readAt is
// unaffected, like a POSIX signal that arrives between reads.
// Implemented in SPFileSystemWin32.cpp; used by interruptBlockingIo.
void cancelPendingRead(DWORD threadId);

} // namespace spfs::win32

#endif // _WIN32
