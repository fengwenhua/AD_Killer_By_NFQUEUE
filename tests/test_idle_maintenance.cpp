// The actual owner maintenance path; no real queues/routes or wall-clock wait.
#include "../src/engine.cpp"
#include <cassert>

FastResponseSender::~FastResponseSender() {} // No sockets are opened by this test.

int main() {
    TcpFlowKey handshake {}; handshake.ip_version = 4; handshake.src_port = 12345; handshake.dst_port = 80;
    const std::string partial = "GET / HTTP/1.1\r\nHost: incomplete";
    assert(g_handshakes.inspect(handshake, 100, 0x18,
        reinterpret_cast<const uint8_t *>(partial.data()), partial.size(), 1).decision == ReassemblyDecision::NEED_MORE);
    auto dns = handshake; dns.dst_port = 53;
    const uint8_t frame[] = {0, 20, 1};
    assert(g_dns_tcp.inspect(dns, 100, 0x18, frame, sizeof(frame), 1).decision == DnsTcpDecision::NEED_MORE);
    assert(g_handshakes.stats().flows == 1 && g_dns_tcp.retained().first == 1);
    maintain_reassembly(0); // A clock regression never underflows expiry.
    maintain_reassembly(10001); // Exactly the idle timeout remains eligible.
    assert(g_handshakes.stats().flows == 1 && g_dns_tcp.retained().first == 1);
    maintain_reassembly(10002); // No additional packet or stats side effect.
    assert(g_handshakes.stats().flows == 0 && g_handshakes.stats().bytes == 0);
    assert(g_handshakes.stats().expired == 1 && g_dns_tcp.retained() == std::make_pair(size_t(0), size_t(0)));
    puts("actual owner maintenance releases idle HTTP/TLS and TCP DNS prefixes without new traffic");
}
