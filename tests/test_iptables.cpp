#include "iptables.h"
#include "common.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

bool starts_with(const std::string &value, const std::string &prefix) {
    return value.compare(0, prefix.size(), prefix) == 0;
}

IptablesRulesetSnapshot valid_snapshot(const IptablesConfig &cfg,
                                       const std::string &slot) {
    IptablesRulesetSnapshot snapshot;
    const std::string main_chain = cfg.chain + "_" + slot;
    const std::string apps_chain = cfg.apps_chain + "_" + slot;
    snapshot.target_chain = cfg.scope == SCOPE_PER_APP ? apps_chain : main_chain;
    snapshot.output_lines = {
        "-P OUTPUT ACCEPT",
        "-A OUTPUT -j ADK_NFQ_ENTRY",
    };
    snapshot.entry_lines = {
        "-N ADK_NFQ_ENTRY",
        "-A ADK_NFQ_ENTRY -j " + snapshot.target_chain,
        "-A ADK_NFQ_ENTRY -j RETURN",
    };
    snapshot.main_lines = {"-N " + main_chain};
    if (cfg.scope == SCOPE_GLOBAL) {
        for (uid_t uid : cfg.bypass_uids) {
            if (uid > 0) {
                snapshot.main_lines.push_back(
                    "-A " + main_chain + " -m owner --uid-owner " +
                    std::to_string(static_cast<int>(uid)) + " -j RETURN");
            }
        }
    }
    snapshot.main_lines.push_back(
        "-A " + main_chain + " -p udp -m udp --dport 53 -j NFQUEUE "
        "--queue-num 0 --queue-bypass");
    snapshot.main_lines.push_back(
        "-A " + main_chain + " -p tcp -m tcp --dport 53 -m connmark "
        "--mark " BLOCK_CONNMARK_SPEC " -j DROP");
    snapshot.main_lines.push_back(
        "-A " + main_chain + " -p tcp -m tcp --dport 53 -j NFQUEUE "
        "--queue-num 0 --queue-bypass");
    snapshot.main_lines.push_back(
        "-A " + main_chain + " -p tcp -m tcp --dport 80 -m connmark "
        "--mark " BLOCK_CONNMARK_SPEC " -j DROP");
    snapshot.main_lines.push_back(
        "-A " + main_chain + " -p tcp -m tcp --dport 80 -m connmark "
        "--mark " PASS_CONNMARK_SPEC " -j RETURN");
    snapshot.main_lines.push_back(
        "-A " + main_chain + " -p tcp -m tcp --dport 80 "
        "-j NFQUEUE --queue-num 1 --queue-bypass");
    snapshot.main_lines.push_back(
        "-A " + main_chain + " -p tcp -m tcp --dport 443 -m connmark "
        "--mark " BLOCK_CONNMARK_SPEC " -j DROP");
    snapshot.main_lines.push_back(
        "-A " + main_chain + " -p tcp -m tcp --dport 443 -m connmark "
        "--mark " PASS_CONNMARK_SPEC " -j RETURN");
    snapshot.main_lines.push_back(
        "-A " + main_chain + " -p tcp -m tcp --dport 443 "
        "-j NFQUEUE --queue-num 1 --queue-bypass");
    if (cfg.block_quic) {
        snapshot.main_lines.push_back(
            "-A " + main_chain + " -p udp -m udp --dport 443 -j NFQUEUE --queue-num 1 --queue-bypass");
    }
    snapshot.main_lines.push_back("-A " + main_chain + " -j RETURN");

    if (cfg.scope == SCOPE_PER_APP) {
        snapshot.apps_lines.push_back("-N " + apps_chain);
        for (uid_t uid : cfg.intercept_uids) {
            if (uid > 0) {
                snapshot.apps_lines.push_back(
                    "-A " + apps_chain + " -m owner --uid-owner " +
                    std::to_string(static_cast<int>(uid)) + " -j " + main_chain);
            }
        }
        snapshot.apps_lines.push_back("-A " + apps_chain + " -j RETURN");
    }
    return snapshot;
}

}  // namespace

