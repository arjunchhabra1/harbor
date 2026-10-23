#pragma once

#include <atomic>
#include <filesystem>
#include <random>
#include <string>
#include <unistd.h>

namespace harbor::testing {

// A fresh, empty directory under the system temp dir, removed on destruction.
class TempDir {
public:
    TempDir() {
        static std::atomic<int> counter{0};
        std::random_device rd;
        path_ = std::filesystem::temp_directory_path() /
                ("harbor-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter++) + "-" +
                 std::to_string(rd()));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

}  // namespace harbor::testing
