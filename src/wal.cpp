#include "harbor/wal.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include "harbor/bytes.hpp"
#include "harbor/crc32c.hpp"

namespace harbor {

namespace {

constexpr std::uint8_t kMagic[4] = {'H', 'W', 'A', 'L'};
constexpr std::uint16_t kVersion = 1;

[[noreturn]] void corrupt(const std::filesystem::path& path, const std::string& why) {
    throw std::runtime_error("corrupt log " + path.string() + ": " + why);
}

std::vector<std::uint8_t> readAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {});
}

}  // namespace

void writeWalHeader(File& file, std::uint64_t segment, std::uint64_t firstSeq) {
    std::array<std::uint8_t, kWalHeaderSize> h{};
    std::memcpy(h.data(), kMagic, 4);
    putU16(h.data() + 4, kVersion);
    putU64(h.data() + 8, segment);
    putU64(h.data() + 16, firstSeq);
    putU32(h.data() + 28, crc32c(h.data(), 28));
    file.writeAll(h.data(), h.size());
}

WalScan scanWal(const std::filesystem::path& path,
                const std::function<void(std::uint64_t, const Command&)>& onRecord) {
    std::vector<std::uint8_t> data = readAll(path);
    if (data.size() < kWalHeaderSize) {
        corrupt(path, "short header");
    }
    const std::uint8_t* h = data.data();
    if (std::memcmp(h, kMagic, 4) != 0 || getU16(h + 4) != kVersion ||
        getU32(h + 28) != crc32c(h, 28)) {
        corrupt(path, "bad header");
    }

    WalScan scan{getU64(h + 8), getU64(h + 16), getU64(h + 16), kWalHeaderSize, false};
    std::size_t slots = (data.size() - kWalHeaderSize) / kRecordSize;
    std::uint64_t seq = 0;
    std::uint64_t batchFirst = 0;
    Command cmd{};
    std::size_t i = 0;
    for (; i < slots; ++i) {
        const std::uint8_t* rec = data.data() + kWalHeaderSize + i * kRecordSize;
        if (!decodeRecord(rec, seq, batchFirst, cmd)) {
            break;
        }
        if (seq != scan.nextSeq) {
            corrupt(path, "expected seq " + std::to_string(scan.nextSeq) + ", found " + std::to_string(seq));
        }
        onRecord(seq, cmd);
        ++scan.nextSeq;
        scan.validBytes += kRecordSize;
    }
    // The engine syncs each batch before writing the next, so only the final
    // batch can be torn, and power loss mid-sync can leave holes anywhere in
    // it. A valid record after the first bad one is fine if it belongs to a
    // batch that started at or before the bad position (the same, final
    // batch). One from a later batch means an acknowledged batch was damaged.
    for (std::size_t j = i + 1; j < slots; ++j) {
        if (decodeRecord(data.data() + kWalHeaderSize + j * kRecordSize, seq, batchFirst, cmd) &&
            batchFirst > scan.nextSeq) {
            corrupt(path, "damaged record at seq " + std::to_string(scan.nextSeq) +
                              " inside an acknowledged batch");
        }
    }
    scan.tornTail = data.size() != scan.validBytes;
    return scan;
}

}  // namespace harbor