int main() {
    expect(!iptables_validate_output_attachment({"-P OUTPUT ACCEPT"}), "missing OUTPUT attachment is unhealthy");
    expect(iptables_validate_output_attachment({"-P OUTPUT ACCEPT", "-A OUTPUT -j ADK_NFQ_ENTRY", "-A OUTPUT -j connmark_mangle_OUTPUT"}), "one owned attachment with foreign rules is valid");
    expect(!iptables_validate_output_attachment({"-A OUTPUT -j ADK_NFQ_ENTRY", "-A OUTPUT -j ADK_NFQ_ENTRY"}), "duplicate attachment is unhealthy");
    expect(!iptables_validate_output_attachment({"-A OUTPUT -j ADK_NFQ_ENTRY", "-A OUTPUT -m owner --uid-owner 1000 -j ADK_NFQ_OUT_A"}), "legacy bypass is unhealthy");
    const std::vector<std::string> android_rules = {
        "-A connmark_mangle_OUTPUT -m connmark --mark 0x0/0xfffff -j CONNMARK "
        "--save-mark --nfmask 0xfffff --ctmask 0xfffff",
        "-A st_clear_detect -m connmark --mark 0x2000000/0x2000000 -j REJECT",
        "-A st_penalty_log -j CONNMARK --set-xmark 0x1000000/0x1000000",
        "-A st_penalty_reject -j CONNMARK --set-xmark 0x2000000/0x2000000",
        "-A bw_INPUT -j MARK --set-xmark 0x100000/0x100000",
    };
    expect(iptables_connmark_conflict(android_rules, BLOCK_CONNMARK_MASK).empty(),
           "classification bits must not overlap observed Android connmark rules");
    expect(!iptables_connmark_conflict(android_rules, 0xffff0000u).empty(),
           "the old high-16-bit mask must detect an Android network mark conflict");
    for (const auto &rule : {
            "-A VENDOR -j CONNMARK --set-xmark 0x4000000/0x4000000",
            "-A VENDOR -j CONNMARK --set-xmark 0x4000000/0xfffff",
            "-A VENDOR -j CONNMARK --save-mark",
            "-A VENDOR -j CONNMARK --save-mark --ctmask 0xfffff",
            "-A VENDOR -j CONNMARK --restore-mark",
            "-A VENDOR -j CONNMARK --or-mark 0x8000000",
            "-A VENDOR -j CONNMARK --and-mark 0x03ffffff",
            "-A VENDOR -m connmark --mark 0x0/0xfc000000 -j RETURN",
            "-A VENDOR -m connmark --mark 0 -j RETURN",
            "-A VENDOR -j CONNMARK --set-xmark garbage"}) {
        expect(!iptables_connmark_conflict({rule}, BLOCK_CONNMARK_MASK).empty(),
               "external overlapping or unparseable connmark rule must fail closed");
    }
    expect(iptables_connmark_conflict({
        "-A VENDOR -j CONNMARK --restore-mark --ctmask 0xfffff",
        "-A VENDOR -j CONNMARK --and-mark 0xfcffffff",
        "-A VENDOR -j MARK --set-xmark 0x0/0xffffffff",
        "-A VENDOR -m comment --comment \"text -j CONNMARK --save-mark\" -j RETURN",
        "-A ADK_NFQ_OUT_A -m connmark --mark " BLOCK_CONNMARK_SPEC " -j DROP",
    }, BLOCK_CONNMARK_MASK).empty(),
           "disjoint ctmark bits, skb marks and module-owned rules should be allowed");

    IptablesConfig cfg;
    cfg.bypass_uids = {10001, 0, 10002};

    std::vector<std::string> ipv4 =
        iptables_build_whitelist_commands("iptables", cfg);
    std::vector<std::string> ipv6 =
        iptables_build_whitelist_commands("ip6tables", cfg);

    expect(ipv4.size() == 2, "IPv4 should contain one command per positive UID");
    expect(ipv6.size() == 2, "IPv6 should contain one command per positive UID");
    expect(starts_with(ipv4[0], "iptables ") && starts_with(ipv4[1], "iptables "),
           "IPv4 phase must only emit iptables commands");
    expect(starts_with(ipv6[0], "ip6tables ") && starts_with(ipv6[1], "ip6tables "),
           "IPv6 phase must only emit ip6tables commands");
    expect(ipv4[0].find("--uid-owner 10002") != std::string::npos &&
           ipv4[1].find("--uid-owner 10001") != std::string::npos,
           "reverse insertion should preserve configured UID order in the chain");

    cfg.bypass_uids.clear();
    expect(iptables_build_whitelist_commands("iptables", cfg).empty(),
           "empty whitelist should emit no commands");
    expect(iptables_build_whitelist_commands(nullptr, cfg).empty(),
           "missing table binary should emit no commands");

    cfg.scope = SCOPE_GLOBAL;
    IptablesGeneration first = iptables_plan_generation(cfg, "");
    expect(first.slot == "A" && first.main_chain == "ADK_NFQ_OUT_A" &&
               first.apps_chain == "ADK_NFQ_APPS_A" &&
               first.target_chain == "ADK_NFQ_OUT_A",
           "first global install should target generation A main chain");

    IptablesGeneration after_a =
        iptables_plan_generation(cfg, "ADK_NFQ_OUT_A");
    expect(after_a.slot == "B" && after_a.target_chain == "ADK_NFQ_OUT_B",
           "global reload should build the generation opposite active A");

    cfg.scope = SCOPE_PER_APP;
    IptablesGeneration apps_after_a =
        iptables_plan_generation(cfg, "ADK_NFQ_APPS_A");
    expect(apps_after_a.slot == "B" &&
               apps_after_a.main_chain == "ADK_NFQ_OUT_B" &&
               apps_after_a.target_chain == "ADK_NFQ_APPS_B",
           "per-app reload should keep selector and main chain in generation B");

    IptablesGeneration scope_change =
        iptables_plan_generation(cfg, "ADK_NFQ_OUT_B");
    expect(scope_change.slot == "A" &&
               scope_change.target_chain == "ADK_NFQ_APPS_A",
           "scope change should use the inactive slot and new selector target");

    char lock_path[] = "/tmp/ad_killer_by_nfqueue_route_lock.XXXXXX";
    int lock_fd = mkstemp(lock_path);
    expect(lock_fd >= 0, "route lock test file should be created");
    if (lock_fd >= 0) {
        close(lock_fd);
        {
            FileLock first(lock_path);
            FileLock competing(lock_path);
            expect(first.ok(), "first route transaction should acquire the lock");
            expect(!competing.ok(),
                   "competing non-blocking route transaction must fail closed");
        }
        unlink(lock_path);
    }

    std::string validation_error;
    cfg.scope = SCOPE_GLOBAL;
    cfg.block_quic = true;
    cfg.bypass_uids = {10001, 0, 10002};
    IptablesRulesetSnapshot global = valid_snapshot(cfg, "A");
    expect(iptables_validate_ruleset(cfg, global, &validation_error),
           "complete global ruleset should validate");
    expect(validation_error.empty(), "successful validation should clear the error");

    IptablesRulesetSnapshot missing_output = global;
    missing_output.output_lines.pop_back();
    expect(!iptables_validate_ruleset(cfg, missing_output, &validation_error),
           "ruleset without OUTPUT to ENTRY reachability must fail");

    IptablesRulesetSnapshot legacy_jump = global;
    legacy_jump.output_lines.push_back("-A OUTPUT -j ADK_NFQ_OUT");
    expect(!iptables_validate_ruleset(cfg, legacy_jump, &validation_error),
           "legacy direct jump bypassing ENTRY must fail status validation");
    IptablesRulesetSnapshot missing_queue = global;
    for (auto it = missing_queue.main_lines.begin();
         it != missing_queue.main_lines.end(); ++it) {
        if (it->find("--dport 443") != std::string::npos &&
            it->find("-j NFQUEUE") != std::string::npos) {
            missing_queue.main_lines.erase(it);
            break;
        }
    }
    expect(!iptables_validate_ruleset(cfg, missing_queue, &validation_error),
           "ruleset missing a queue 1 port must fail");

    IptablesRulesetSnapshot missing_dns_mark_drop = global;
    for (auto it = missing_dns_mark_drop.main_lines.begin();
         it != missing_dns_mark_drop.main_lines.end(); ++it) {
        if (it->find("--dport 53") != std::string::npos &&
            it->find("-m connmark") != std::string::npos) {
            missing_dns_mark_drop.main_lines.erase(it);
            break;
        }
    }
    expect(!iptables_validate_ruleset(cfg, missing_dns_mark_drop,
                                      &validation_error),
           "TCP DNS ruleset missing persistent BLOCK mark must fail");

    IptablesRulesetSnapshot missing_mark_drop = global;
    for (auto it = missing_mark_drop.main_lines.begin();
         it != missing_mark_drop.main_lines.end(); ++it) {
        if (it->find("--dport 80") != std::string::npos &&
            it->find("-m connmark") != std::string::npos) {
            missing_mark_drop.main_lines.erase(it);
            break;
        }
    }
    expect(!iptables_validate_ruleset(cfg, missing_mark_drop, &validation_error),
           "ruleset missing persistent connmark DROP must fail");

    IptablesRulesetSnapshot missing_pass_mark = global;
    for (auto it = missing_pass_mark.main_lines.begin();
         it != missing_pass_mark.main_lines.end(); ++it) {
        if (it->find("--dport 443") != std::string::npos &&
            it->find("--mark " PASS_CONNMARK_SPEC) != std::string::npos) {
            missing_pass_mark.main_lines.erase(it);
            break;
        }
    }
    expect(!iptables_validate_ruleset(cfg, missing_pass_mark, &validation_error),
           "ruleset missing classified PASS connmark RETURN must fail");

    IptablesRulesetSnapshot late_mark_drop = global;
    size_t mark_index = late_mark_drop.main_lines.size();
    size_t queue_index = late_mark_drop.main_lines.size();
    for (size_t i = 0; i < late_mark_drop.main_lines.size(); ++i) {
        const std::string &line = late_mark_drop.main_lines[i];
        if (line.find("--dport 80") != std::string::npos &&
            line.find("--mark " BLOCK_CONNMARK_SPEC) != std::string::npos)
            mark_index = i;
        if (line.find("--dport 80") != std::string::npos &&
            line.find("-j NFQUEUE") != std::string::npos)
            queue_index = i;
    }
    if (mark_index < late_mark_drop.main_lines.size() &&
        queue_index < late_mark_drop.main_lines.size())
        std::swap(late_mark_drop.main_lines[mark_index],
                  late_mark_drop.main_lines[queue_index]);
    expect(!iptables_validate_ruleset(cfg, late_mark_drop, &validation_error),
           "persistent connmark DROP after NFQUEUE must fail");

    IptablesRulesetSnapshot stale_window = global;
    for (auto &line : stale_window.main_lines) {
        if (line.find("--dport 443") != std::string::npos &&
            line.find("-j NFQUEUE") != std::string::npos) {
            line.insert(line.find("-j NFQUEUE"),
                        "-m connbytes --connbytes 1:12 "
                        "--connbytes-dir original --connbytes-mode packets ");
            break;
        }
    }
    expect(!iptables_validate_ruleset(cfg, stale_window, &validation_error),
           "finite connbytes window must not be accepted for unclassified flows");

    IptablesRulesetSnapshot quic_mismatch = global;
    IptablesConfig quic_off = cfg;
    quic_off.block_quic = false;
    expect(!iptables_validate_ruleset(quic_off, quic_mismatch, &validation_error),
           "unexpected QUIC DROP while disabled must fail");

    cfg.scope = SCOPE_PER_APP;
    cfg.block_quic = false;
    cfg.bypass_uids.clear();
    cfg.intercept_uids = {10101, 10102};
    IptablesRulesetSnapshot per_app = valid_snapshot(cfg, "B");
    expect(iptables_validate_ruleset(cfg, per_app, &validation_error),
           "complete per-app ruleset should validate");

    IptablesRulesetSnapshot cross_generation = per_app;
    cross_generation.apps_lines[1].replace(
        cross_generation.apps_lines[1].find("ADK_NFQ_OUT_B"),
        std::string("ADK_NFQ_OUT_B").size(), "ADK_NFQ_OUT_A");
    expect(!iptables_validate_ruleset(cfg, cross_generation, &validation_error),
           "selector must not jump across generations");

    IptablesRulesetSnapshot wrong_scope = per_app;
    wrong_scope.target_chain = "ADK_NFQ_OUT_B";
    wrong_scope.entry_lines[1] = "-A ADK_NFQ_ENTRY -j ADK_NFQ_OUT_B";
    expect(!iptables_validate_ruleset(cfg, wrong_scope, &validation_error),
           "ENTRY target inconsistent with scope must fail");

    if (failures != 0) {
        std::cerr << failures << " iptables test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "iptables tests passed\n";
    return EXIT_SUCCESS;
}
