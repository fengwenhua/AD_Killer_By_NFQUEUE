// Real reload control flow with only route execution and socket destruction
// isolated; no root, packets, cgroup migration or host firewall operations.
#include "../src/engine.cpp"
#include <cassert>

int cleanup_calls = 0;
int installs = 0;
std::string failure;
std::string iptables_install(const IptablesConfig &cfg) {
    ++installs;
    assert(cfg.scope == 0 && !cfg.block_quic);
    // Actual production reload must not expose new domains before route
    // commit. Workers must bypass rather than classify during the switch.
    assert(match_domain("old.example.com") == Verdict::BLOCK_EXACT);
    assert(match_domain("new.example.com") == Verdict::PASS_DEFAULT);
    PolicyBarrier::Read packet(g_policy);
    assert(!packet.active());
    return failure;
}
IptablesCleanupResult iptables_remove_all(const IptablesCleanupLimits &) {
    ++cleanup_calls; return {FirewallCleanupState::CLEAN, {}, {}};
}
FastResponseSender::~FastResponseSender() {}

int main() {
    g_owner = {1234, "12345678-1234-1234-1234-123456789abc", 100};
    assert(write_file_atomic(FILE_STATE_CONF, "mode=2\nscope=0\nblock_quic=false\n"));
    EngineConfig checked; std::string error;
    assert(load_engine_config_checked(checked, error)); // absent legacy empty lists
    assert(write_file_atomic(FILE_STATE_CONF, "mode=2\nmode=1\n"));
    assert(!load_engine_config_checked(checked, error));
    assert(write_file_atomic(FILE_STATE_CONF, "mode=2\nblock_quic=broken\n"));
    assert(!load_engine_config_checked(checked, error));
    assert(write_file_atomic(FILE_STATE_CONF, "mode=2\nscope=0\nblock_quic=false\n"));
    assert(write_file_atomic(FILE_APP_WHITELIST, "com.example.app\n"));
    assert(!load_engine_config_checked(checked, error)); // missing cache is not an empty selection
    assert(write_file_atomic(DATA_FILE("app_uids.conf").c_str(), "4294967296 com.example.app\n"));
    assert(!load_engine_config_checked(checked, error));
    const auto list = edit_config::read(DATA_DIR, "allow"); assert(list.status == "ok");
    assert(write_file_atomic(DATA_FILE("app_uids.conf").c_str(), "# packages_revision=" + list.revision + "\n10001 com.example.app\n"));
    assert(load_engine_config_checked(checked, error) && checked.bypass_uids == std::vector<uid_t>{10001});
    assert(write_file_atomic(FILE_APP_WHITELIST, "com.example.other\n"));
    assert(!load_engine_config_checked(checked, error));
    assert(unlink(FILE_APP_WHITELIST) == 0 && unlink(DATA_FILE("app_uids.conf").c_str()) == 0);
    assert(write_file_atomic(FILE_CUSTOM_RULES, "old.example.com\n"));
    CandidateInputs captured;
    assert(capture_candidate_inputs(captured, error));
    { ConfigInputLock inputs(DATA_DIR); assert(inputs.ok()); } // no expensive build under config locks
    assert(write_file_atomic(FILE_CUSTOM_RULES, "changed-after-capture.example.com\n"));
    RuleDB pinned;
    load_rule_files(pinned, captured.config.mode, captured.bundle.hosts_path, captured.bundle.adguard_path,
                    "", captured.bundle.from_manifest, &captured.custom);
    assert(pinned.black.classify("old.example.com") == RuleSet::Hit::EXACT);
    assert(pinned.black.classify("changed-after-capture.example.com") == RuleSet::Hit::NONE);
    assert(write_file_atomic(FILE_CUSTOM_RULES, "old.example.com\n"));
    auto old = std::make_shared<RuleDB>();
    load_all_rules(*old); publish_rules_snapshot(old); save_rule_stats(*old);
    const auto old_stats = [&] { std::string text; assert(read_file(FILE_RULE_STATS, text)); return text; }();
    assert(write_file_atomic(FILE_CUSTOM_RULES, "new.example.com\nsecond.example.com\n"));
    failure = "injected switch failure; rollback succeeded";
    assert(!hot_reload());
    assert(!g_fault.load() && !g_stop.load() && installs == 1);
    assert(rules_snapshot() == old);
    std::string stats; assert(read_file(FILE_RULE_STATS, stats) && stats == old_stats);
    { PolicyBarrier::Read packet(g_policy); assert(packet.active()); }

    // Corrupt generation must not invoke the route installer at all.
    assert(write_file_atomic(FILE_RULESET_CURRENT, "corrupt manifest\n"));
    assert(!hot_reload() && installs == 1 && rules_snapshot() == old);
    assert(unlink(FILE_RULESET_CURRENT) == 0);

    assert(write_file_atomic(FILE_CUSTOM_RULES, std::string(1, '\xff')));
    assert(!hot_reload() && installs == 1 && rules_snapshot() == old);
    assert(write_file_atomic(FILE_CUSTOM_RULES, "new.example.com\nsecond.example.com\n"));

    // The mode, custom list and routing cache locks are shared with writers.
    { ExclusiveFileLock lock(STATE_LOCK_FILE); assert(lock.ok());
      assert(!hot_reload() && installs == 1 && rules_snapshot() == old); }

    failure.clear();
    assert(hot_reload() && installs == 2);
    assert(match_domain("new.example.com") == Verdict::BLOCK_EXACT);
    assert(match_domain("old.example.com") == Verdict::PASS_DEFAULT);
    assert(read_conf_value(FILE_RULE_STATS, "black") == "2");
    { PolicyBarrier::Read packet(g_policy); assert(packet.active()); }

    publish_rules_snapshot(old);
    failure = "injected switch failure; rollback failed";
    g_fault.store(false); g_stop.store(false);
    assert(!hot_reload());
    assert(g_fault.load() && g_stop.load());
    assert(cleanup_calls == 0); // Owner must release listeners before cleanup.
    assert(read_conf_value(FILE_ENGINE_ERROR, "code") == "reload_rollback_failed");
    assert(rules_snapshot() == old);
    { PolicyBarrier::Read packet(g_policy); assert(!packet.active()); }
    puts("reload retains old policy/stats on failure, fences cutover, and preserves rollback faults");
}
