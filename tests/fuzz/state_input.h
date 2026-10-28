#pragma once
#include "tcp_reassembly.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>

// Each action is op/flow/flags/clock-step/LE-sequence/LE-length/payload.
// Clock low 3 bits select a step; upper bits extend the flow identity/family.
// One input describes a complete isolated session, never state across inputs.
template<class Callback>
uint64_t fuzz_actions(const uint8_t *data, size_t size, Callback callback) {
    size_t at = 0;
    uint64_t now = 100000, greatest = now;
    constexpr int steps[] = {0, 1, 9999, 10000, 10001, 30000, 30001, -1000};
    for (size_t action = 0; action < 2048 && size - at >= 10; ++action) {
        const auto *header = data + at;
        at += 10;
        const int step = steps[header[3] % 8];
        now = step < 0 ? (now >= 1000 ? now - 1000 : 0) : now + static_cast<unsigned>(step);
        greatest = std::max(now, greatest);
        TcpFlowKey key;
        key.ip_version = (header[3] & 128) ? 6 : 4;
        key.src_addr[0] = header[1]; key.src_addr[1] = (header[3] >> 3) & 15;
        key.dst_addr[0] = 1;
        key.src_port = static_cast<uint16_t>(1000 + header[1]);
        key.dst_port = 443;
        const uint32_t sequence = uint32_t(header[4]) | uint32_t(header[5]) << 8 |
                                  uint32_t(header[6]) << 16 | uint32_t(header[7]) << 24;
        const size_t bytes = std::min(size - at, size_t(header[8]) | size_t(header[9]) << 8);
        callback(header[0] % 6, key, sequence, header[2], data + at, bytes, now);
        at += bytes;
    }
    return greatest;
}
