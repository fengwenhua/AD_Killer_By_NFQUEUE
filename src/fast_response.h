#pragma once
#include <cstdint>
#include <vector>

// Emits replies only to the local originator of an intercepted outbound packet.
// Headers/checksums are built by the bounded pure packet helpers.
class FastResponseSender {
public:
    FastResponseSender() = default;
    ~FastResponseSender();
    FastResponseSender(const FastResponseSender &) = delete;
    FastResponseSender &operator=(const FastResponseSender &) = delete;
    bool open();
    void close();
    bool send(const std::vector<uint8_t> &packet, uint32_t interface_index = 0);
private:
    int ipv4_ = -1;
    int ipv6_ = -1;
};
