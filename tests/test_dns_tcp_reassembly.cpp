#include "dns_tcp_reassembly.h"

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

std::vector<uint8_t> dns_frame(const std::string &host, uint16_t id) {
    std::vector<uint8_t> message = {
        static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id),
        0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00,
    };
    size_t start = 0;
    while (start < host.size()) {
        size_t dot = host.find('.', start);
        size_t end = dot == std::string::npos ? host.size() : dot;
        message.push_back(static_cast<uint8_t>(end - start));
        message.insert(message.end(), host.begin() + start, host.begin() + end);
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    message.insert(message.end(), {0, 0, 1, 0, 1});
    std::vector<uint8_t> frame = {
        static_cast<uint8_t>(message.size() >> 8),
        static_cast<uint8_t>(message.size()),
    };
    frame.insert(frame.end(), message.begin(), message.end());
    return frame;
}

TcpFlowKey flow_key(uint8_t id) {
    TcpFlowKey key;
    key.ip_version = 4;
    key.src_addr[0] = 10;
    key.src_addr[3] = id;
    key.dst_addr[0] = 8;
    key.dst_addr[3] = 8;
    key.src_port = static_cast<uint16_t>(41000 + id);
    key.dst_port = 53;
    return key;
}

}  // namespace

int main() {
    DnsTcpReassembler reassembler;
    const auto first = dns_frame("ads.example.com", 1);
    TcpFlowKey key = flow_key(1);
    const size_t split = 9;
    expect(reassembler.inspect(key, 1000, 0x18, first.data(), split, 1).decision ==
               DnsTcpDecision::NEED_MORE,
           "partial TCP DNS frame should wait for its remaining bytes");
    DnsTcpResult completed = reassembler.inspect(
        key, 1000 + split, 0x18, first.data() + split, first.size() - split, 2);
    expect(completed.decision == DnsTcpDecision::MESSAGES_READY &&
               completed.domains.size() == 1 &&
               completed.domains[0] == "ads.example.com",
           "segmented TCP DNS query should yield its normalized qname");
    reassembler.finish(key, true, 2);
    expect(reassembler.inspect(key, 1000, 0x18, first.data(), first.size(), 3).decision ==
               DnsTcpDecision::BLOCKED,
           "blocked TCP DNS flow should cover an already queued retransmission");

    const auto second = dns_frame("safe.example.org", 2);
    TcpFlowKey out_of_order = flow_key(2);
    const size_t head = 13;
    expect(reassembler.inspect(
               out_of_order, 2000 + head, 0x18, second.data() + head,
               second.size() - head, 4).decision == DnsTcpDecision::NEED_MORE,
           "out-of-order DNS tail should be retained briefly");
    DnsTcpResult reordered = reassembler.inspect(
        out_of_order, 2000, 0x18, second.data(), head, 5);
    expect(reordered.decision == DnsTcpDecision::MESSAGES_READY &&
               reordered.domains.size() == 1 &&
               reordered.domains[0] == "safe.example.org",
           "out-of-order TCP DNS segments should merge by sequence");
    reassembler.finish(out_of_order, false, 5);

    std::vector<uint8_t> coalesced = first;
    coalesced.insert(coalesced.end(), second.begin(), second.end());
    TcpFlowKey multi = flow_key(3);
    DnsTcpResult multiple = reassembler.inspect(
        multi, 3000, 0x18, coalesced.data(), coalesced.size(), 6);
    expect(multiple.decision == DnsTcpDecision::MESSAGES_READY &&
               multiple.domains.size() == 2 &&
               multiple.domains[0] == "ads.example.com" &&
               multiple.domains[1] == "safe.example.org",
           "coalesced TCP segment should inspect every complete DNS message");
    reassembler.finish(multi, false, 6);

    TcpFlowKey pipelined = flow_key(5);
    std::vector<uint8_t> partial_next = second;
    partial_next.insert(partial_next.end(), first.begin(), first.begin() + split);
    DnsTcpResult ready_first = reassembler.inspect(
        pipelined, 5000, 0x18, partial_next.data(), partial_next.size(), 10);
    expect(ready_first.decision == DnsTcpDecision::MESSAGES_READY &&
               ready_first.domains == std::vector<std::string>{"safe.example.org"},
           "complete query must be inspected even when followed by a partial frame");
    reassembler.finish(pipelined, false, 10);
    expect(reassembler.inspect(pipelined, 5000, 0x18, second.data(),
                               second.size(), 11).decision == DnsTcpDecision::NEED_MORE,
           "retransmitted consumed query must not corrupt the pending frame");
    DnsTcpResult ready_next = reassembler.inspect(
        pipelined, 5000 + partial_next.size(), 0x18, first.data() + split,
        first.size() - split, 12);
    expect(ready_next.decision == DnsTcpDecision::MESSAGES_READY &&
               ready_next.domains == std::vector<std::string>{"ads.example.com"},
           "pending DNS frame must survive consumption of the preceding allowed query");
    reassembler.finish(pipelined, true, 12);

    TcpFlowKey fast_open = flow_key(6);
    reassembler.inspect(fast_open, 6000, 0x02, first.data(), split, 13);
    DnsTcpResult tfo = reassembler.inspect(
        fast_open, 6001 + split, 0x18, first.data() + split, first.size() - split, 14);
    expect(tfo.decision == DnsTcpDecision::MESSAGES_READY &&
               tfo.domains == std::vector<std::string>{"ads.example.com"},
           "SYN data must account for SYN consuming one sequence number");

    std::vector<uint8_t> invalid = {0xff, 0xff, 0, 0};
    TcpFlowKey malformed = flow_key(4);
    expect(reassembler.inspect(malformed, 4000, 0x18, invalid.data(),
                               invalid.size(), 7).decision ==
               DnsTcpDecision::NEED_MORE,
           "an out-of-order-looking prefix should get a bounded retry");
    reassembler.inspect(malformed, 4000, 0x18, invalid.data(), invalid.size(), 8);
    expect(reassembler.inspect(malformed, 4000, 0x18, invalid.data(),
                               invalid.size(), 9).decision ==
               DnsTcpDecision::GIVE_UP,
           "invalid length prefix must fail open after bounded retries");

    if (failures != 0) {
        std::cerr << failures << " TCP DNS reassembly test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "TCP DNS reassembly tests passed\n";
    return EXIT_SUCCESS;
}
