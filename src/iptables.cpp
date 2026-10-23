#include "iptables.h"
#include "logging.h"
#include "common.h"
#include "process_runner.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <cerrno>
#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <map>
#include <sstream>
#include <sys/file.h>
#include <unistd.h>

namespace {

constexpr const char *ENTRY_CHAIN = "ADK_NFQ_ENTRY";
constexpr const char *CHAIN_PREFIX = "ADK_NFQ_";
constexpr const char *TB[2] = {"iptables -w 5", "ip6tables -w 5"};
constexpr const char *TB_SAVE[2] = {"iptables-save", "ip6tables-save"};
using Clock = std::chrono::steady_clock;
thread_local auto command_deadline = Clock::time_point::max();
thread_local unsigned command_limit_ms = 6000;
thread_local int route_lease = -1;
thread_local bool uncertain_command = false;
struct RouteLease {
    int previous = route_lease;
    explicit RouteLease(int fd) { route_lease = fd; }
    ~RouteLease() { route_lease = previous; }
};
struct CommandBudget {
    Clock::time_point previous = command_deadline;
    unsigned previous_limit = command_limit_ms;
    bool previous_uncertain = uncertain_command;
    explicit CommandBudget(unsigned total_ms, unsigned single_ms = 6000) {
        command_deadline = std::min(previous, Clock::now() + std::chrono::milliseconds(total_ms));
        command_limit_ms = std::min(previous_limit, single_ms);
        if (previous == Clock::time_point::max()) uncertain_command = false;
    }
    ~CommandBudget() {
        uncertain_command = previous == Clock::time_point::max() ? previous_uncertain : (uncertain_command || previous_uncertain);
        command_deadline = previous; command_limit_ms = previous_limit;
    }
};
unsigned remaining_command_ms() {
    if (Clock::now() >= command_deadline) return 0;
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(command_deadline - Clock::now()).count();
    return static_cast<unsigned>(std::min<int64_t>(command_limit_ms, std::max<int64_t>(1, remaining)));
}
ProcessResult execute(const std::vector<std::string> &arguments) {
    if (uncertain_command) {
        ProcessResult result; result.error = "previous command group is not confirmed stopped";
        result.reaped = false; result.group_gone = false; return result;
    }
    auto result = run_process(arguments, remaining_command_ms(), 2u * 1024u * 1024u, route_lease);
    if (!result.reaped || !result.group_gone) uncertain_command = true;
    return result;
}
ProcessResult execute_shell(const char *command) {
    if (!command) { ProcessResult result; result.error = "empty command"; return result; }
#if defined(__ANDROID__)
    return execute({"/system/bin/sh", "-c", command});
#else
    return execute({"/bin/sh", "-c", command});
#endif
}

bool owned_chain_name(const std::string &name) {
    return name.compare(0, strlen(CHAIN_PREFIX), CHAIN_PREFIX) == 0;
}

}  // namespace

int run_shell(const char *cmd) {
    const auto result = execute_shell(cmd);
    return result.ok() ? 0 : result.exit_code > 0 ? result.exit_code : -1;
}

namespace {

static bool shell_ok(const char *fmt, ...) {
    char cmd[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    return run_shell(cmd) == 0;
}

static void remove_rule(const char *fmt, ...) {
    char cmd[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    for (int i = 0; i < 32; i++) {
        if (run_shell(cmd) != 0) break;
    }
}

static std::vector<std::string> shell_output_lines(const char *cmd,
                                                  bool *success = nullptr) {
    if (success) *success = false;
    std::vector<std::string> lines;
    const auto result = execute_shell(cmd);
    if (!result.ok() || result.output.find('\0') != std::string::npos) return lines;
    std::istringstream input(result.output);
    std::string s;
    while (std::getline(input, s)) {
        if (s.size() > 65536 || lines.size() >= 32768) return {};
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        lines.push_back(s);
    }
    if (success) *success = true;
    return lines;
}

static bool shell_output_contains(const char *cmd, const std::string &needle) {
    for (const auto &line : shell_output_lines(cmd)) {
        if (line.find(needle) != std::string::npos) return true;
    }
    return false;
}

static bool chain_exists(const char *tb, const std::string &chain) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -t mangle -S %s", tb, chain.c_str());
    return shell_output_contains(cmd, "-N " + chain);
}

static std::string entry_target(const char *tb) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -t mangle -S %s", tb, ENTRY_CHAIN);
    const std::string prefix = std::string("-A ") + ENTRY_CHAIN + " -j ";
    for (const auto &line : shell_output_lines(cmd)) {
        if (line.compare(0, prefix.size(), prefix) != 0) continue;
        std::string target = line.substr(prefix.size());
        if (target != "RETURN") return target;
    }
    return "";
}

static bool entry_layout_valid(const char *tb, const std::string &target) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "%s -t mangle -S %s", tb, ENTRY_CHAIN);
    std::vector<std::string> rules;
    for (const auto &line : shell_output_lines(cmd)) {
        if (line.compare(0, 3, "-A ") == 0) rules.push_back(line);
    }
    return rules.size() == 2 &&
           rules[0] == std::string("-A ") + ENTRY_CHAIN + " -j " + target &&
           rules[1] == std::string("-A ") + ENTRY_CHAIN + " -j RETURN";
}

static bool known_target(const IptablesConfig &cfg, const std::string &target) {
    return target == cfg.chain + "_A" || target == cfg.chain + "_B" ||
           target == cfg.apps_chain + "_A" || target == cfg.apps_chain + "_B";
}

