#include "harbor/file_io.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>

namespace harbor {

namespace {

[[noreturn]] void fail(const char* what, const std::filesystem::path& path) {
    throw std::runtime_error(std::string(what) + " " + path.string() + ": " + std::strerror(errno));
}

int openOrThrow(const std::filesystem::path& path, int flags) {
    int fd = 0;
    do {
        fd = ::open(path.c_str(), flags | O_CLOEXEC, 0644);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        fail("open", path);
    }
    return fd;
}

void syncFd(int fd, SyncMode mode, const std::filesystem::path& path) {
    int rc = 0;
    switch (mode) {
        case SyncMode::None:
            return;
        case SyncMode::Full:
#if defined(__APPLE__)
            rc = ::fcntl(fd, F_FULLFSYNC);
            if (rc < 0 && (errno == ENOTSUP || errno == EINVAL)) {
                rc = ::fsync(fd);  // filesystems without F_FULLFSYNC support
            }
#else
            rc = ::fdatasync(fd);
#endif
            break;
        case SyncMode::Fsync:
            rc = ::fsync(fd);
            break;
    }
    if (rc < 0) {
        fail("sync", path);
    }
}

std::string numbered(const char* prefix, std::uint64_t segment, const char* suffix) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s%012llu%s", prefix, static_cast<unsigned long long>(segment), suffix);
    return buf;
}

}  // namespace

File File::create(const std::filesystem::path& path) {
    return File(openOrThrow(path, O_WRONLY | O_CREAT | O_EXCL), path);
}

File File::openAppend(const std::filesystem::path& path) {
    return File(openOrThrow(path, O_WRONLY | O_APPEND), path);
}

File::File(File&& other) noexcept : fd_(other.fd_), path_(std::move(other.path_)) {
    other.fd_ = -1;
}

File& File::operator=(File&& other) noexcept {
    if (this != &other) {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = other.fd_;
        path_ = std::move(other.path_);
        other.fd_ = -1;
    }
    return *this;
}

File::~File() {
    if (fd_ >= 0) {
        ::close(fd_);
    }
}

void File::writeAll(const void* data, std::size_t n) {
    const auto* p = static_cast<const std::uint8_t*>(data);
    while (n > 0) {
        ssize_t w = ::write(fd_, p, n);
        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }
            fail("write", path_);
        }
        p += w;
        n -= static_cast<std::size_t>(w);
    }
}

void File::sync(SyncMode mode) {
    syncFd(fd_, mode, path_);
}

void syncDir(const std::filesystem::path& dir, SyncMode mode) {
    if (mode == SyncMode::None) {
        return;
    }
    int fd = openOrThrow(dir, O_RDONLY);
    int rc = ::fsync(fd);
    int savedErrno = errno;
    ::close(fd);
    if (rc < 0) {
        errno = savedErrno;
        fail("fsync directory", dir);
    }
}

void truncateFile(const std::filesystem::path& path, std::uintmax_t size) {
    if (::truncate(path.c_str(), static_cast<off_t>(size)) < 0) {
        fail("truncate", path);
    }
}

std::string walName(std::uint64_t segment) {
    return numbered("wal-", segment, ".log");
}

std::string snapName(std::uint64_t segment) {
    return numbered("snap-", segment, ".bin");
}

}  // namespace harbor
