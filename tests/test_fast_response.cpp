#include "packet.h"
#include <algorithm>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

static void put16(std::vector<uint8_t> &p, size_t at, size_t v) { p[at] = v >> 8; p[at + 1] = v; }
static uint32_t sum(const uint8_t *p, size_t n, uint32_t s = 0) {
    for (; n >= 2; n -= 2, p += 2) s += (uint32_t(p[0]) << 8) | p[1];
    if (n) s += uint32_t(*p) << 8;
    while (s >> 16) s = (s & 65535) + (s >> 16);
    return s;
}
static std::vector<uint8_t> query() {
    return {0x12,0x34,1,0x20,0,1,0,0,0,0,0,0,
            3,'A','d','S',7,'e','x','a','m','p','l','e',3,'c','o','m',0,0,1,0,1};
}
static std::vector<uint8_t> udp(int version, const std::vector<uint8_t> &data, int port = 53) {
    size_t h = version == 4 ? 20 : 40;
    std::vector<uint8_t> p(h + 8 + data.size(), 0);
    if (version == 4) {
        p[0] = 0x45; put16(p, 2, p.size()); p[8] = 64; p[9] = 17;
        p[12] = 192; p[15] = 2; p[16] = 8; p[19] = 8;
    } else {
        p[0] = 0x60; put16(p, 4, p.size() - 40); p[6] = 17; p[7] = 64;
        p[8] = 0x20; p[9] = 1; p[23] = 2; p[24] = 0x20; p[25] = 1; p[39] = 8;
    }
    put16(p, h, 53001); put16(p, h + 2, port); put16(p, h + 4, data.size() + 8);
    std::copy(data.begin(), data.end(), p.begin() + h + 8);
    return p;
}
static void check_checksum(const std::vector<uint8_t> &p) {
    bool v4 = p[0] >> 4 == 4;
    size_t h = v4 ? 20 : 40;
    int protocol = p[v4 ? 9 : 6];
    if (v4) assert(sum(p.data(), h) == 65535);
    uint32_t pseudo = protocol == 1 ? 0 : sum(p.data() + (v4 ? 12 : 8), v4 ? 8 : 32) + protocol + p.size() - h;
    assert(sum(p.data() + h, p.size() - h, pseudo) == 65535);
}
int main() {
    for (int version : {4, 6}) {
        uint16_t hw = version == 4 ? ETH_PROTO_IPV4 : ETH_PROTO_IPV6;
        auto dns = query(), request = udp(version, dns);
        auto response = dns_negative_packet(request.data(), request.size(), hw);
        assert(!response.empty()); check_checksum(response);
        TransportView v;
        assert(decode_ip_transport(response.data(), response.size(), hw, v));
        assert(v.src_port == 53 && v.dst_port == 53001 && v.payload_len == dns.size());
        assert(v.payload[0] == 0x12 && v.payload[1] == 0x34 && v.payload[2] == 0x81 && v.payload[3] == 0x83);
        assert(std::equal(dns.begin() + 12, dns.end(), v.payload + 12));
        dns[11] = 1; // EDNS OPT must not be echoed as an answer or dangling data.
        dns.insert(dns.end(), {0,0,41,0x10,0,0,0,0,0,0,0});
        request = udp(version, dns);
        response = dns_negative_packet(request.data(), request.size(), hw);
        assert(response.size() == (version == 4 ? 20 : 40) + 8 + query().size());
        check_checksum(response);
        dns = query(); dns[5] = 2; dns.insert(dns.end(), {0xc0,12,0,28,0,1});
        request = udp(version, dns);
        response = dns_negative_packet(request.data(), request.size(), hw);
        assert(!response.empty()); check_checksum(response);
        dns = query(); dns[12] = 0xc0; dns[13] = 12;
        request = udp(version, dns);
        assert(dns_negative_packet(request.data(), request.size(), hw).empty());
        request = udp(version, query()); request.pop_back();
        assert(dns_negative_packet(request.data(), request.size(), hw).empty());
        dns = query(); dns[2] = 0x81; request = udp(version, dns);
        assert(dns_negative_packet(request.data(), request.size(), hw).empty());
        request = udp(version, std::vector<uint8_t>(1400, 0xab), 443);
        response = udp_unreachable_packet(request.data(), request.size(), hw);
        assert(!response.empty() && response.size() <= 1280); check_checksum(response);
        assert(response[version == 4 ? 20 : 40] == (version == 4 ? 3 : 1));
        assert(response[(version == 4 ? 20 : 40) + 1] == (version == 4 ? 3 : 4));
        TransportView tcp;
        tcp.ip_version = version; tcp.protocol = TransportProtocol::TCP;
        tcp.src_port = 53001; tcp.dst_port = 443; tcp.tcp_flags = 0x18;
        tcp.tcp_acknowledgment = 0x12345678; tcp.src_addr[0] = 1; tcp.dst_addr[0] = 2;
        response = tcp_reset_packet(tcp); check_checksum(response);
        assert(decode_ip_transport(response.data(), response.size(), hw, v));
        assert(v.tcp_flags == 4 && v.tcp_sequence == 0x12345678 && v.src_port == 443);
        tcp.tcp_flags = 2; tcp.tcp_sequence = 0xffffffff;
        response = tcp_reset_packet(tcp); check_checksum(response);
        assert(decode_ip_transport(response.data(), response.size(), hw, v) && v.tcp_acknowledgment == 0 && v.tcp_flags == 0x14);
        tcp.tcp_flags = 4; assert(tcp_reset_packet(tcp).empty());
    }
    for (size_t n = 0; n < 80; ++n) {
        std::vector<uint8_t> bytes(n, 0xff);
        assert(dns_negative_packet(bytes.data(), bytes.size(), ETH_PROTO_IPV4).empty());
        assert(udp_unreachable_packet(bytes.data(), bytes.size(), ETH_PROTO_IPV6).empty());
    }
    std::cout << "fast DNS/TCP/QUIC response IDs, flags, checksums, bounds and compression tests passed\n";
}
