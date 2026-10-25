// Actual packet callbacks and parsers, isolated netlink/reply outcomes. No I/O.
#include "../src/engine.cpp"
#include <cassert>

namespace {
bool reply_ok = true, verdict_ok = true, ct_ok = true;
uint32_t last_verdict = 99;
unsigned verdict_calls = 0;
Verdict choice = Verdict::BLOCK_EXACT;
void put16(std::vector<uint8_t> &p, size_t at, size_t value) { p[at] = value >> 8; p[at + 1] = value; }
std::vector<uint8_t> packet(const std::vector<uint8_t> &data, bool tcp = false, unsigned port = 53) {
    const size_t h = tcp ? 40 : 28;
    std::vector<uint8_t> p(h + data.size());
    p[0] = 0x45; put16(p, 2, p.size()); p[8] = 64; p[9] = tcp ? 6 : 17;
    p[12] = 192; p[15] = 2; p[16] = 8; p[19] = 8;
    put16(p, 20, 53001); put16(p, 22, port);
    if (tcp) { p[27] = 100; p[32] = 0x50; p[33] = 0x18; }
    else put16(p, 24, 8 + data.size());
    std::copy(data.begin(), data.end(), p.begin() + h); return p;
}
bool dispatch(std::vector<uint8_t> bytes, bool dns = true) {
    NfqPacket p; p.id = 7; p.hw_proto = ETH_PROTO_IPV4; p.payload = bytes.data(); p.payload_len = bytes.size();
    return dns ? on_dns_packet(p, nullptr) : on_tls_packet(p, nullptr);
}
std::vector<uint8_t> query() {
    return {0x12,0x34,1,0,0,1,0,0,0,0,0,0,
        3,'a','d','s',7,'e','x','a','m','p','l','e',3,'c','o','m',0,0,1,0,1};
}
}
FastResponseSender::~FastResponseSender() {}
bool FastResponseSender::send(const std::vector<uint8_t> &, uint32_t) { errno = EIO; return reply_ok; }
bool nfq_set_verdict(int, uint16_t, uint32_t, uint32_t verdict) { ++verdict_calls; last_verdict = verdict; return verdict_ok; }
bool nfq_set_verdict_with_connmark(int fd, uint16_t q, uint32_t id, uint32_t v, uint32_t, uint32_t) { return nfq_set_verdict(fd, q, id, v); }
bool nfct_set_mark(int, const TransportView &, uint32_t, uint32_t) { errno = EPERM; return ct_ok; }
Verdict match_domain(const std::string &) { return choice; }
const char *verdict_text(Verdict) { return "test"; }
void log_dns_query(const std::string &, const char *) {}
void log_line(const char *, const char *, ...) {}
bool write_file_atomic(const char *, const std::string &) { return true; }

int main() {
    auto multiple = query(); multiple[5] = 2; multiple.insert(multiple.end(), {0xc0,12,0,28,0,1});
    auto before = g_metrics.snapshot();
    assert(dispatch(packet(multiple)));
    auto after = g_metrics.snapshot();
    assert(after.block_exact_hits - before.block_exact_hits == 2);
    assert(after.dns_negative_replies - before.dns_negative_replies == 1 && after.dns_drop_verdicts - before.dns_drop_verdicts == 1);

    reply_ok = false; before = after;
    assert(dispatch(packet(query())) && last_verdict == NF_ACCEPT_);
    after = g_metrics.snapshot();
    assert(after.block_exact_hits == before.block_exact_hits + 1 && after.response_failures == before.response_failures + 1);
    assert(after.domain_parse_failures == before.domain_parse_failures); // A send error is not a qname parse failure.
    assert(after.dns_drop_verdicts == before.dns_drop_verdicts && after.dns_negative_replies == before.dns_negative_replies);

    reply_ok = true; verdict_ok = false; before = after;
    assert(!dispatch(packet(query()))); after = g_metrics.snapshot();
    assert(after.dns_negative_replies == before.dns_negative_replies + 1 && after.verdict_failures == before.verdict_failures + 1);
    assert(after.dns_drop_verdicts == before.dns_drop_verdicts);

    verdict_ok = true; g_applied_block_quic = true; before = after;
    assert(dispatch(packet({1,2,3}, false, 443), false)); after = g_metrics.snapshot();
    assert(after.quic_rejects == before.quic_rejects + 1 && after.inspected_drop_verdicts == before.inspected_drop_verdicts + 1);
    assert(after.block_exact_hits == before.block_exact_hits);

    const std::string http = "GET / HTTP/1.1\r\nHost: ads.example.com\r\n\r\n";
    const auto tcp = packet(std::vector<uint8_t>(http.begin(), http.end()), true, 80);
    reply_ok = false; before = after;
    assert(dispatch(tcp, false) && last_verdict == NF_ACCEPT_); after = g_metrics.snapshot();
    assert(after.response_failures == before.response_failures + 1 && after.inspected_drop_verdicts == before.inspected_drop_verdicts);
    reply_ok = true; g_handshakes.clear();
    before = after;
    assert(dispatch(tcp, false) && dispatch(tcp, false)); after = g_metrics.snapshot();
    assert(after.block_exact_hits == before.block_exact_hits + 1 && after.blocked_retransmits == before.blocked_retransmits + 1);
    assert(after.inspected_drop_verdicts == before.inspected_drop_verdicts + 2 && after.tcp_resets == before.tcp_resets + 2);
    g_handshakes.clear(); verdict_ok = false; before = after;
    assert(!dispatch(tcp, false)); after = g_metrics.snapshot();
    assert(after.inspected_drop_verdicts == before.inspected_drop_verdicts && after.verdict_failures == before.verdict_failures + 1);
    verdict_ok = true;

    auto frame = query(); frame.insert(frame.begin(), {0, static_cast<uint8_t>(frame.size())});
    const auto dns_tcp = packet(frame, true);
    before = after;
    assert(dispatch(dns_tcp) && dispatch(dns_tcp)); after = g_metrics.snapshot();
    assert(after.block_exact_hits == before.block_exact_hits + 1 && after.dns_drop_verdicts == before.dns_drop_verdicts + 2);
    assert(after.blocked_retransmits == before.blocked_retransmits + 1);

    g_handshakes.clear(); g_ct_fds[1] = 7; ct_ok = false; before = after;
    const auto calls = verdict_calls;
    assert(!dispatch(tcp, false)); after = g_metrics.snapshot();
    assert(verdict_calls == calls && after.inspected_drop_verdicts == before.inspected_drop_verdicts && after.verdict_failures == before.verdict_failures + 1);
    puts("packet metrics: multi-question hits, safe pass, response/verdict/CT failure, QUIC and cached TCP/DNS rejection stay distinct");
}
