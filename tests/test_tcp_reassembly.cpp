#include "tcp_reassembly.h"

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

void append_u16(std::vector<uint8_t> &data, size_t value) {
    data.push_back(static_cast<uint8_t>(value >> 8));
    data.push_back(static_cast<uint8_t>(value));
}

void append_u24(std::vector<uint8_t> &data, size_t value) {
    data.push_back(static_cast<uint8_t>(value >> 16));
    data.push_back(static_cast<uint8_t>(value >> 8));
    data.push_back(static_cast<uint8_t>(value));
}

std::vector<uint8_t> client_hello(const std::string &host) {
    std::vector<uint8_t> body = {0x03, 0x03};
    body.insert(body.end(), 32, 0x42);        // random
    body.push_back(0);                        // session id length
    append_u16(body, 2);                      // cipher suites length
    append_u16(body, 0x1301);
    body.push_back(1);                        // compression methods length
    body.push_back(0);

    std::vector<uint8_t> sni;
    append_u16(sni, 3 + host.size());         // server name list length
    sni.push_back(0);                         // host_name
    append_u16(sni, host.size());
    sni.insert(sni.end(), host.begin(), host.end());
    std::vector<uint8_t> extensions;
    append_u16(extensions, 0);                // SNI type
    append_u16(extensions, sni.size());
    extensions.insert(extensions.end(), sni.begin(), sni.end());
    append_u16(body, extensions.size());
    body.insert(body.end(), extensions.begin(), extensions.end());

    std::vector<uint8_t> handshake = {0x01};
    append_u24(handshake, body.size());
    handshake.insert(handshake.end(), body.begin(), body.end());
    std::vector<uint8_t> record = {0x16, 0x03, 0x03};
    append_u16(record, handshake.size());
    record.insert(record.end(), handshake.begin(), handshake.end());
    return record;
}

std::vector<uint8_t> split_tls_records(const std::vector<uint8_t> &record,
                                       size_t first_payload) {
    const size_t payload_size = record.size() - 5;
    if (first_payload >= payload_size) return record;
    std::vector<uint8_t> split = {0x16, 0x03, 0x03};
    append_u16(split, first_payload);
    split.insert(split.end(), record.begin() + 5,
                 record.begin() + 5 + first_payload);
    split.insert(split.end(), {0x16, 0x03, 0x03});
    append_u16(split, payload_size - first_payload);
    split.insert(split.end(), record.begin() + 5 + first_payload, record.end());
    return split;
}

TcpFlowKey flow_key(uint8_t id) {
    TcpFlowKey key;
    key.ip_version = 4;
    key.src_addr[0] = 10;
    key.src_addr[3] = id;
    key.dst_addr[0] = 203;
    key.dst_addr[1] = 0;
    key.dst_addr[2] = 113;
    key.dst_addr[3] = id;
    key.src_port = static_cast<uint16_t>(40000 + id);
    key.dst_port = 443;
    return key;
}

}  // namespace

