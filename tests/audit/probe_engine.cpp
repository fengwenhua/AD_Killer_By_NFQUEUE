// Actual hot_reload, rules, and reassembly; no real queues or firewall linked.
#include "../../src/engine.cpp"
#include "../../src/cgroup_isolation.h"
#include <cassert>

namespace {
std::string injected_failure;
int cleanup_calls = 0;
}

std::string iptables_install(const IptablesConfig &) { return injected_failure; }
IptablesCleanupResult iptables_remove_all(const IptablesCleanupLimits &) {
    ++cleanup_calls; return {FirewallCleanupState::CLEAN, {}, {}};
}
FastResponseSender::~FastResponseSender() {}

int main() {
    HandshakeReassembler reassembler;
    TcpFlowKey key{};
    key.ip_version = 4; key.src_port = 12345; key.dst_port = 80;
    const std::string first = "GET / HTTP/1.1\r\nHost: incomplete";
    reassembler.inspect(key, 100, 0x18,
                        reinterpret_cast<const uint8_t *>(first.data()), first.size(), 1);
    const auto retained = reassembler.stats();
    assert(retained.flows == 1 && retained.bytes > 0);
    // stats() has no clock/pruning; no packet or explicit prune means retention.
    assert(reassembler.stats().flows == 1);
    reassembler.prune(20000);
    assert(reassembler.stats().flows == 0);
    puts("OBSERVED idle-retention: memory releases on explicit prune, not on stats reads");

}
