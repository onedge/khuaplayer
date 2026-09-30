#if !defined(_WIN32)

#include "SPFileSystem.hpp"

#include <cerrno>
#include <sys/stat.h>

namespace spfs {

bool statPath(const std::string &path, FileStat *out, int *error) {
    struct stat sb {};
    if (::stat(path.c_str(), &sb) != 0) {
        if (error) *error = errno;
        return false;
    }
    FileStat result;
    result.regular = S_ISREG(sb.st_mode);
    result.size = (int64_t)sb.st_size;
#if defined(__APPLE__)
    result.mtimeNs = (int64_t)sb.st_mtimespec.tv_sec * 1000000000ll + sb.st_mtimespec.tv_nsec;
#else
    result.mtimeNs = (int64_t)sb.st_mtim.tv_sec * 1000000000ll + sb.st_mtim.tv_nsec;
#endif
    result.identity.volume = (uint64_t)sb.st_dev;
    result.identity.fileLow = (uint64_t)sb.st_ino;
    *out = result;
    if (error) *error = 0;
    return true;
}

std::FILE *openForReading(const std::string &path) {
    return std::fopen(path.c_str(), "rb");
}

} // namespace spfs

#endif // !_WIN32