static std::string main_chain_for_target(const IptablesConfig &cfg,
                                         const std::string &target) {
    if (target == cfg.chain + "_A" || target == cfg.apps_chain + "_A")
        return cfg.chain + "_A";
    if (target == cfg.chain + "_B" || target == cfg.apps_chain + "_B")
        return cfg.chain + "_B";
    return "";
}

static void delete_chain(const char *tb, const std::string &chain) {
    // -F 对空链仍返回成功,不能像 -D 一样循环到失败。
    shell_ok("%s -t mangle -F %s", tb, chain.c_str());
    shell_ok("%s -t mangle -X %s", tb, chain.c_str());
}

static void detach_output_target(const char *tb, const std::string &chain) {
    remove_rule("%s -t mangle -D OUTPUT -j %s", tb, chain.c_str());
}

static void delete_generation(const char *tb,
                              const IptablesGeneration &generation) {
    detach_output_target(tb, generation.apps_chain);
    detach_output_target(tb, generation.main_chain);
    delete_chain(tb, generation.apps_chain);
    delete_chain(tb, generation.main_chain);
}

static void remove_obsolete_rules(const char *tb) {
    const char *obsolete_chains[] = {
        "ADK_NFQ_APPS_NEXT", "ADK_NFQ_APPS_OLD", "ADK_NFQ_APPS", "ADK_NFQ_OUT"
    };
    for (const char *chain : obsolete_chains) detach_output_target(tb, chain);

    remove_rule("%s -t mangle -D OUTPUT -m owner --uid-owner 0 -j ADK_NFQ_OUT", tb);
    // Anonymous OUTPUT NFQUEUE/DROP rules cannot be attributed to this module.
    // Deleting them by port/queue alone can undo another firewall's policy.

    for (const char *chain : obsolete_chains) delete_chain(tb, chain);
}

static std::string apply_whitelist(const char *tb,
                                   const IptablesConfig &cfg) {
    for (const auto &cmd : iptables_build_whitelist_commands(tb, cfg)) {
        if (!shell_ok("%s", cmd.c_str()))
            return std::string(tb) + " 白名单规则插入失败";
    }
    return "";
}

static std::string build_generation(const char *tb,
                                    const IptablesConfig &source_cfg,
                                    const IptablesGeneration &generation) {
    IptablesConfig cfg = source_cfg;
    cfg.chain = generation.main_chain;
    cfg.apps_chain = generation.apps_chain;

    if (!shell_ok("%s -t mangle -N %s", tb, cfg.chain.c_str()))
        return std::string(tb) + " 建主链失败";

    if (!shell_ok("%s -t mangle -A %s -p udp --dport 53 -j NFQUEUE --queue-num 0 --queue-bypass",
                  tb, cfg.chain.c_str()))
        return std::string(tb) + " DNS(UDP) 规则失败";
    if (!shell_ok("%s -t mangle -A %s -p tcp --dport 53 -m connmark "
                  "--mark " BLOCK_CONNMARK_SPEC " -j DROP",
                  tb, cfg.chain.c_str()))
        return std::string(tb) + " DNS(TCP) 连接级 DROP 规则失败";
    if (!shell_ok("%s -t mangle -A %s -p tcp --dport 53 -j NFQUEUE --queue-num 0 --queue-bypass",
                  tb, cfg.chain.c_str()))
        return std::string(tb) + " DNS(TCP) 规则失败";

    for (const char *port : {"80", "443"}) {
        // 未分类连接持续进入 NFQUEUE；用户态完成一次有界判定后写 PASS/BLOCK
        // connmark。后续包分别直接 RETURN/DROP，不再依赖可被重传耗尽的包数窗口。
        if (!shell_ok("%s -t mangle -A %s -p tcp --dport %s -m connmark "
                      "--mark " BLOCK_CONNMARK_SPEC " -j DROP",
                      tb, cfg.chain.c_str(), port))
            return std::string(tb) + " 连接级 DROP 规则失败";
        if (!shell_ok("%s -t mangle -A %s -p tcp --dport %s -m connmark "
                      "--mark " PASS_CONNMARK_SPEC " -j RETURN",
                      tb, cfg.chain.c_str(), port))
            return std::string(tb) + " 已分类放行规则失败";
        if (!shell_ok("%s -t mangle -A %s -p tcp --dport %s "
                      "-j NFQUEUE --queue-num 1 --queue-bypass",
                      tb, cfg.chain.c_str(), port))
            return std::string(tb) + " 连接分类规则失败";
    }

    if (cfg.block_quic &&
        !shell_ok("%s -t mangle -A %s -p udp --dport 443 -j NFQUEUE --queue-num 1 --queue-bypass",
                  tb, cfg.chain.c_str()))
        return std::string(tb) + " QUIC 快速拒绝队列规则失败";

    if (!shell_ok("%s -t mangle -A %s -j RETURN", tb, cfg.chain.c_str()))
        return std::string(tb) + " 主链兜底规则失败";

    if (cfg.scope == SCOPE_GLOBAL) return apply_whitelist(tb, cfg);

    if (!shell_ok("%s -t mangle -N %s", tb, cfg.apps_chain.c_str()))
        return std::string(tb) + " 建 selector 链失败";
    for (uid_t uid : cfg.intercept_uids) {
        if (uid <= 0) continue;
        if (!shell_ok("%s -t mangle -A %s -m owner --uid-owner %d -j %s",
                      tb, cfg.apps_chain.c_str(), static_cast<int>(uid),
                      cfg.chain.c_str()))
            return std::string(tb) + " selector UID 规则失败";
    }
    if (!shell_ok("%s -t mangle -A %s -j RETURN", tb, cfg.apps_chain.c_str()))
        return std::string(tb) + " selector 兜底规则失败";
    return "";
}

