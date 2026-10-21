#include "packet.h"

#include <atomic>
#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

// ============================================================================
// DNS 报文解析
//   报文结构:[id:2][flags:2][qdcount:2][ancount:2][nscount:2][arcount:2] question...
//   question:qname(标签序列)+ qtype:2 + qclass:2
//   DNS-over-TCP 在报文前多 2 字节长度域
// =========================================================================

// 网络序读 16 位
static size_t read_u16(const uint8_t *p) { return (size_t)p[0] << 8 | p[1]; }
static uint32_t read_u32(const uint8_t *p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

static std::atomic<uint64_t> g_ipv4_fragments{0};
static std::atomic<uint64_t> g_ipv6_fragments{0};
static std::atomic<uint64_t> g_ipv6_extension_errors{0};

IpExtensionStats ip_extension_stats() {
    return {
        g_ipv4_fragments.load(std::memory_order_relaxed),
        g_ipv6_fragments.load(std::memory_order_relaxed),
        g_ipv6_extension_errors.load(std::memory_order_relaxed),
    };
}

void reset_ip_extension_stats() {
    g_ipv4_fragments.store(0, std::memory_order_relaxed);
    g_ipv6_fragments.store(0, std::memory_order_relaxed);
    g_ipv6_extension_errors.store(0, std::memory_order_relaxed);
}

bool decode_ip_transport(const uint8_t *packet, size_t packet_len,
                         uint16_t hw_proto, TransportView &out) {
    out = TransportView{};
    auto reject = [&](TransportParseStatus status) {
        out.status = status;
        return false;
    };
    if (!packet) return reject(TransportParseStatus::MALFORMED);

    const uint8_t *l4 = nullptr;
    size_t l4_len = 0;
    uint8_t protocol = 0;
    bool truncated = false;

    if (hw_proto == ETH_PROTO_IPV4) {
        if (packet_len < 20) return reject(TransportParseStatus::TRUNCATED);
        if ((packet[0] >> 4) != 4) return reject(TransportParseStatus::MALFORMED);
        size_t ihl = (size_t)(packet[0] & 0x0f) * 4;
        if (ihl < 20) return reject(TransportParseStatus::MALFORMED);
        if (ihl > packet_len) return reject(TransportParseStatus::TRUNCATED);
        size_t total_len = read_u16(packet + 2);
        if (total_len < ihl) return reject(TransportParseStatus::MALFORMED);

        // 任意真实分片均放行，避免只阻断首片造成连接永久超时。
        uint16_t fragment = (uint16_t)read_u16(packet + 6);
        if ((fragment & 0x3fff) != 0) {
            g_ipv4_fragments.fetch_add(1, std::memory_order_relaxed);
            return reject(TransportParseStatus::FRAGMENTED);
        }

        size_t available = packet_len;
        if (total_len < available) available = total_len;
        else if (total_len > available) truncated = true;
        protocol = packet[9];
        out.ip_version = 4;
        memcpy(out.src_addr.data(), packet + 12, 4);
        memcpy(out.dst_addr.data(), packet + 16, 4);
        l4 = packet + ihl;
        l4_len = available - ihl;
    } else if (hw_proto == ETH_PROTO_IPV6) {
        if (packet_len < 40) return reject(TransportParseStatus::TRUNCATED);
        if ((packet[0] >> 4) != 6) return reject(TransportParseStatus::MALFORMED);
        if (read_u16(packet + 4) == 0)
            return reject(TransportParseStatus::UNSUPPORTED);  // jumbogram
        size_t total_len = 40 + read_u16(packet + 4);
        size_t available = packet_len;
        if (total_len < available) available = total_len;
        else if (total_len > available) truncated = true;
        protocol = packet[6];
        out.ip_version = 6;
        memcpy(out.src_addr.data(), packet + 8, 16);
        memcpy(out.dst_addr.data(), packet + 24, 16);
        size_t offset = 40;
        size_t extension_bytes = 0;
        unsigned extension_count = 0;
        bool saw_fragment = false;
        while (protocol == 0 || protocol == 43 || protocol == 44 ||
               protocol == 51 || protocol == 60) {
            if (++extension_count > 8 || extension_bytes >= 256) {
                g_ipv6_extension_errors.fetch_add(1, std::memory_order_relaxed);
                return reject(TransportParseStatus::EXTENSION_LIMIT);
            }
            if (protocol == 0 && offset != 40) {
                g_ipv6_extension_errors.fetch_add(1, std::memory_order_relaxed);
                return reject(TransportParseStatus::MALFORMED);
            }
            size_t header_len = 0;
            if (protocol == 44) {
                if (saw_fragment) {
                    g_ipv6_extension_errors.fetch_add(1, std::memory_order_relaxed);
                    return reject(TransportParseStatus::MALFORMED);
                }
                saw_fragment = true;
                header_len = 8;
            } else {
                if (offset + 2 > available) {
                    g_ipv6_extension_errors.fetch_add(1, std::memory_order_relaxed);
                    return reject(TransportParseStatus::TRUNCATED);
                }
                header_len = protocol == 51
                    ? (static_cast<size_t>(packet[offset + 1]) + 2) * 4
                    : (static_cast<size_t>(packet[offset + 1]) + 1) * 8;
            }
            size_t minimum_len = protocol == 51 ? 12 : 8;
            if (header_len < minimum_len) {
                g_ipv6_extension_errors.fetch_add(1, std::memory_order_relaxed);
                return reject(TransportParseStatus::MALFORMED);
            }
            if (header_len > 256 - extension_bytes) {
                g_ipv6_extension_errors.fetch_add(1, std::memory_order_relaxed);
                return reject(TransportParseStatus::EXTENSION_LIMIT);
            }
            if (offset > available || header_len > available - offset) {
                g_ipv6_extension_errors.fetch_add(1, std::memory_order_relaxed);
                return reject(TransportParseStatus::TRUNCATED);
            }
            uint8_t next = packet[offset];
            if (protocol == 44) {
                uint16_t fragment = (uint16_t)read_u16(packet + offset + 2);
                if ((fragment & 0x0006) != 0) {
                    g_ipv6_extension_errors.fetch_add(1, std::memory_order_relaxed);
                    return reject(TransportParseStatus::MALFORMED);
                }
                // offset=0 且 M=0 是 atomic fragment，不需要重组。
                if ((fragment & 0xfff9) != 0) {
                    g_ipv6_fragments.fetch_add(1, std::memory_order_relaxed);
                    return reject(TransportParseStatus::FRAGMENTED);
                }
            }
            offset += header_len;
            extension_bytes += header_len;
            protocol = next;
        }
        if (protocol == 50 || protocol == 59)
            return reject(TransportParseStatus::UNSUPPORTED);
        l4 = packet + offset;
        l4_len = available - offset;
    } else {
        return reject(TransportParseStatus::UNSUPPORTED);
    }

    if (protocol == static_cast<uint8_t>(TransportProtocol::UDP)) {
        if (l4_len < 8) return reject(TransportParseStatus::TRUNCATED);
        size_t udp_len = read_u16(l4 + 4);
        if (udp_len < 8) return reject(TransportParseStatus::MALFORMED);
        size_t available = l4_len;
        if (udp_len < available) available = udp_len;
        else if (udp_len > available) truncated = true;
        out.protocol = TransportProtocol::UDP;
        out.payload = l4 + 8;
        out.payload_len = available - 8;
    } else if (protocol == static_cast<uint8_t>(TransportProtocol::TCP)) {
        if (l4_len < 20) return reject(TransportParseStatus::TRUNCATED);
        size_t tcp_header_len = (size_t)(l4[12] >> 4) * 4;
        if (tcp_header_len < 20) return reject(TransportParseStatus::MALFORMED);
        if (tcp_header_len > l4_len) return reject(TransportParseStatus::TRUNCATED);
        out.protocol = TransportProtocol::TCP;
        out.tcp_sequence = read_u32(l4 + 4);
        out.tcp_flags = l4[13];
        out.tcp_acknowledgment = read_u32(l4 + 8);
        out.payload = l4 + tcp_header_len;
        out.payload_len = l4_len - tcp_header_len;
    } else {
        return reject(TransportParseStatus::UNSUPPORTED);
    }

    out.src_port = (uint16_t)read_u16(l4);
    out.dst_port = (uint16_t)read_u16(l4 + 2);
    out.truncated = truncated;
    out.status = TransportParseStatus::OK;
    return true;
}

namespace {

bool parse_dns_name(const uint8_t *message, size_t message_len, size_t offset,
                    size_t &next_offset, std::string &name) {
    if (!message || offset >= message_len) return false;
    size_t cursor = offset;
    size_t expanded = 0;
    bool jumped = false;
    bool hostname_valid = true;
    name.clear();
    for (size_t steps = 0; steps < 128; ++steps) {
        if (cursor >= message_len) return false;
        const uint8_t label_len = message[cursor];
        if (label_len == 0) {
            if (!jumped) next_offset = cursor + 1;
            if (name.size() < 4 || name.find('.') == std::string::npos ||
                !hostname_valid)
                name.clear();
            return true;
        }
        if ((label_len & 0xc0) == 0xc0) {
            if (cursor + 1 >= message_len) return false;
            const size_t pointer =
                (static_cast<size_t>(label_len & 0x3f) << 8) |
                message[cursor + 1];
            if (pointer >= message_len) return false;
            if (!jumped) next_offset = cursor + 2;
            cursor = pointer;
            jumped = true;
            continue;
        }
        if ((label_len & 0xc0) != 0 || label_len > 63 ||
            cursor + 1 + label_len > message_len)
            return false;
        if (!name.empty()) {
            name.push_back('.');
            expanded++;
        }
        for (size_t i = 0; i < label_len; ++i) {
            unsigned char c = message[cursor + 1 + i];
            const bool valid = (c >= 'a' && c <= 'z') ||
                               (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') ||
                               c == '-' || c == '_';
            hostname_valid = hostname_valid && valid;
            if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c + 32);
            name.push_back(static_cast<char>(c));
        }
        expanded += label_len;
        if (expanded > 253) return false;
        cursor += 1 + label_len;
        if (!jumped) next_offset = cursor;
    }
    return false;
}

}  // namespace

std::vector<std::string> dns_extract_qnames(const uint8_t *data, size_t len,
                                            bool is_tcp) {
    std::vector<std::string> names;
    if (!data) return names;
    const uint8_t *p = data;
    size_t remain = len;
    // DNS-over-TCP:跳过 2 字节长度前缀
    if (is_tcp) {
        if (remain < 14) return names;   // 2 长度 + 12 头
        size_t message_len = read_u16(p);
        if (message_len < 12 || message_len > remain - 2) return names;
        p += 2;
        remain = message_len;
    } else {
        if (remain < 12) return names;
    }
    // 仅处理标准查询(QR=0);响应包不重复解析
    uint16_t qdcount = (uint16_t)read_u16(p + 4);
    if (qdcount == 0 || qdcount > 4) return names;
    if ((p[2] & 0x80) != 0) return names;   // QR 位=1 是响应,跳过

    size_t offset = 12;
    for (uint16_t question = 0; question < qdcount; ++question) {
        size_t next = offset;
        std::string name;
        if (!parse_dns_name(p, remain, offset, next, name) ||
            next > remain || remain - next < 4) {
            names.clear();
            return names;
        }
        if (!name.empty()) names.push_back(std::move(name));
        offset = next + 4;
    }
    return names;
}

std::string dns_extract_qname(const uint8_t *data, size_t len, bool is_tcp) {
    std::vector<std::string> names = dns_extract_qnames(data, len, is_tcp);
    return names.empty() ? std::string() : std::move(names.front());
}

namespace {
void write_be16(uint8_t *out, size_t value) {
    out[0] = static_cast<uint8_t>(value >> 8);
    out[1] = static_cast<uint8_t>(value);
}
uint32_t checksum_sum(const uint8_t *data, size_t len, uint32_t sum = 0) {
    while (len >= 2) { sum += read_u16(data); data += 2; len -= 2; }
    if (len) sum += static_cast<uint32_t>(data[0]) << 8;
    return sum;
}
uint16_t checksum_finish(uint32_t sum) {
    while (sum >> 16) sum = (sum & 0xffff) + (sum >> 16);
    return static_cast<uint16_t>(~sum);
}
std::vector<uint8_t> reply_header(const TransportView &v, uint8_t protocol, size_t size) {
    if (v.ip_version != 4 && v.ip_version != 6) return {};
    size_t ip_len = v.ip_version == 4 ? 20 : 40;
    std::vector<uint8_t> out(ip_len + size, 0);
    auto *ip = out.data();
    if (v.ip_version == 4) {
        ip[0] = 0x45; ip[8] = 64; ip[9] = protocol;
        write_be16(ip + 2, out.size());
        memcpy(ip + 12, v.dst_addr.data(), 4);
        memcpy(ip + 16, v.src_addr.data(), 4);
        write_be16(ip + 10, checksum_finish(checksum_sum(ip, 20)));
    } else {
        ip[0] = 0x60; ip[6] = protocol; ip[7] = 64;
        write_be16(ip + 4, size);
        memcpy(ip + 8, v.dst_addr.data(), 16);
        memcpy(ip + 24, v.src_addr.data(), 16);
    }
    return out;
}
void reply_checksum(std::vector<uint8_t> &packet, size_t checksum_offset) {
    bool v4 = (packet[0] >> 4) == 4;
    size_t ip_len = v4 ? 20 : 40, length = packet.size() - ip_len;
    uint8_t protocol = packet[v4 ? 9 : 6];
    uint32_t sum = 0;
    if (protocol != 1) {
        sum = checksum_sum(packet.data() + (v4 ? 12 : 8), v4 ? 8 : 32) + protocol + length;
    }
    uint16_t check = checksum_finish(checksum_sum(packet.data() + ip_len, length, sum));
    write_be16(packet.data() + ip_len + checksum_offset, check ? check : 0xffff);
}
}

std::vector<uint8_t> dns_negative_packet(const uint8_t *packet, size_t len,
                                         uint16_t hw_proto) {
    TransportView v;
    if (!decode_ip_transport(packet, len, hw_proto, v) || v.truncated ||
        v.protocol != TransportProtocol::UDP || v.dst_port != 53 ||
        v.src_port == 53 || v.payload_len < 12 || v.payload_len > 4000)
        return {};
    // Do not discard IP options/extension headers or rely on stale skb offsets.
    if ((v.ip_version == 4 && packet[0] != 0x45) ||
        (v.ip_version == 6 && packet[6] != 17)) return {};
    const uint8_t *dns = v.payload;
    if ((dns[2] & 0xf8) != 0 || read_u16(dns + 6) || read_u16(dns + 8)) return {};
    size_t questions = read_u16(dns + 4), end = 12;
    if (questions == 0 || questions > 4) return {};
    for (size_t i = 0; i < questions; ++i) {
        size_t next = end;
        std::string name;
        if (!parse_dns_name(dns, v.payload_len, end, next, name) || name.empty() ||
            next > v.payload_len || v.payload_len - next < 4) return {};
        end = next + 4;
    }
    // After removing EDNS/additional records, every compression pointer must
    // still resolve within the retained questions, never into removed data.
    size_t offset = 12;
    for (size_t i = 0; i < questions; ++i) {
        size_t next = offset;
        std::string name;
        if (!parse_dns_name(dns, end, offset, next, name) || name.empty()) return {};
        offset = next + 4;
    }
    const size_t ip_len = v.ip_version == 4 ? 20 : 40, udp_len = 8 + end;
    std::vector<uint8_t> out = reply_header(v, 17, udp_len);
    uint8_t *udp = out.data() + ip_len, *reply = udp + 8;
    write_be16(udp, 53); write_be16(udp + 2, v.src_port); write_be16(udp + 4, udp_len);
    memcpy(reply, dns, end);
    reply[2] = static_cast<uint8_t>(0x80 | (dns[2] & 1)); // QR + original RD
    reply[3] = static_cast<uint8_t>(0x83 | (dns[3] & 0x10)); // RA + NXDOMAIN, clear AD
    memset(reply + 6, 0, 6); // No answers, authority or additional records.
    reply_checksum(out, 6);
    return out;
}

std::vector<uint8_t> tcp_reset_packet(const TransportView &v) {
    if (v.protocol != TransportProtocol::TCP || (v.tcp_flags & 4)) return {};
    auto out = reply_header(v, 6, 20);
    if (out.empty()) return {};
    auto *tcp = out.data() + (v.ip_version == 4 ? 20 : 40);
    write_be16(tcp, v.dst_port); write_be16(tcp + 2, v.src_port);
    uint32_t sequence = 0, ack = 0;
    tcp[12] = 0x50;
    if (v.tcp_flags & 0x10) { sequence = v.tcp_acknowledgment; tcp[13] = 4; }
    else {
        ack = v.tcp_sequence + static_cast<uint32_t>(v.payload_len) +
              ((v.tcp_flags & 2) ? 1u : 0u) + ((v.tcp_flags & 1) ? 1u : 0u);
        tcp[13] = 0x14;
    }
    for (int i = 0; i < 4; ++i) {
        tcp[4 + i] = static_cast<uint8_t>(sequence >> (24 - 8 * i));
        tcp[8 + i] = static_cast<uint8_t>(ack >> (24 - 8 * i));
    }
    reply_checksum(out, 16);
    return out;
}

std::vector<uint8_t> udp_unreachable_packet(const uint8_t *packet, size_t len,
                                            uint16_t hw_proto) {
    TransportView v;
    if (!decode_ip_transport(packet, len, hw_proto, v) ||
        v.protocol != TransportProtocol::UDP || v.dst_port != 443) return {};
    // Quote the original IP header plus UDP header; cap IPv6 errors below its
    // minimum MTU (1280). Never emit an error in response to an ICMP error.
    size_t original_length = v.ip_version == 4 ? read_u16(packet + 2) : 40 + read_u16(packet + 4);
    size_t quote = v.ip_version == 4 ? ((packet[0] & 15) * 4u + 8u) : 1232u;
    quote = std::min(quote, std::min(len, original_length));
    auto out = reply_header(v, v.ip_version == 4 ? 1 : 58, 8 + quote);
    auto *icmp = out.data() + (v.ip_version == 4 ? 20 : 40);
    icmp[0] = v.ip_version == 4 ? 3 : 1;
    icmp[1] = v.ip_version == 4 ? 3 : 4;
    memcpy(icmp + 8, packet, quote);
    reply_checksum(out, 2);
    return out;
}

// ============================================================================
// TLS ClientHello SNI 解析
//
//   记录层: [type:1][ver:2][len:2] handshake...
//     type=0x16(handshake) ver=0x0301~0x0303
//   握手层: [type:1][len:3] body...
//     type=0x01(ClientHello)
//   ClientHello:
//     [ver:2][random:32][sid_len:1][sid][cs_len:2][cs][comp_len:1][comp]
//     [ext_total:2] extensions...
//   扩展:   [type:2][len:2][data]
//     type=0x0000(SNI): [list_len:2][type:1=host_name][len:2][name]
// =========================================================================
namespace {

constexpr size_t TLS_RECORD_LIMIT = 16u * 1024u;
constexpr size_t TLS_HANDSHAKE_LIMIT = 16u * 1024u;

bool normalize_hostname(const uint8_t *data, size_t len, std::string &out) {
    if (!data || len == 0 || len > 253) return false;
    out.assign(reinterpret_cast<const char *>(data), len);
    if (!out.empty() && out.back() == '.') out.pop_back();
    if (out.empty() || out.size() > 253 || out.find('.') == std::string::npos)
        return false;
    size_t label_start = 0;
    for (size_t i = 0; i <= out.size(); ++i) {
        if (i != out.size() && out[i] != '.') {
            unsigned char value = static_cast<unsigned char>(out[i]);
            if (value >= 'A' && value <= 'Z') {
                out[i] = static_cast<char>(value + ('a' - 'A'));
                value = static_cast<unsigned char>(out[i]);
            }
            if (!((value >= 'a' && value <= 'z') ||
                  (value >= '0' && value <= '9') || value == '-'))
                return false;
            continue;
        }
        const size_t label_len = i - label_start;
        if (label_len == 0 || label_len > 63 || out[label_start] == '-' ||
            out[i - 1] == '-')
            return false;
        label_start = i + 1;
    }
    return true;
}

DomainParseResult parse_client_hello(const uint8_t *hs, size_t available) {
    if (available < 4) return {DomainParseStatus::NEED_MORE, ""};
    if (hs[0] != 0x01) return {DomainParseStatus::NO_DOMAIN, ""};
    const size_t hs_len = (static_cast<size_t>(hs[1]) << 16) |
                          (static_cast<size_t>(hs[2]) << 8) | hs[3];
    if (hs_len < 41 || hs_len > TLS_HANDSHAKE_LIMIT - 4)
        return {DomainParseStatus::MALFORMED, ""};
    if (available < hs_len + 4) return {DomainParseStatus::NEED_MORE, ""};

    const uint8_t *ch = hs + 4;
    const size_t ch_len = hs_len;
    size_t pos = 0;
    if (ch_len < 35) return {DomainParseStatus::MALFORMED, ""};
    pos = 34;  // legacy_version + random
    const size_t sid_len = ch[pos++];
    if (sid_len > ch_len - pos) return {DomainParseStatus::MALFORMED, ""};
    pos += sid_len;
    if (ch_len - pos < 2) return {DomainParseStatus::MALFORMED, ""};
    const size_t cs_len = read_u16(ch + pos);
    pos += 2;
    if (cs_len < 2 || (cs_len & 1u) != 0 || cs_len > ch_len - pos)
        return {DomainParseStatus::MALFORMED, ""};
    pos += cs_len;
    if (ch_len - pos < 1) return {DomainParseStatus::MALFORMED, ""};
    const size_t comp_len = ch[pos++];
    if (comp_len == 0 || comp_len > ch_len - pos)
        return {DomainParseStatus::MALFORMED, ""};
    pos += comp_len;
    if (pos == ch_len) return {DomainParseStatus::NO_DOMAIN, ""};
    if (ch_len - pos < 2) return {DomainParseStatus::MALFORMED, ""};
    const size_t ext_total = read_u16(ch + pos);
    pos += 2;
    if (ext_total != ch_len - pos)
        return {DomainParseStatus::MALFORMED, ""};
    const size_t ext_end = pos + ext_total;
    while (pos + 4 <= ext_end) {
        const uint16_t etype = static_cast<uint16_t>(read_u16(ch + pos));
        const size_t elen = read_u16(ch + pos + 2);
        pos += 4;
        if (elen > ext_end - pos)
            return {DomainParseStatus::MALFORMED, ""};
        if (etype == 0x0000) {
            if (elen < 2) return {DomainParseStatus::MALFORMED, ""};
            const size_t list_len = read_u16(ch + pos);
            if (list_len != elen - 2)
                return {DomainParseStatus::MALFORMED, ""};
            size_t name_pos = pos + 2;
            const size_t name_end = pos + elen;
            while (name_pos < name_end) {
                if (name_end - name_pos < 3)
                    return {DomainParseStatus::MALFORMED, ""};
                const uint8_t name_type = ch[name_pos++];
                const size_t name_len = read_u16(ch + name_pos);
                name_pos += 2;
                if (name_len > name_end - name_pos)
                    return {DomainParseStatus::MALFORMED, ""};
                if (name_type == 0) {
                    std::string domain;
                    if (!normalize_hostname(ch + name_pos, name_len, domain))
                        return {DomainParseStatus::MALFORMED, ""};
                    return {DomainParseStatus::FOUND, std::move(domain)};
                }
                name_pos += name_len;
            }
            return {DomainParseStatus::NO_DOMAIN, ""};
        }
        pos += elen;
    }
    if (pos != ext_end) return {DomainParseStatus::MALFORMED, ""};
    return {DomainParseStatus::NO_DOMAIN, ""};
}

bool starts_with_http_method(const uint8_t *data, size_t len) {
    static const char *methods[] = {
        "GET ", "POST ", "PUT ", "HEAD ", "DELETE ", "OPTIONS ",
        "PATCH ", "CONNECT ", "TRACE ",
    };
    for (const char *method : methods) {
        const size_t method_len = strlen(method);
        if (len >= method_len && memcmp(data, method, method_len) == 0) return true;
    }
    return false;
}

bool ascii_equal_ignore_case(const uint8_t *data, size_t len,
                             const char *expected) {
    if (strlen(expected) != len) return false;
    for (size_t i = 0; i < len; ++i) {
        unsigned char value = data[i];
        if (value >= 'A' && value <= 'Z') value += 'a' - 'A';
        if (value != static_cast<unsigned char>(expected[i])) return false;
    }
    return true;
}

}  // namespace

DomainParseResult tls_inspect_client_hello(const uint8_t *data, size_t len) {
    if (!data) return {DomainParseStatus::MALFORMED, ""};
    std::vector<uint8_t> handshake;
    size_t offset = 0;
    size_t required = 4;
    while (handshake.size() < required) {
        if (len - offset < 5) return {DomainParseStatus::NEED_MORE, ""};
        if (data[offset] != 0x16 || data[offset + 1] != 0x03)
            return {DomainParseStatus::NO_DOMAIN, ""};
        const size_t record_len = read_u16(data + offset + 3);
        if (record_len > TLS_RECORD_LIMIT)
            return {DomainParseStatus::MALFORMED, ""};
        if (record_len > len - offset - 5)
            return {DomainParseStatus::NEED_MORE, ""};
        if (record_len > TLS_HANDSHAKE_LIMIT - handshake.size())
            return {DomainParseStatus::MALFORMED, ""};
        handshake.insert(handshake.end(), data + offset + 5,
                         data + offset + 5 + record_len);
        offset += 5 + record_len;
        if (handshake.size() >= 4) {
            required = 4 + (static_cast<size_t>(handshake[1]) << 16) +
                       (static_cast<size_t>(handshake[2]) << 8) + handshake[3];
            if (required > TLS_HANDSHAKE_LIMIT)
                return {DomainParseStatus::MALFORMED, ""};
        }
    }
    return parse_client_hello(handshake.data(), handshake.size());
}

std::string tls_extract_sni(const uint8_t *data, size_t len) {
    DomainParseResult result = tls_inspect_client_hello(data, len);
    return result.status == DomainParseStatus::FOUND ? std::move(result.domain) : "";
}

// ============================================================================
// HTTP Host 头解析
//   请求行 + 头部;查找 "host:"(大小写不敏感),取到行尾/CRLF/端口前的值
// =========================================================================
std::string http_extract_host(const uint8_t *data, size_t len) {
    if (!data || len < 16 || !starts_with_http_method(data, len)) return "";
    const size_t limit = len < 8192 ? len : 8192;
    size_t line_start = 0;
    bool request_line = true;
    while (line_start < limit) {
        size_t lf = line_start;
        while (lf < limit && data[lf] != '\n') ++lf;
        if (lf == limit) return "";  // 不对尚未收完整的 header line 提前判定
        size_t line_end = lf;
        if (line_end > line_start && data[line_end - 1] == '\r') --line_end;
        if (request_line) {
            request_line = false;
            line_start = lf + 1;
            continue;
        }
        if (line_end == line_start) return "";  // headers 完成但没有 Host

        size_t colon = line_start;
        while (colon < line_end && data[colon] != ':') ++colon;
        if (colon < line_end &&
            ascii_equal_ignore_case(data + line_start, colon - line_start, "host")) {
            size_t value_start = colon + 1;
            while (value_start < line_end &&
                   (data[value_start] == ' ' || data[value_start] == '\t'))
                ++value_start;
            size_t value_end = line_end;
            while (value_end > value_start &&
                   (data[value_end - 1] == ' ' || data[value_end - 1] == '\t'))
                --value_end;
            if (value_start == value_end || data[value_start] == '[') return "";
            size_t host_end = value_start;
            while (host_end < value_end && data[host_end] != ':') ++host_end;
            if (host_end < value_end) {
                if (++host_end == value_end) return "";
                for (size_t i = host_end; i < value_end; ++i)
                    if (data[i] < '0' || data[i] > '9') return "";
                --host_end;
            }
            std::string host;
            if (!normalize_hostname(data + value_start, host_end - value_start, host))
                return "";
            return host;
        }
        line_start = lf + 1;
    }
    return "";
}

// TLS 优先(TLS 流量大),失败再试 HTTP
std::string extract_domain_from_stream(const uint8_t *data, size_t len) {
    std::string s = tls_extract_sni(data, len);
    if (!s.empty()) return s;
    return http_extract_host(data, len);
}