int main() {
    HandshakeReassembler reassembler;
    TcpFlowKey tls_key = flow_key(1);
    std::vector<uint8_t> tls = client_hello("Ads.Example.COM");
    size_t first = 7;
    size_t second = 31;
    expect(reassembler.inspect(tls_key, 1000, 0x18, tls.data(), first, 1).decision ==
               ReassemblyDecision::NEED_MORE,
           "first TLS fragment should wait for more bytes");
    expect(reassembler.inspect(tls_key, 1000 + first, 0x18,
                               tls.data() + first, second - first, 2).decision ==
               ReassemblyDecision::NEED_MORE,
           "middle TLS fragment should remain incomplete");
    ReassemblyResult tls_result = reassembler.inspect(
        tls_key, 1000 + second, 0x18, tls.data() + second,
        tls.size() - second, 3);
    expect(tls_result.decision == ReassemblyDecision::DOMAIN_FOUND &&
               tls_result.domain == "ads.example.com",
           "segmented ClientHello should yield normalized SNI");
    expect(reassembler.stats().bytes == tls.size(),
           "contiguous TLS bytes should remain bounded and deduplicated");

    reassembler.mark_blocked(tls_key, 3);
    expect(reassembler.stats().bytes == 0,
           "blocked flows should release buffered handshake bytes");
    expect(reassembler.inspect(tls_key, 1000, 0x18, tls.data(), first, 4).decision ==
               ReassemblyDecision::BLOCKED,
           "blocked state should cover retransmitted handshake segments");

    std::vector<uint8_t> multi_record = split_tls_records(tls, 23);
    DomainParseResult multi_parsed = tls_inspect_client_hello(
        multi_record.data(), multi_record.size());
    expect(multi_parsed.status == DomainParseStatus::FOUND &&
               multi_parsed.domain == "ads.example.com",
           "ClientHello split across TLS records should expose SNI");
    DomainParseResult incomplete_tls = tls_inspect_client_hello(
        multi_record.data(), multi_record.size() - 3);
    expect(incomplete_tls.status == DomainParseStatus::NEED_MORE,
           "partial trailing TLS record should request more bytes");
    TcpFlowKey multi_key = flow_key(7);
    ReassemblyResult multi_result = reassembler.inspect(
        multi_key, 7000, 0x18, multi_record.data(), multi_record.size(), 5);
    expect(multi_result.decision == ReassemblyDecision::DOMAIN_FOUND &&
               multi_result.domain == "ads.example.com",
           "stream reassembly should parse multi-record ClientHello");
    reassembler.forget(multi_key);

    TcpFlowKey tfo_key = flow_key(9);
    reassembler.inspect(tfo_key, 9000, 0x02, tls.data(), first, 6);
    ReassemblyResult tfo = reassembler.inspect(
        tfo_key, 9001 + first, 0x18, tls.data() + first, tls.size() - first, 7);
    expect(tfo.decision == ReassemblyDecision::DOMAIN_FOUND &&
               tfo.domain == "ads.example.com",
           "TLS over TCP Fast Open must account for the SYN sequence number");
    reassembler.forget(tfo_key);

    TcpFlowKey http_key = flow_key(2);
    std::string request =
        "GET / HTTP/1.1\r\nUser-Agent: test\r\nHost: Track.Example.org\r\n\r\n";
    size_t split = 24;
    expect(reassembler.inspect(
               http_key, 5000 + split, 0x18,
               reinterpret_cast<const uint8_t *>(request.data() + split),
               request.size() - split, 10).decision == ReassemblyDecision::NEED_MORE,
           "out-of-order HTTP tail should be retained briefly");
    ReassemblyResult http_result = reassembler.inspect(
        http_key, 5000, 0x18,
        reinterpret_cast<const uint8_t *>(request.data()), split, 11);
    expect(http_result.decision == ReassemblyDecision::DOMAIN_FOUND &&
               http_result.domain == "track.example.org",
           "out-of-order HTTP headers should reassemble by TCP sequence");
    size_t before_retransmit = reassembler.stats().bytes;
    reassembler.inspect(http_key, 5000, 0x18,
                        reinterpret_cast<const uint8_t *>(request.data()), split, 12);
    expect(reassembler.stats().bytes == before_retransmit,
           "overlapping retransmissions should not duplicate stored bytes");
    reassembler.forget(http_key);

    TcpFlowKey strict_http_key = flow_key(8);
    std::string strict_request =
        "GET /host:fake.example HTTP/1.1\r\n"
        "X-Host: fake.example\r\nHost: Ads.Example.COM:8080\r\n\r\n";
    const size_t host_split = strict_request.find(".COM") + 1;
    expect(reassembler.inspect(
               strict_http_key, 8000, 0x18,
               reinterpret_cast<const uint8_t *>(strict_request.data()),
               host_split, 20).decision == ReassemblyDecision::NEED_MORE,
           "partial Host line must not produce an early domain decision");
    ReassemblyResult strict_http = reassembler.inspect(
        strict_http_key, 8000 + host_split, 0x18,
        reinterpret_cast<const uint8_t *>(strict_request.data() + host_split),
        strict_request.size() - host_split, 21);
    expect(strict_http.decision == ReassemblyDecision::DOMAIN_FOUND &&
               strict_http.domain == "ads.example.com",
           "only the complete Host header should be used, with port removed");
    reassembler.forget(strict_http_key);

    std::string deceptive =
        "GET /host:ads.example HTTP/1.1\r\nX-Host: ads.example\r\n\r\n";
    expect(http_extract_host(reinterpret_cast<const uint8_t *>(deceptive.data()),
                             deceptive.size()).empty(),
           "request targets and X-Host headers must not be mistaken for Host");

    ReassemblyLimits limits;
    limits.per_flow_bytes = 32;
    limits.total_bytes = 48;
    limits.max_flows = 2;
    limits.idle_timeout_ms = 5;
    limits.blocked_timeout_ms = 7;
    HandshakeReassembler bounded(limits);
    std::string partial(40, 'x');
    expect(bounded.inspect(flow_key(3), 1, 0x18,
                           reinterpret_cast<const uint8_t *>(partial.data()),
                           partial.size(), 1).decision ==
               ReassemblyDecision::GIVE_UP,
           "per-flow byte limit should reject oversized prefixes");

    std::string small = "GET / HTTP";
    bounded.inspect(flow_key(4), 1, 0x18,
                    reinterpret_cast<const uint8_t *>(small.data()), small.size(), 2);
    bounded.inspect(flow_key(5), 1, 0x18,
                    reinterpret_cast<const uint8_t *>(small.data()), small.size(), 3);
    bounded.inspect(flow_key(6), 1, 0x18,
                    reinterpret_cast<const uint8_t *>(small.data()), small.size(), 4);
    expect(bounded.stats().flows <= 2 && bounded.stats().evicted >= 1,
           "flow count should evict the least recently used handshake");
    bounded.prune(20);
    expect(bounded.stats().flows == 0 && bounded.stats().bytes == 0 &&
               bounded.stats().expired >= 1,
           "idle timeout should release all cached flow memory");

    ReassemblyLimits global_limits = limits;
    global_limits.max_flows = 10;
    global_limits.per_flow_bytes = 32;
    global_limits.total_bytes = 48;
    HandshakeReassembler global_bounded(global_limits);
    std::string payload_24(24, 'x');
    for (uint8_t id = 10; id < 13; ++id) {
        global_bounded.inspect(
            flow_key(id), 1, 0x18,
            reinterpret_cast<const uint8_t *>(payload_24.data()),
            payload_24.size(), id);
    }
    expect(global_bounded.stats().bytes <= global_limits.total_bytes &&
               global_bounded.stats().evicted >= 1,
           "global byte budget should evict old flows before exceeding its cap");

    if (failures != 0) {
        std::cerr << failures << " TCP reassembly test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "TCP reassembly tests passed\n";
    return EXIT_SUCCESS;
}