struct TableTransaction {
    const char *tb = nullptr;
    bool entry_existed = false;
    bool output_had_entry = false;
    bool entry_created = false;
    bool output_inserted = false;
    bool target_replaced = false;
    std::string old_target;
    IptablesGeneration next;
};

static bool rollback_switch(TableTransaction &tx) {
    bool ok = true;
    if (tx.entry_created) {
        detach_output_target(tx.tb, ENTRY_CHAIN);
        delete_chain(tx.tb, ENTRY_CHAIN);
        tx.output_inserted = false;
        tx.entry_created = false;
        return true;
    }
    if (tx.target_replaced &&
        !shell_ok("%s -t mangle -R %s 1 -j %s", tx.tb, ENTRY_CHAIN,
                  tx.old_target.c_str()))
        ok = false;
    if (tx.output_inserted) {
        if (!shell_ok("%s -t mangle -D OUTPUT -j %s", tx.tb, ENTRY_CHAIN)) ok = false;
        tx.output_inserted = false;
    }
    tx.target_replaced = false;
    return ok;
}

static std::string switch_generation(TableTransaction &tx) {
    if (!tx.entry_existed) {
        if (!shell_ok("%s -t mangle -N %s", tx.tb, ENTRY_CHAIN))
            return std::string(tx.tb) + " 建入口链失败";
        tx.entry_created = true;
        if (!shell_ok("%s -t mangle -A %s -j %s", tx.tb, ENTRY_CHAIN,
                      tx.next.target_chain.c_str()) ||
            !shell_ok("%s -t mangle -A %s -j RETURN", tx.tb, ENTRY_CHAIN)) {
            rollback_switch(tx);
            return std::string(tx.tb) + " 填充入口链失败";
        }
    } else {
        if (!shell_ok("%s -t mangle -R %s 1 -j %s", tx.tb, ENTRY_CHAIN,
                      tx.next.target_chain.c_str()))
            return std::string(tx.tb) + " 切换入口链失败";
        tx.target_replaced = true;
    }

    if (!tx.output_had_entry) {
        if (!shell_ok("%s -t mangle -I OUTPUT 1 -j %s", tx.tb, ENTRY_CHAIN)) {
            rollback_switch(tx);
            return std::string(tx.tb) + " 挂载入口链失败";
        }
        tx.output_inserted = true;
    }
    return "";
}

static IptablesRulesetSnapshot read_ruleset(const char *tb,
                                             const IptablesConfig &cfg,
                                             const std::string &target) {
    IptablesRulesetSnapshot snapshot;
    snapshot.target_chain = target;
    snapshot.output_lines = shell_output_lines(
        (std::string(tb) + " -t mangle -S OUTPUT").c_str());
    snapshot.entry_lines = shell_output_lines(
        (std::string(tb) + " -t mangle -S " + ENTRY_CHAIN).c_str());
    const std::string main_chain = main_chain_for_target(cfg, target);
    if (!main_chain.empty()) {
        snapshot.main_lines = shell_output_lines(
            (std::string(tb) + " -t mangle -S " + main_chain).c_str());
    }
    if (target == cfg.apps_chain + "_A" || target == cfg.apps_chain + "_B") {
        snapshot.apps_lines = shell_output_lines(
            (std::string(tb) + " -t mangle -S " + target).c_str());
    }
    return snapshot;
}

static bool verify_target(const char *tb, const IptablesConfig &cfg,
                          const std::string &target,
                          bool allow_migration_jumps = false) {
    IptablesRulesetSnapshot snapshot = read_ruleset(tb, cfg, target);
    if (allow_migration_jumps) {
        const std::string entry_jump = std::string("-A OUTPUT -j ") + ENTRY_CHAIN;
        std::vector<std::string> filtered;
        for (const auto &line : snapshot.output_lines) {
            if (line != entry_jump &&
                line.find("-j ADK_NFQ_") != std::string::npos)
                continue;
            filtered.push_back(line);
        }
        snapshot.output_lines.swap(filtered);
    }
    std::string error;
    if (!iptables_validate_ruleset(cfg, snapshot, &error)) {
        log_line("错误", "%s 路由校验失败: %s", tb, error.c_str());
        return false;
    }
    return true;
}

}  // namespace

IptablesGeneration iptables_plan_generation(const IptablesConfig &cfg,
                                             const std::string &active_target) {
    IptablesGeneration generation;
    bool active_a = active_target == cfg.chain + "_A" ||
                    active_target == cfg.apps_chain + "_A";
    generation.slot = active_a ? "B" : "A";
    generation.main_chain = cfg.chain + "_" + generation.slot;
    generation.apps_chain = cfg.apps_chain + "_" + generation.slot;
    generation.target_chain = cfg.scope == SCOPE_PER_APP
                                  ? generation.apps_chain
                                  : generation.main_chain;
    return generation;
}

