#if !defined(_WIN32)

#include "SPFileSystem.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <libproc.h>
#include <sys/param.h>
#include <sys/proc_info.h>
#endif

namespace spfs {
namespace {

void setError(int *error, int value) {
    if (error) *error = value;
}

FileStat statFrom(const struct stat &sb) {
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
    return result;
}

VolumeInfo volumeFrom(const struct statfs &sfs) {
    VolumeInfo info;
    info.local = (sfs.f_flags & MNT_LOCAL) != 0;
#if defined(__APPLE__)
    const char *type = sfs.f_fstypename;
    info.remote = strcmp(type, "smbfs") == 0 || strcmp(type, "afpfs") == 0 ||
                  strcmp(type, "nfs") == 0 || strcmp(type, "webdav") == 0;
#endif
    return info;
}

} // namespace

Handle openForRead(const std::string &path, int *error) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    setError(error, fd < 0 ? errno : 0);
    return fd < 0 ? kInvalidHandle : (Handle)fd;
}

Handle duplicate(Handle handle, int *error) {
    const int fd = fcntl((int)handle, F_DUPFD_CLOEXEC, 0);
    setError(error, fd < 0 ? errno : 0);
    return fd < 0 ? kInvalidHandle : (Handle)fd;
}

void close(Handle handle) {
    if (valid(handle)) ::close((int)handle);
}

int64_t readAt(Handle handle, void *buffer, size_t length, int64_t offset, int *error) {
    const ssize_t n = ::pread((int)handle, buffer, length, (off_t)offset);
    setError(error, n < 0 ? errno : 0);
    return (int64_t)n;
}

bool stat(Handle handle, FileStat *out, int *error) {
    struct stat sb {};
    if (fstat((int)handle, &sb) != 0) {
        setError(error, errno);
        return false;
    }
    *out = statFrom(sb);
    setError(error, 0);
    return true;
}

bool statPath(const std::string &path, FileStat *out, int *error) {
    struct stat sb {};
    if (::stat(path.c_str(), &sb) != 0) {
        setError(error, errno);
        return false;
    }
    *out = statFrom(sb);
    setError(error, 0);
    return true;
}

bool pathExists(const std::string &path) {
    struct stat sb {};
    return ::stat(path.c_str(), &sb) == 0;
}

std::string currentPath(Handle handle) {
#if defined(__APPLE__)
    char buf[MAXPATHLEN] = {0};
    if (fcntl((int)handle, F_GETPATH, buf) != -1) return buf;
#else
    (void)handle;
#endif
    return {};
}

VolumeInfo volume(Handle handle) {
    struct statfs sfs {};
    if (fstatfs((int)handle, &sfs) != 0) return {};
    return volumeFrom(sfs);
}

VolumeInfo volumeOfPath(const std::string &path) {
    struct statfs sfs {};
    if (statfs(path.c_str(), &sfs) != 0) return {};
    return volumeFrom(sfs);
}

int nextAllocatedRange(Handle handle, int64_t offset, int64_t *begin, int64_t *end) {
#if defined(SEEK_DATA) && defined(SEEK_HOLE)
    const off_t data = lseek((int)handle, (off_t)offset, SEEK_DATA);
    if (data < 0) return errno == ENXIO ? 0 : -1;
    const off_t hole = lseek((int)handle, data, SEEK_HOLE);
    *begin = (int64_t)data;
    *end = (int64_t)hole; // A failed SEEK_HOLE leaves end <= begin.
    return 1;
#else
    (void)handle; (void)offset; (void)begin; (void)end;
    return -1;
#endif
}

void adviseReadAhead(Handle handle) {
#if defined(__APPLE__)
    (void)fcntl((int)handle, F_RDAHEAD, 1);
#else
    (void)handle;
#endif
}

void lowerReadPriority(Handle) {}

int pathHasOtherWriter(const std::string &path, int *inspected, bool *incomplete) {
    if (inspected) *inspected = 0;
    if (incomplete) *incomplete = false;
#if defined(__APPLE__)
    struct stat target {};
    if (::stat(path.c_str(), &target) != 0) return -1;
    int result = -1, checked = 0;
    bool unknown = false;
    int bytes = proc_listpids(PROC_UID_ONLY, getuid(), nullptr, 0);
    if (bytes > 0) {
        std::vector<pid_t> pids((size_t)bytes / sizeof(pid_t) + 64);
        bytes = proc_listpids(PROC_UID_ONLY, getuid(), pids.data(), (int)(pids.size() * sizeof(pid_t)));
        const pid_t self = getpid();
        const int n = bytes > 0 ? std::min<int>((int)pids.size(), bytes / (int)sizeof(pid_t)) : 0;
        std::vector<proc_fdinfo> fds;
        for (int i = 0; i < n && result != 1; ++i) {
            const pid_t pid = pids[(size_t)i];
            if (pid <= 0 || pid == self) continue;
            const int need = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
            if (need <= 0) { unknown = true; continue; }
            fds.resize((size_t)need / sizeof(proc_fdinfo) + 8);
            const int got = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(), (int)(fds.size() * sizeof(proc_fdinfo)));
            if (got <= 0) { unknown = true; continue; }
            ++checked;
            for (int k = 0; k < got / (int)sizeof(proc_fdinfo); ++k) {
                if (fds[(size_t)k].proc_fdtype != PROX_FDTYPE_VNODE) continue;
                vnode_fdinfo vi {};
                if (proc_pidfdinfo(pid, fds[(size_t)k].proc_fd, PROC_PIDFDVNODEINFO, &vi, sizeof(vi)) != (int)sizeof(vi)) continue;
                if ((dev_t)vi.pvi.vi_stat.vst_dev == target.st_dev && vi.pvi.vi_stat.vst_ino == target.st_ino &&
                    (vi.pfi.fi_openflags & FWRITE)) { result = 1; break; }
            }
        }
        if (result != 1) result = checked > 0 ? 0 : -1;
    }
    if (inspected) *inspected = checked;
    if (incomplete) *incomplete = unknown;
    return result;
#else
    (void)path;
    return -1;
#endif
}

std::FILE *openForReading(const std::string &path) {
    return std::fopen(path.c_str(), "rb");
}

} // namespace spfs

#endif // !_WIN32
