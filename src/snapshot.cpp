#include "harbor/snapshot.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include "harbor/bytes.hpp"
#include "harbor/crc32c.hpp"

namespace harbor {

namespace {

constexpr std::uint8_t kMagic[4] = {'H', 'S', 'N', 'P'};
constexpr std::uint16_t kVersion = 1;

[[noreturn]] void corrupt(const std::filesystem::path& path, const std::string& why) {
    throw std::runtime_error("corrupt snapshot " + path.string() + ": " + why);
}

}  // namespace

void writeSnapshotFile(const std::filesystem::path& path, const Snapshot& snap, SyncMode mode) {
    std::vector<std::uint8_t> buf(kSnapshotHeaderSize + snap.orders.size() * kSnapshotEntrySize + 4, 0);
    std::uint8_t* h = buf.data();
    std::memcpy(h, kMagic, 4);
    putU16(h + 4, kVersion);
    putU64(h + 8, snap.segment);
    putU64(h + 16, snap.lastSeq);
    putU64(h + 24, static_cast<std::uint64_t>(snap.cfg.minPrice));
    putU64(h + 32, static_cast<std::uint64_t>(snap.cfg.maxPrice));
    putU64(h + 40, static_cast<std::uint64_t>(snap.cfg.tick));
    putU32(h + 48, snap.cfg.maxOrders);
    putU32(h + 52, static_cast<std::uint32_t>(snap.orders.size()));
    std::uint8_t* e = h + kSnapshotHeaderSize;
    for (const RestingOrder& o : snap.orders) {
        putU64(e, o.id);
        putU32(e + 8, o.client);
        e[12] = o.side == Side::Buy ? 0 : 1;
        putU64(e + 16, static_cast<std::uint64_t>(o.price));
        putU32(e + 24, o.remaining);
        e += kSnapshotEntrySize;
    }
    putU32(e, crc32c(buf.data(), buf.size() - 4));

    File file = File::create(path);
    file.writeAll(buf.data(), buf.size());
    file.sync(mode);
}

Snapshot readSnapshotFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open " + path.string());
    }
    std::vector<std::uint8_t> buf(std::istreambuf_iterator<char>(in), {});
    if (buf.size() < kSnapshotHeaderSize + 4) {
        corrupt(path, "too short");
    }
    if (getU32(buf.data() + buf.size() - 4) != crc32c(buf.data(), buf.size() - 4)) {
        corrupt(path, "checksum mismatch");
    }
    const std::uint8_t* h = buf.data();
    if (std::memcmp(h, kMagic, 4) != 0 || getU16(h + 4) != kVersion) {
        corrupt(path, "bad magic or version");
    }
    std::uint32_t count = getU32(h + 52);
    if (buf.size() != kSnapshotHeaderSize + std::size_t{count} * kSnapshotEntrySize + 4) {
        corrupt(path, "size does not match order count");
    }
    Snapshot snap{getU64(h + 8),
                  getU64(h + 16),
                  BookConfig{static_cast<Price>(getU64(h + 24)), static_cast<Price>(getU64(h + 32)),
                             static_cast<Price>(getU64(h + 40)), getU32(h + 48)},
                  {}};
    snap.orders.reserve(count);
    const std::uint8_t* e = h + kSnapshotHeaderSize;
    for (std::uint32_t i = 0; i < count; ++i, e += kSnapshotEntrySize) {
        if (e[12] > 1) {
            corrupt(path, "bad side in entry " + std::to_string(i));
        }
        snap.orders.push_back(RestingOrder{getU64(e), getU32(e + 8), e[12] == 0 ? Side::Buy : Side::Sell,
                                           static_cast<Price>(getU64(e + 16)), getU32(e + 24)});
    }
    return snap;
}

}  // namespace harbor