namespace {

static std::vector<std::string> split_tokens(const std::string &line) {
    std::istringstream input(line);
    std::vector<std::string> tokens;
    std::string token;
    while (input >> token) tokens.push_back(token);
    return tokens;
}

static bool has_token(const std::vector<std::string> &tokens,
                      const std::string &token) {
    for (const auto &candidate : tokens) {
        if (candidate == token) return true;
    }
    return false;
}

static bool has_pair(const std::vector<std::string> &tokens,
                     const std::string &key, const std::string &value) {
    for (size_t i = 0; i + 1 < tokens.size(); i++) {
        if (tokens[i] == key && tokens[i + 1] == value) return true;
    }
    return false;
}

static bool chain_declared(const std::vector<std::string> &lines,
                           const std::string &chain) {
    const std::string expected = "-N " + chain;
    for (const auto &line : lines) {
        if (line == expected) return true;
    }
    return false;
}

static std::vector<std::string> chain_rules(const std::vector<std::string> &lines,
                                            const std::string &chain) {
    std::vector<std::string> rules;
    const std::string prefix = "-A " + chain + " ";
    for (const auto &line : lines) {
        if (line.compare(0, prefix.size(), prefix) == 0) rules.push_back(line);
    }
    return rules;
}

static void set_validation_error(std::string *error, const std::string &message) {
    if (error) *error = message;
}

static std::map<std::string, int> expected_uid_counts(
        const std::vector<uid_t> &uids) {
    std::map<std::string, int> counts;
    for (uid_t uid : uids) {
        if (uid > 0) counts[std::to_string(static_cast<int>(uid))]++;
    }
    return counts;
}

}  // namespace

