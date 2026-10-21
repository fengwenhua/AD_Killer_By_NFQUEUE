#include "packet.h"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

void put_u16(std::vector<uint8_t> &data, size_t offset, size_t value) {
    data[offset] = (uint8_t)(value >> 8);
    data[offset + 1] = (uint8_t)value;
}

std::vector<uint8_t> dns_query() {
    return {
        0x12, 0x34, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
        0x03, 'A', 'd', 's',
        0x07, 'E', 'x', 'a', 'm', 'p', 'l', 'e',
        0x03, 'C', 'O', 'M', 0x00,
        0x00, 0x01, 0x00, 0x01,
    };
}

std::vector<uint8_t> udp_segment(const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> udp(8, 0);
    put_u16(udp, 0, 12345);
    put_u16(udp, 2, 53);
    put_u16(udp, 4, 8 + payload.size());
    udp.insert(udp.end(), payload.begin(), payload.end());
    return udp;
}

std::vector<uint8_t> tcp_segment(const std::vector<uint8_t> &payload) {
    std::vector<uint8_t> tcp(20, 0);
    put_u16(tcp, 0, 12345);
    put_u16(tcp, 2, 53);
    tcp[12] = 5u << 4;
    tcp.insert(tcp.end(), payload.begin(), payload.end());
    return tcp;
}

std::vector<uint8_t> ipv4_packet(uint8_t protocol,
                                 const std::vector<uint8_t> &segment) {
    std::vector<uint8_t> packet(20, 0);
    packet[0] = 0x45;
    put_u16(packet, 2, 20 + segment.size());
    packet[8] = 64;
    packet[9] = protocol;
    packet.insert(packet.end(), segment.begin(), segment.end());
    return packet;
}

std::vector<uint8_t> ipv6_packet(uint8_t protocol,
                                 const std::vector<uint8_t> &segment) {
    std::vector<uint8_t> packet(40, 0);
    packet[0] = 0x60;
    put_u16(packet, 4, segment.size());
    packet[6] = protocol;
    packet[7] = 64;
    packet.insert(packet.end(), segment.begin(), segment.end());
    return packet;
}

void append_bytes(std::vector<uint8_t> &target,
                  const std::vector<uint8_t> &source) {
    target.insert(target.end(), source.begin(), source.end());
}

std::vector<uint8_t> ipv6_option(uint8_t next) {
    std::vector<uint8_t> header(8, 0);
    header[0] = next;
    header[1] = 0;  // (0 + 1) * 8
    return header;
}

std::vector<uint8_t> ipv6_ah(uint8_t next) {
    std::vector<uint8_t> header(12, 0);
    header[0] = next;
    header[1] = 1;  // (1 + 2) * 4
    return header;
}

std::vector<uint8_t> ipv6_fragment(uint8_t next, uint16_t offset_flags) {
    std::vector<uint8_t> header(8, 0);
    header[0] = next;
    put_u16(header, 2, offset_flags);
    return header;
}

void check_dns_packet(const std::vector<uint8_t> &packet, uint16_t hw_proto,
                      TransportProtocol protocol, const char *message) {
    TransportView view;
    expect(decode_ip_transport(packet.data(), packet.size(), hw_proto, view), message);
    if (!view.payload) return;
    expect(view.status == TransportParseStatus::OK,
           "successful transport parsing should report OK");
    expect(view.protocol == protocol, "transport protocol should match");
    expect(view.src_port == 12345 && view.dst_port == 53,
           "transport ports should be decoded in host byte order");
    expect(dns_extract_qname(view.payload, view.payload_len,
                             protocol == TransportProtocol::TCP) == "ads.example.com",
           "DNS payload should decode after transport header is removed");
}

}  // namespace

