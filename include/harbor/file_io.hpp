#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace harbor {

enum class SyncMode {
    Full,   // F_FULLFSYNC on macOS (flushes the drive cache), fdatasync on Linux
    Fsync,  // plain fsync
    None,   // no sync: survives a process crash, not power loss
};

// Owning POSIX file descriptor. Every failure throws std::runtime_error.
class File {
public:
    static File create(const std::filesystem::path& path);      // must not already exist
    static File openAppend(const std::filesystem::path& path);  // must exist

    File(File&& other) noexcept;
    File& operator=(File&& other) noexcept;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File();

    // Writes every byte, retrying short writes and EINTR.
    void writeAll(const void* data, std::size_t n);
    void sync(SyncMode mode);

private:
    File(int fd, std::filesystem::path path) : fd_(fd), path_(std::move(path)) {}
    int fd_ = -1;
    std::filesystem::path path_;
};

// Makes creates, renames, and deletes inside `dir` durable. No-op for None.
void syncDir(const std::filesystem::path& dir, SyncMode mode);

void truncateFile(const std::filesystem::path& path, std::uintmax_t size);

std::string walName(std::uint64_t segment);   // wal-000000000042.log
std::string snapName(std::uint64_t segment);  // snap-000000000042.bin

}  // namespace harbor