bool iptables_validate_ruleset(const IptablesConfig &cfg,
                               const IptablesRulesetSnapshot &snapshot,
                               std::string *error) {
    if (cfg.scope != SCOPE_GLOBAL && cfg.scope != SCOPE_PER_APP) {
        set_validation_error(error, "scope 非法");
        return false;
    }

    const bool slot_a = snapshot.target_chain == cfg.chain + "_A" ||
                        snapshot.target_chain == cfg.apps_chain + "_A";
    const bool slot_b = snapshot.target_chain == cfg.chain + "_B" ||
                        snapshot.target_chain == cfg.apps_chain + "_B";
    if (!slot_a && !slot_b) {
        set_validation_error(error, "ENTRY 目标不是已知代际");
        return false;
    }
    const std::string slot = slot_a ? "A" : "B";
    const std::string main_chain = cfg.chain + "_" + slot;
    const std::string apps_chain = cfg.apps_chain + "_" + slot;
    const std::string expected_target = cfg.scope == SCOPE_PER_APP
                                            ? apps_chain : main_chain;
    if (snapshot.target_chain != expected_target) {
        set_validation_error(error, "ENTRY 目标与 scope 不一致");
        return false;
    }

    int output_jumps = 0;
    const std::string output_jump = std::string("-A OUTPUT -j ") + ENTRY_CHAIN;
    for (const auto &line : snapshot.output_lines) {
        if (line == output_jump) output_jumps++;
        else if (line.find("-A OUTPUT ") == 0 &&
                 line.find("-j ADK_NFQ_") != std::string::npos) {
            set_validation_error(error, "OUTPUT 残留绕过 ENTRY 的旧版跳转");
            return false;
        }
    }
    if (output_jumps != 1) {
        set_validation_error(error, "OUTPUT 必须且只能挂载一次 ENTRY");
        return false;
    }

    if (!chain_declared(snapshot.entry_lines, ENTRY_CHAIN)) {
        set_validation_error(error, "ENTRY 链不存在");
        return false;
    }
    const auto entry_rules = chain_rules(snapshot.entry_lines, ENTRY_CHAIN);
    if (entry_rules.size() != 2 ||
        entry_rules[0] != std::string("-A ") + ENTRY_CHAIN + " -j " + expected_target ||
        entry_rules[1] != std::string("-A ") + ENTRY_CHAIN + " -j RETURN") {
        set_validation_error(error, "ENTRY 跳转或兜底规则不完整");
        return false;
    }

    if (!chain_declared(snapshot.main_lines, main_chain)) {
        set_validation_error(error, "活动主链不存在");
        return false;
    }
    const auto main_rules = chain_rules(snapshot.main_lines, main_chain);
    std::map<std::string, int> remaining_whitelist = expected_uid_counts(
        cfg.scope == SCOPE_GLOBAL ? cfg.bypass_uids : std::vector<uid_t>{});
    int dns_udp = 0;
    int dns_tcp = 0;
    int dns_tcp_marked_drop = 0;
    int queue_80 = 0;
    int queue_443 = 0;
    int marked_drop_80 = 0;
    int marked_drop_443 = 0;
    int marked_pass_80 = 0;
    int marked_pass_443 = 0;
    int quic_drop = 0;
    int main_return = 0;
    for (const auto &line : main_rules) {
        const auto tokens = split_tokens(line);
        const bool is_udp = has_pair(tokens, "-p", "udp");
        const bool is_tcp = has_pair(tokens, "-p", "tcp");
        const bool is_nfqueue = has_pair(tokens, "-j", "NFQUEUE");
        const bool bypass = has_token(tokens, "--queue-bypass");
        if (is_udp && has_pair(tokens, "--dport", "53") && is_nfqueue && bypass &&
            has_pair(tokens, "--queue-num", "0")) {
            dns_udp++;
        } else if (is_tcp && has_pair(tokens, "--dport", "53") &&
                   has_pair(tokens, "-m", "connmark") &&
                   has_pair(tokens, "--mark", BLOCK_CONNMARK_SPEC) &&
                   has_pair(tokens, "-j", "DROP")) {
            dns_tcp_marked_drop++;
        } else if (is_tcp && has_pair(tokens, "--dport", "53") && is_nfqueue && bypass &&
                   has_pair(tokens, "--queue-num", "0")) {
            if (dns_tcp_marked_drop != 1) {
                set_validation_error(
                    error, "DNS TCP connmark DROP 必须位于 queue 之前");
                return false;
            }
            dns_tcp++;
        } else if (is_tcp && has_pair(tokens, "--dport", "80") &&
                   has_pair(tokens, "-m", "connmark") &&
                   has_pair(tokens, "--mark", BLOCK_CONNMARK_SPEC) &&
                   has_pair(tokens, "-j", "DROP")) {
            marked_drop_80++;
        } else if (is_tcp && has_pair(tokens, "--dport", "443") &&
                   has_pair(tokens, "-m", "connmark") &&
                   has_pair(tokens, "--mark", BLOCK_CONNMARK_SPEC) &&
                   has_pair(tokens, "-j", "DROP")) {
            marked_drop_443++;
        } else if (is_tcp && has_pair(tokens, "--dport", "80") &&
                   has_pair(tokens, "-m", "connmark") &&
                   has_pair(tokens, "--mark", PASS_CONNMARK_SPEC) &&
                   has_pair(tokens, "-j", "RETURN")) {
            marked_pass_80++;
        } else if (is_tcp && has_pair(tokens, "--dport", "443") &&
                   has_pair(tokens, "-m", "connmark") &&
                   has_pair(tokens, "--mark", PASS_CONNMARK_SPEC) &&
                   has_pair(tokens, "-j", "RETURN")) {
            marked_pass_443++;
        } else if (is_tcp && has_pair(tokens, "--dport", "80") && is_nfqueue && bypass &&
                   has_pair(tokens, "--queue-num", "1") &&
                   !has_pair(tokens, "-m", "connbytes") &&
                   !has_token(tokens, "--connbytes")) {
            if (marked_drop_80 != 1 || marked_pass_80 != 1) {
                set_validation_error(
                    error, "TCP 80 connmark DROP/RETURN 必须位于 queue 之前");
                return false;
            }
            queue_80++;
        } else if (is_tcp && has_pair(tokens, "--dport", "443") && is_nfqueue && bypass &&
                   has_pair(tokens, "--queue-num", "1") &&
                   !has_pair(tokens, "-m", "connbytes") &&
                   !has_token(tokens, "--connbytes")) {
            if (marked_drop_443 != 1 || marked_pass_443 != 1) {
                set_validation_error(
                    error, "TCP 443 connmark DROP/RETURN 必须位于 queue 之前");
                return false;
            }
            queue_443++;
        } else if (is_udp && has_pair(tokens, "--dport", "443") &&
                   is_nfqueue && bypass && has_pair(tokens, "--queue-num", "1")) {
            quic_drop++;
        } else if (line == "-A " + main_chain + " -j RETURN") {
            main_return++;
        } else if (has_pair(tokens, "-m", "owner") &&
                   has_pair(tokens, "-j", "RETURN")) {
            std::string uid;
            for (size_t i = 0; i + 1 < tokens.size(); i++) {
                if (tokens[i] == "--uid-owner") uid = tokens[i + 1];
            }
            auto found = remaining_whitelist.find(uid);
            if (found == remaining_whitelist.end() || found->second == 0) {
                set_validation_error(error, "主链包含未配置的 UID 放行规则");
                return false;
            }
            found->second--;
        } else {
            set_validation_error(error, "主链包含未知或不完整规则");
            return false;
        }
    }
    for (const auto &item : remaining_whitelist) {
        if (item.second != 0) {
            set_validation_error(error, "主链缺少 UID 放行规则");
            return false;
        }
    }
    if (dns_udp != 1 || dns_tcp != 1 || dns_tcp_marked_drop != 1 ||
        marked_drop_80 != 1 || marked_drop_443 != 1 ||
        marked_pass_80 != 1 || marked_pass_443 != 1 ||
        queue_80 != 1 || queue_443 != 1 ||
        main_return != 1 || quic_drop != (cfg.block_quic ? 1 : 0)) {
        set_validation_error(error,
                             "connmark DROP/RETURN、queue 0/1、QUIC 或主链兜底规则数量不正确");
        return false;
    }

    if (cfg.scope == SCOPE_PER_APP) {
        if (!chain_declared(snapshot.apps_lines, apps_chain)) {
            set_validation_error(error, "活动 selector 链不存在");
            return false;
        }
        std::map<std::string, int> remaining_intercepts =
            expected_uid_counts(cfg.intercept_uids);
        int apps_return = 0;
        for (const auto &line : chain_rules(snapshot.apps_lines, apps_chain)) {
            if (line == "-A " + apps_chain + " -j RETURN") {
                apps_return++;
                continue;
            }
            const auto tokens = split_tokens(line);
            if (!has_pair(tokens, "-m", "owner") ||
                !has_pair(tokens, "-j", main_chain)) {
                set_validation_error(error, "selector 包含未知规则或跨代跳转");
                return false;
            }
            std::string uid;
            for (size_t i = 0; i + 1 < tokens.size(); i++) {
                if (tokens[i] == "--uid-owner") uid = tokens[i + 1];
            }
            auto found = remaining_intercepts.find(uid);
            if (found == remaining_intercepts.end() || found->second == 0) {
                set_validation_error(error, "selector 包含未配置的 UID");
                return false;
            }
            found->second--;
        }
        for (const auto &item : remaining_intercepts) {
            if (item.second != 0) {
                set_validation_error(error, "selector 缺少拦截 UID");
                return false;
            }
        }
        if (apps_return != 1) {
            set_validation_error(error, "selector 兜底 RETURN 数量不正确");
            return false;
        }
    }

    if (error) error->clear();
    return true;
}

