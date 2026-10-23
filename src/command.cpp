#include "harbor/command.hpp"

#include <cstring>
#include "harbor/bytes.hpp"
#include "harbor/crc32c.hpp"

namespace harbor {

void encodeRecord(std::uint64_t seq, const Command& cmd, std::uint8_t* out) {
    std::memset(out, 0, kRecordSize);
    putU64(out, seq);
    out[8] = static_cast<std::uint8_t>(cmd.kind);
    switch (cmd.kind) {
        case Command::Kind::Add:
            out[9] = cmd.order.side == Side::Buy ? 0 : 1;
            out[10] = cmd.order.tif == TimeInForce::GTC ? 0 : 1;
            putU32(out + 12, cmd.order.client);
            putU64(out + 16, cmd.order.id);
            putU64(out + 24, static_cast<std::uint64_t>(cmd.order.price));
            putU32(out + 32, cmd.order.qty);
            break;
        case Command::Kind::Cancel:
            putU64(out + 16, cmd.id);
            break;
        case Command::Kind::Replace:
            putU64(out + 16, cmd.id);
            putU64(out + 24, static_cast<std::uint64_t>(cmd.price));
            putU32(out + 32, cmd.qty);
            break;
    }
    putU32(out + 44, crc32c(out, 44));
}

bool decodeRecord(const std::uint8_t* in, std::uint64_t& seq, Command& cmd) {
    if (getU32(in + 44) != crc32c(in, 44)) {
        return false;
    }
    std::uint8_t kind = in[8];
    std::uint8_t side = in[9];
    std::uint8_t tif = in[10];
    if (kind < 1 || kind > 3 || side > 1 || tif > 1) {
        return false;
    }
    seq = getU64(in);
    cmd = Command{};
    cmd.kind = static_cast<Command::Kind>(kind);
    std::uint64_t id = getU64(in + 16);
    auto price = static_cast<Price>(getU64(in + 24));  // modular conversion, defined in C++20
    Quantity qty = getU32(in + 32);
    if (cmd.kind == Command::Kind::Add) {
        cmd.order = NewOrder{id, getU32(in + 12), side == 0 ? Side::Buy : Side::Sell, price, qty,
                             tif == 0 ? TimeInForce::GTC : TimeInForce::IOC};
    } else {
        cmd.id = id;
        cmd.price = price;
        cmd.qty = qty;
    }
    return true;
}

}  // namespace harbor
