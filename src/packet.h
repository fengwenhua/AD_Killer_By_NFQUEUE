// ============================================================================
// AD_Killer_By_NFQUEUE - 报文解析:DNS QNAME / TLS ClientHello SNI / HTTP Host
//
// 全部为无依赖纯函数,手工处理网络字节序(BE)。
// 三条提取路径对应 NFQUEUE 三个抓包点:
//   queue 0 → DNS 查询的 QNAME
//   queue 1 → TLS ClientHello 的 SNI 扩展 或 HTTP 请求头 Host
// ============================================================================
#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <string>
#include <vector>

// NFQUEUE 的 hardware protocol 使用主机字节序保存。
constexpr uint16_t ETH_PROTO_IPV4 = 0x0800;
constexpr uint16_t ETH_PROTO_IPV6 = 0x86DD;

enum class TransportProtocol : uint8_t {
    UDP = 17,
    TCP = 6,
};

enum class TransportParseStatus : uint8_t {
    OK,
    TRUNCATED,
    FRAGMENTED,
    MALFORMED,
    EXTENSION_LIMIT,
    UNSUPPORTED,
};

struct IpExtensionStats {
    uint64_t ipv4_fragments = 0;
    uint64_t ipv6_fragments = 0;
    uint64_t ipv6_extension_errors = 0;
};

// 从完整 IP 报文中解析出的传输层视图。payload 已跳过 UDP/TCP 头。
// 指针只在原始 packet 缓冲区有效期间有效。
struct TransportView {
    TransportProtocol protocol = TransportProtocol::UDP;
    const uint8_t *payload = nullptr;
    size_t payload_len = 0;
    uint16_t src_port = 0;
    uint16_t dst_port = 0;
    uint8_t ip_version = 0;
    std::array<uint8_t, 16> src_addr{};
    std::array<uint8_t, 16> dst_addr{};
    uint32_t tcp_sequence = 0;
    uint32_t tcp_acknowledgment = 0;
    uint8_t tcp_flags = 0;
    bool truncated = false;
    TransportParseStatus status = TransportParseStatus::MALFORMED;
};

// 解析 IPv4/IPv6、常见 IPv6 扩展头和 UDP/TCP 头。真实分片不做重组并返回
// FRAGMENTED，由调用方安全放行；IPv6 atomic fragment 可继续解析。
bool decode_ip_transport(const uint8_t *packet, size_t packet_len,
                         uint16_t hw_proto, TransportView &out);

IpExtensionStats ip_extension_stats();
void reset_ip_extension_stats();

// 解析 DNS 查询域名(QDCOUNT>=1 的标准查询)
// data: 传输层载荷;is_tcp: DNS-over-TCP 前置 2 字节长度域
// 返回空串表示解析失败(调用方应放行)
std::string dns_extract_qname(const uint8_t *data, size_t len, bool is_tcp);
// 解析全部问题项（最多 4 个），包括合法的 DNS 压缩指针；结构非法返回空数组。
std::vector<std::string> dns_extract_qnames(const uint8_t *data, size_t len,
                                            bool is_tcp);
// Build a bounded UDP DNS NXDOMAIN response (complete IP packet), preserving
// transaction ID and question bytes. Unsupported/truncated input returns empty.
std::vector<uint8_t> dns_negative_packet(const uint8_t *packet, size_t len,
                                         uint16_t hw_proto);
std::vector<uint8_t> tcp_reset_packet(const TransportView &flow);
std::vector<uint8_t> udp_unreachable_packet(const uint8_t *packet, size_t len,
                                            uint16_t hw_proto);

enum class DomainParseStatus : uint8_t {
    NEED_MORE,
    FOUND,
    NO_DOMAIN,
    MALFORMED,
};

struct DomainParseResult {
    DomainParseStatus status = DomainParseStatus::MALFORMED;
    std::string domain;
};

// 解析 TLS ClientHello 提取 SNI(服务端名指示扩展 type=0x0000)
// 支持一个 ClientHello 跨多个连续 TLS handshake record。
DomainParseResult tls_inspect_client_hello(const uint8_t *data, size_t len);

// 兼容纯提取调用；只有 FOUND 返回域名，其余状态返回空串。
std::string tls_extract_sni(const uint8_t *data, size_t len);

// 解析 HTTP 请求 Host 头；只接受完整 header line，不匹配路径或 X-Host。
// data: TCP 载荷(GET/POST 等请求方法打头)
std::string http_extract_host(const uint8_t *data, size_t len);

// 二选一:TLS ClientHello 或 HTTP 请求 → 域名;否则空
std::string extract_domain_from_stream(const uint8_t *data, size_t len);