std::string iptables_connmark_conflict(const std::vector<std::string> &lines,
                                      uint32_t mask) {
    auto saved_tokens = [](const std::string &line) {
        std::vector<std::string> tokens;
        std::string token;
        bool quoted = false, escaped = false;
        for (char ch : line) {
            if (escaped) {
                token += ch;
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                quoted = !quoted;
            } else if (!quoted && (ch == ' ' || ch == '\t')) {
                if (!token.empty()) {
                    tokens.push_back(std::move(token));
                    token.clear();
                }
            } else {
                token += ch;
            }
        }
        if (!token.empty()) tokens.push_back(std::move(token));
        return tokens;
    };
    auto number = [](const std::string &text, uint32_t &value) {
        if (text.empty() || text.front() == '-') return false;
        char *end = nullptr;
        errno = 0;
        unsigned long long parsed = strtoull(text.c_str(), &end, 0);
        if (errno != 0 || !end || *end || parsed > UINT32_MAX) return false;
        value = static_cast<uint32_t>(parsed);
        return true;
    };
    auto mark_mask = [&](const std::string &text) {
        const size_t slash = text.find('/');
        uint32_t value = 0, bits = UINT32_MAX;
        if (!number(text.substr(0, slash), value)) return UINT32_MAX;
        if (slash != std::string::npos && !number(text.substr(slash + 1), bits))
            return UINT32_MAX;
        return bits;
    };
    for (const auto &line : lines) {
        const auto tokens = saved_tokens(line);
        if (tokens.size() < 3 || tokens[0] != "-A" ||
            owned_chain_name(tokens[1])) continue;
        uint32_t touched = 0;
        if (has_pair(tokens, "-m", "connmark")) {
            uint32_t matched = UINT32_MAX;
            for (size_t i = 0; i + 1 < tokens.size(); ++i)
                if (tokens[i] == "--mark") matched = mark_mask(tokens[i + 1]);
            touched |= matched;
        }
        if (has_pair(tokens, "-j", "CONNMARK")) {
            uint32_t changed = UINT32_MAX;
            for (size_t i = 0; i + 1 < tokens.size(); ++i) {
                const auto &option = tokens[i];
                if (option == "--set-mark" || option == "--set-xmark") {
                    const std::string &spec = tokens[i + 1];
                    uint32_t value = 0;
                    changed = mark_mask(spec);
                    if (number(spec.substr(0, spec.find('/')), value))
                        changed |= value;  // set-xmark XORs value after clearing mask
                }
                else if (option == "--and-mark" || option == "--or-mark" ||
                         option == "--xor-mark") {
                    uint32_t operand = 0;
                    if (!number(tokens[i + 1], operand)) changed = UINT32_MAX;
                    else changed = option == "--and-mark" ? ~operand : operand;
                }
            }
            // save writes ctmark; restore reads it back into skb->mark. Both
            // directions can change system behavior when the bits overlap.
            if (has_token(tokens, "--save-mark") ||
                has_token(tokens, "--restore-mark")) {
                changed = UINT32_MAX;  // documented default ctmask
                for (size_t i = 0; i + 1 < tokens.size(); ++i) {
                    if (tokens[i] == "--ctmask" &&
                        !number(tokens[i + 1], changed)) changed = UINT32_MAX;
                }
                if (has_token(tokens, "--save-mark")) {
                    uint32_t nfmask = UINT32_MAX;
                    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
                        if (tokens[i] == "--nfmask" &&
                            !number(tokens[i + 1], nfmask)) nfmask = UINT32_MAX;
                    }
                    changed |= nfmask;  // save: (ctmark & ~ctmask) ^ (nfmark & nfmask)
                }
            }
            touched |= changed;
        }
        if ((touched & mask) != 0) return line;
    }
    return "";
}

std::vector<std::string> iptables_build_whitelist_commands(
        const char *table_binary, const IptablesConfig &cfg) {
    std::vector<std::string> commands;
    if (!table_binary) return commands;
    for (auto it = cfg.bypass_uids.rbegin(); it != cfg.bypass_uids.rend(); ++it) {
        if (*it <= 0) continue;
        char cmd[512];
        snprintf(cmd, sizeof(cmd),
                 "%s -t mangle -I %s 1 -m owner --uid-owner %d -j RETURN",
                 table_binary, cfg.chain.c_str(), static_cast<int>(*it));
        commands.emplace_back(cmd);
    }
    return commands;
}

