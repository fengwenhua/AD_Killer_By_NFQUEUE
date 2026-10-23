// Real reload control flow with only route execution and socket destruction
// isolated; no root, packets, cgroup migration or host firewall operations.
#include "../src/engine.cpp"
#include <cassert>

int cleanup_calls = 0;
std::string iptables_install(const IptablesConfig &) { return "injected switch failure; rollback failed"; }
IptablesCleanupResult iptables_remove_all(const IptablesCleanupLimits &) {
    ++cleanup_calls; return {FirewallCleanupState::CLEAN, {}, {}};
}
FastResponseSender::~FastResponseSender() {}

int main() {
    assert(write_file_atomic(FILE_STATE_CONF, "mode=2\nscope=0\nblock_quic=false\n"));
    assert(write_file_atomic(FILE_CUSTOM_RULES, "ads.example.com\n"));
    g_fault.store(false); g_stop.store(false);
    hot_reload();
    assert(g_fault.load() && g_stop.load());
    assert(cleanup_calls == 0); // Owner must release listeners before cleanup.
    assert(read_conf_value(FILE_ENGINE_ERROR, "code") == "reload_rollback_failed");
    puts("reload rollback failure preserves fault and defers cleanup to listener-safe shutdown");
}
