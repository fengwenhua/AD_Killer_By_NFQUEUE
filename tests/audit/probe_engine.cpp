// Actual hot_reload, rules, and reassembly; no real queues or firewall linked.
#include "../../src/engine.cpp"
#include "../../src/cgroup_isolation.h"
#include <cassert>

namespace {
std::string injected_failure;
int cleanup_calls = 0;
}

std::string iptables_install(const IptablesConfig &) { return injected_failure; }
void iptables_remove_all() { ++cleanup_calls; }
FastResponseSender::~FastResponseSender() {}

int main() {
    assert(write_file_atomic(FILE_STATE_CONF, "mode=2\nscope=0\nblock_quic=false\n"));
    assert(write_file_atomic(FILE_CUSTOM_RULES, "old.example.com\n"));
    auto old = std::make_shared<RuleDB>();
    load_all_rules(*old);
    assert(old->ok);
    publish_rules_snapshot(old);
    assert(match_domain("old.example.com") == Verdict::BLOCK_EXACT);
    assert(write_file_atomic(FILE_CUSTOM_RULES, "new.example.com\n"));
    injected_failure = "injected route installation failure";
    hot_reload();
    assert(match_domain("old.example.com") == Verdict::PASS_DEFAULT);
    assert(match_domain("new.example.com") == Verdict::BLOCK_EXACT);
    assert(!g_stop.load() && !g_fault.load());
    puts("OBSERVED mixed-generation: new rule snapshot published despite route installation failure");

    injected_failure = "injected route installation failure; rollback failed";
    hot_reload();
    assert(g_stop.load() && !g_fault.load() && cleanup_calls == 1);
    puts("OBSERVED fault-state: rollback failure sets stop but leaves g_fault false");

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

    const CgroupRead unavailable = [](const std::string &, std::string &) { return false; };
    assert(!frozen_cgroup_process(123, unavailable));
    const CgroupRead frozen = [](const std::string &path, std::string &out) {
        if (path == "/proc/123/cgroup") out = "0::/audit\n";
        else if (path == "/proc/123/mountinfo") out = "1 0 0:1 / /synthetic rw - cgroup2 cgroup rw\n";
        else if (path == "/synthetic/audit/cgroup.events") out = "populated 1\nfrozen 1\n";
        else return false;
        return true;
    };
    assert(frozen_cgroup_process(123, frozen));
    const CgroupRead missing_events = [&](const std::string &path, std::string &out) {
        return path != "/synthetic/audit/cgroup.events" && frozen(path, out);
    };
    assert(!frozen_cgroup_process(123, missing_events));
    puts("OBSERVED unknown-freezer: failed membership/events reads collapse to false, like not frozen");

    ParsedDomainRule rule;
    assert(parse_adguard_rule("||tracker.example.com^", rule));
    assert(!parse_adguard_rule("||badfilter.example.com^", rule));
    assert(!parse_adguard_rule("@@||badfilter.example.com^", rule));
    assert(parse_hosts_rule("badfilter.example.com", rule));
    puts("OBSERVED badfilter-substring: valid black/white domains containing badfilter are rejected by AdGuard parser");
}