namespace {
constexpr const char *CLEANUP_BINARIES[] = {"iptables", "ip6tables"};
const std::vector<std::string> CLEANUP_CHAINS = {"ADK_NFQ_ENTRY", "ADK_NFQ_APPS_A", "ADK_NFQ_APPS_B",
    "ADK_NFQ_OUT_A", "ADK_NFQ_OUT_B", "ADK_NFQ_APPS_NEXT", "ADK_NFQ_APPS_OLD", "ADK_NFQ_APPS", "ADK_NFQ_OUT"};
bool cleanup_chain(const std::string &name) {
    return std::find(CLEANUP_CHAINS.begin(), CLEANUP_CHAINS.end(), name) != CLEANUP_CHAINS.end();
}
bool saved_arguments(const std::string &line, std::vector<std::string> &arguments) {
    if (line.size() > 65536) return false;
    std::string value;
    char quote = 0;
    bool started = false, escaped = false;
    for (unsigned char ch : line) {
        if ((ch < 32 && ch != '\t') || ch == 127) return false;
        if (escaped) { value += ch; escaped = false; started = true; }
        else if (ch == '\\' && quote != '\'') { escaped = true; started = true; }
        else if (quote) { if (ch == quote) quote = 0; else value += ch; }
        else if (ch == '\'' || ch == '"') { quote = static_cast<char>(ch); started = true; }
        else if (ch == ' ' || ch == '\t') {
            if (started) { arguments.push_back(value); value.clear(); started = false; }
        } else { value += ch; started = true; }
        if (arguments.size() > 128) return false;
    }
    if (escaped || quote) return false;
    if (started) arguments.push_back(value);
    return arguments.size() <= 128;
}
std::string command_error(const ProcessResult &result) {
    std::string message = result.error.empty() ? "exit " + std::to_string(result.exit_code) : result.error;
    if (!result.diagnostic.empty()) message += ": " + result.diagnostic.substr(0, 256);
    for (char &ch : message) if (ch == '\r' || ch == '\n') ch = ' ';
    return message;
}
struct OwnedInventory {
    bool valid = false;
    bool capped = false;
    std::vector<std::string> chains, remaining;
    std::vector<std::vector<std::string>> detach;
    std::string error;
};
OwnedInventory owned_inventory(const char *binary) {
    OwnedInventory out;
    const auto query = execute({binary, "-w", "5", "-t", "mangle", "-S"});
    if (!query.ok()) { out.error = command_error(query); return out; }
    bool saw_output = false;
    size_t count = 0;
    std::istringstream input(query.output);
    std::string line;
    while (std::getline(input, line)) {
        if (++count > 32768) { out.error = "ruleset line limit exceeded"; return out; }
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> tokens;
        if (!saved_arguments(line, tokens)) { out.error = "cannot parse ruleset safely"; return out; }
        if (tokens.empty()) continue;
        if (tokens[0] == "-P" && tokens.size() == 3) { saw_output = saw_output || tokens[1] == "OUTPUT"; continue; }
        if (tokens[0] == "-N" && tokens.size() == 2) {
            if (owned_chain_name(tokens[1])) out.remaining.push_back(std::string(binary) + ":chain:" + tokens[1]);
            if (cleanup_chain(tokens[1])) out.chains.push_back(tokens[1]);
            continue;
        }
        if (tokens[0] != "-A" || tokens.size() < 3) { out.error = "incomplete ruleset output"; return out; }
        if (tokens[1] != "OUTPUT") continue;
        for (size_t i = 2; i + 1 < tokens.size(); ++i) {
            if (tokens[i] != "-j" && tokens[i] != "--jump" && tokens[i] != "-g" && tokens[i] != "--goto") continue;
            if (!owned_chain_name(tokens[i + 1])) continue;
            out.remaining.push_back(std::string(binary) + ":OUTPUT->" + tokens[i + 1]);
            if (cleanup_chain(tokens[i + 1])) {
                std::vector<std::string> command{binary, "-w", "5", "-t", "mangle", "-D", "OUTPUT"};
                command.insert(command.end(), tokens.begin() + 2, tokens.end());
                if (out.detach.size() < 256) out.detach.push_back(std::move(command));
                else out.capped = true;
            }
            break;
        }
    }
    if (!saw_output) { out.error = "missing OUTPUT policy; empty output is not proof of cleanup"; return out; }
    out.valid = true; return out;
}
IptablesCleanupResult inspect_owned() {
    IptablesCleanupResult result;
    result.state = FirewallCleanupState::CLEAN;
    for (const char *binary : CLEANUP_BINARIES) {
        const auto inventory = owned_inventory(binary);
        if (!inventory.valid) {
            result.state = FirewallCleanupState::UNKNOWN;
            result.errors.push_back(std::string(binary) + " -t mangle -S: " + inventory.error);
        } else {
            result.remaining.insert(result.remaining.end(), inventory.remaining.begin(), inventory.remaining.end());
            if (!inventory.remaining.empty() && result.state != FirewallCleanupState::UNKNOWN) result.state = FirewallCleanupState::PARTIAL;
        }
    }
    return result;
}
}

IptablesCleanupResult iptables_inspect_owned() {
    CommandBudget budget(8000);
    return inspect_owned();
}

IptablesCleanupResult iptables_remove_all(const IptablesCleanupLimits &limits) {
    CommandBudget budget(limits.total_ms, limits.command_ms);
    IptablesCleanupResult result;
    if (!remaining_command_ms()) { result.errors.push_back("cleanup deadline expired"); return result; }
    FileLock lock(DATA_DIR "/.route.lock", limits.lock_ms != 0, std::min(limits.lock_ms, remaining_command_ms()));
    if (!lock.ok()) { result.errors.push_back("route lock unavailable; cleanup was not executed"); return result; }
    RouteLease lease(lock.native_fd());
    bool termination_unknown = false, mutated = false;
    unsigned operations = 0;
    const auto attempt = [&](const std::vector<std::string> &command) {
        if (termination_unknown) return;
        if (++operations > 256) {
            if (result.errors.size() < 16) result.errors.push_back("cleanup command limit reached");
            return;
        }
        mutated = true;
        const auto execution = execute(command);
        termination_unknown = termination_unknown || !execution.reaped || !execution.group_gone;
        if (!execution.ok() && result.errors.size() < 16) {
            // Fixed chain names, not pasted rules/comments, make this safe to log.
            result.errors.push_back(command[0] + " " + command[5] + " " + command[6] + ": " + command_error(execution));
        }
    };
    bool initially_known = true;
    for (const char *binary : CLEANUP_BINARIES) {
        const auto inventory = owned_inventory(binary);
        if (!inventory.valid) {
            initially_known = false;
            result.errors.push_back(std::string(binary) + " -t mangle -S: " + inventory.error);
            continue; // Never mutate a family that we could not inventory.
        }
        result.remaining.insert(result.remaining.end(), inventory.remaining.begin(), inventory.remaining.end());
        if (inventory.capped) result.errors.push_back(std::string(binary) + ": too many owned OUTPUT references; cleanup is bounded");
        for (const auto &command : inventory.detach) attempt(command);
        // Flush every known owned chain before deleting any of them; ENTRY and
        // selectors can reference the main chains. Foreign chains/rules stay.
        for (const auto &chain : inventory.chains) attempt({binary, "-w", "5", "-t", "mangle", "-F", chain});
        for (const auto &chain : inventory.chains) attempt({binary, "-w", "5", "-t", "mangle", "-X", chain});
    }
    if (mutated) {
        const auto final = inspect_owned();
        result.state = termination_unknown ? FirewallCleanupState::UNKNOWN : final.state;
        result.remaining = final.remaining;
        result.errors.insert(result.errors.end(), final.errors.begin(), final.errors.end());
        if (termination_unknown) result.errors.push_back("command termination not confirmed; do not assume no future side effects");
    } else result.state = initially_known ? (result.remaining.empty() ? FirewallCleanupState::CLEAN : FirewallCleanupState::PARTIAL) : FirewallCleanupState::UNKNOWN;
    log_line(result.ok() ? "系统" : "错误", "模块路由清理结果: %s (剩余 %zu，诊断 %zu)", result.name(), result.remaining.size(), result.errors.size());
    return result;
}