int main() {
    reset_ip_extension_stats();
    const auto dns = dns_query();
    auto tcp_dns = dns;
    tcp_dns.insert(tcp_dns.begin(), {(uint8_t)(dns.size() >> 8), (uint8_t)dns.size()});

    check_dns_packet(ipv4_packet(17, udp_segment(dns)), ETH_PROTO_IPV4,
                     TransportProtocol::UDP, "IPv4 UDP should decode");
    check_dns_packet(ipv6_packet(17, udp_segment(dns)), ETH_PROTO_IPV6,
                     TransportProtocol::UDP, "IPv6 UDP should decode");
    check_dns_packet(ipv4_packet(6, tcp_segment(tcp_dns)), ETH_PROTO_IPV4,
                     TransportProtocol::TCP, "IPv4 TCP should decode");
    check_dns_packet(ipv6_packet(6, tcp_segment(tcp_dns)), ETH_PROTO_IPV6,
                     TransportProtocol::TCP, "IPv6 TCP should decode");

    auto multi_question = dns;
    multi_question[5] = 2;
    multi_question.insert(multi_question.end(), {
        0x07, 'T', 'r', 'a', 'c', 'k', 'e', 'r',
        0x04, 'T', 'e', 's', 't', 0,
        0, 1, 0, 1,
    });
    std::vector<std::string> names = dns_extract_qnames(
        multi_question.data(), multi_question.size(), false);
    expect(names.size() == 2 && names[0] == "ads.example.com" &&
               names[1] == "tracker.test",
           "all DNS questions should be parsed and normalized");

    auto compressed_question = dns;
    compressed_question[5] = 2;
    compressed_question.insert(compressed_question.end(), {
        0xc0, 0x0c, 0, 1, 0, 1,
    });
    names = dns_extract_qnames(compressed_question.data(),
                               compressed_question.size(), false);
    expect(names.size() == 2 && names[0] == "ads.example.com" &&
               names[1] == "ads.example.com",
           "compressed DNS question names should be followed safely");

    // Hop-by-Hop → Routing → Destination → AH → atomic Fragment → UDP。
    std::vector<uint8_t> extended;
    append_bytes(extended, ipv6_option(43));
    append_bytes(extended, ipv6_option(60));
    append_bytes(extended, ipv6_option(51));
    append_bytes(extended, ipv6_ah(44));
    append_bytes(extended, ipv6_fragment(17, 0));
    append_bytes(extended, udp_segment(dns));
    check_dns_packet(ipv6_packet(0, extended), ETH_PROTO_IPV6,
                     TransportProtocol::UDP,
                     "common IPv6 extension chain should reach UDP");

    TransportView view;
    std::vector<uint8_t> short_packet(10, 0);
    expect(!decode_ip_transport(short_packet.data(), short_packet.size(),
                                ETH_PROTO_IPV4, view),
           "truncated IPv4 should be rejected");

    auto fragmented = ipv4_packet(17, udp_segment(dns));
    fragmented[6] = 0x20;  // more fragments
    expect(!decode_ip_transport(fragmented.data(), fragmented.size(),
                                ETH_PROTO_IPV4, view),
           "fragmented IPv4 should be rejected until reassembly is supported");
    expect(view.status == TransportParseStatus::FRAGMENTED,
           "IPv4 fragments should expose a countable reason");

    std::vector<uint8_t> first_fragment;
    append_bytes(first_fragment, ipv6_fragment(17, 1));  // M=1
    append_bytes(first_fragment, udp_segment(dns));
    auto first_packet = ipv6_packet(44, first_fragment);
    expect(!decode_ip_transport(first_packet.data(), first_packet.size(),
                                ETH_PROTO_IPV6, view) &&
               view.status == TransportParseStatus::FRAGMENTED,
           "IPv6 first fragments should be passed for reassembly consistency");

    std::vector<uint8_t> later_fragment;
    append_bytes(later_fragment, ipv6_fragment(17, 8));  // offset=1
    append_bytes(later_fragment, udp_segment(dns));
    auto later_packet = ipv6_packet(44, later_fragment);
    expect(!decode_ip_transport(later_packet.data(), later_packet.size(),
                                ETH_PROTO_IPV6, view) &&
               view.status == TransportParseStatus::FRAGMENTED,
           "IPv6 non-first fragments should be safely rejected");

    std::vector<uint8_t> reserved_fragment;
    append_bytes(reserved_fragment, ipv6_fragment(17, 2));
    append_bytes(reserved_fragment, udp_segment(dns));
    auto reserved_packet = ipv6_packet(44, reserved_fragment);
    expect(!decode_ip_transport(reserved_packet.data(), reserved_packet.size(),
                                ETH_PROTO_IPV6, view) &&
               view.status == TransportParseStatus::MALFORMED,
           "IPv6 fragment reserved bits should be rejected as malformed");

    std::vector<uint8_t> misplaced_hop;
    append_bytes(misplaced_hop, ipv6_option(0));
    append_bytes(misplaced_hop, ipv6_option(17));
    append_bytes(misplaced_hop, udp_segment(dns));
    auto misplaced_packet = ipv6_packet(60, misplaced_hop);
    expect(!decode_ip_transport(misplaced_packet.data(), misplaced_packet.size(),
                                ETH_PROTO_IPV6, view) &&
               view.status == TransportParseStatus::MALFORMED,
           "Hop-by-Hop header outside the first position should be rejected");

    auto truncated_extension = ipv6_packet(60, std::vector<uint8_t>(4, 0));
    expect(!decode_ip_transport(truncated_extension.data(),
                                truncated_extension.size(), ETH_PROTO_IPV6, view) &&
               view.status == TransportParseStatus::TRUNCATED,
           "truncated IPv6 extension headers should be safely rejected");

    std::vector<uint8_t> short_ah(8, 0);
    short_ah[0] = 17;
    short_ah[1] = 0;  // 8 bytes，低于 AH 规定的 12 字节最小长度
    append_bytes(short_ah, udp_segment(dns));
    auto short_ah_packet = ipv6_packet(51, short_ah);
    expect(!decode_ip_transport(short_ah_packet.data(), short_ah_packet.size(),
                                ETH_PROTO_IPV6, view) &&
               view.status == TransportParseStatus::MALFORMED,
           "undersized AH headers should be classified as malformed");

    std::vector<uint8_t> too_many_extensions;
    for (int i = 0; i < 9; ++i)
        append_bytes(too_many_extensions, ipv6_option(i == 8 ? 17 : 60));
    append_bytes(too_many_extensions, udp_segment(dns));
    auto too_many_packet = ipv6_packet(60, too_many_extensions);
    expect(!decode_ip_transport(too_many_packet.data(), too_many_packet.size(),
                                ETH_PROTO_IPV6, view) &&
               view.status == TransportParseStatus::EXTENSION_LIMIT,
           "pathological IPv6 extension chains should hit a bounded limit");

    IpExtensionStats stats = ip_extension_stats();
    expect(stats.ipv4_fragments == 1 && stats.ipv6_fragments == 2 &&
               stats.ipv6_extension_errors == 5,
           "fragment and malformed extension skips should be counted");

    auto bad_tcp_dns = tcp_dns;
    bad_tcp_dns[1]++;
    expect(dns_extract_qname(bad_tcp_dns.data(), bad_tcp_dns.size(), true).empty(),
           "incomplete TCP DNS message should be rejected");

    if (failures != 0) {
        std::cerr << failures << " packet test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "packet tests passed\n";
    return EXIT_SUCCESS;
}