std::string iptables_install(const IptablesConfig &cfg) {
    CommandBudget budget(60000);
    if (cfg.scope != SCOPE_GLOBAL && cfg.scope != SCOPE_PER_APP)
        return "invalid scope";

    FileLock lock(DATA_DIR "/.route.lock");
    if (!lock.ok()) return "route lock busy";
    RouteLease lease(lock.native_fd());

    for (const char *save : TB_SAVE) {
        bool read_ok = false;
        const auto lines = shell_output_lines(save, &read_ok);
        if (!read_ok) return std::string(save) + " 无法检查连接标记冲突,拒绝接管流量";
        const std::string conflict = iptables_connmark_conflict(
            lines, BLOCK_CONNMARK_MASK);
        if (!conflict.empty())
            return std::string(save) + " 连接标记位冲突: " + conflict;
    }

    TableTransaction tx[2];
    for (int i = 0; i < 2; i++) {
        tx[i].tb = TB[i];
        tx[i].entry_existed = chain_exists(TB[i], ENTRY_CHAIN);
        tx[i].old_target = entry_target(TB[i]);
        tx[i].output_had_entry = shell_ok("%s -t mangle -C OUTPUT -j %s",
                                          TB[i], ENTRY_CHAIN);

        if (tx[i].entry_existed &&
            (!known_target(cfg, tx[i].old_target) ||
             !entry_layout_valid(TB[i], tx[i].old_target)))
            return std::string(TB[i]) + " 入口链目标异常,拒绝覆盖";

        tx[i].next = iptables_plan_generation(cfg, tx[i].old_target);
    }

    for (int i = 0; i < 2; i++) {
        delete_generation(TB[i], tx[i].next);
        std::string err = build_generation(TB[i], cfg, tx[i].next);
        if (!err.empty()) {
            for (int j = 0; j <= i; j++) delete_generation(TB[j], tx[j].next);
            return err;
        }
    }

    for (int i = 0; i < 2; i++) {
        std::string err = switch_generation(tx[i]);
        if (!err.empty()) {
            bool rollback_ok = true;
            for (int j = i; j >= 0; j--) rollback_ok = rollback_switch(tx[j]) && rollback_ok;
            for (int j = 0; j < 2; j++) delete_generation(TB[j], tx[j].next);
            return err + (rollback_ok ? "" : "; rollback failed");
        }
    }

    bool verified = true;
    for (int i = 0; i < 2; i++) {
        verified = verify_target(TB[i], cfg, tx[i].next.target_chain, true) && verified;
    }
    if (!verified) {
        bool rollback_ok = true;
        for (int i = 1; i >= 0; i--) rollback_ok = rollback_switch(tx[i]) && rollback_ok;
        for (int i = 0; i < 2; i++) delete_generation(TB[i], tx[i].next);
        return std::string("mangle 新代际校验失败") +
               (rollback_ok ? "" : "; rollback failed");
    }

    for (int i = 0; i < 2; i++) {
        IptablesGeneration old_or_stale = iptables_plan_generation(
            cfg, tx[i].next.target_chain);
        delete_generation(TB[i], old_or_stale);
        remove_obsolete_rules(TB[i]);
    }
    return "";
}

std::string iptables_sync_app_routes(const IptablesConfig &cfg) {
    return cfg.scope == SCOPE_PER_APP ? iptables_install(cfg) : "";
}

bool iptables_verify(const IptablesConfig &cfg) {
    CommandBudget budget(8000);
    for (const char *tb : TB) {
        std::string target = entry_target(tb);
        if (!known_target(cfg, target) || !verify_target(tb, cfg, target)) return false;
    }
    return true;
}

bool iptables_chain_active(const IptablesConfig &cfg) {
    return iptables_verify(cfg);
}

bool iptables_validate_output_attachment(const std::vector<std::string> &lines) {
    size_t attached = 0;
    for (const auto &line : lines) {
        if (line == std::string("-A OUTPUT -j ") + ENTRY_CHAIN) ++attached;
        else if (line.find("-A OUTPUT ") == 0 && line.find("-j ADK_NFQ_") != std::string::npos)
            return false;
    }
    return attached == 1;
}

bool iptables_entry_attached() {
    CommandBudget budget(8000);
    for (const char *tb : TB) {
        bool ok = false;
        auto lines = shell_output_lines((std::string(tb) + " -t mangle -S OUTPUT").c_str(), &ok);
        if (!ok || !iptables_validate_output_attachment(lines)) return false;
    }
    return true;
}

bool iptables_any_left() {
    // A failed query is not evidence of absence.
    return !iptables_inspect_owned().ok();
}
