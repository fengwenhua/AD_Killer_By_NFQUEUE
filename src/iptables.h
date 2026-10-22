// ============================================================================
// AD_Killer_By_NFQUEUE - iptables/ip6tables 拦截规则管理
//
// OUTPUT 永远只跳到稳定入口 ADK_NFQ_ENTRY。配置更新时先在非活动的 A/B
// 代际中完整构建主链与 selector,IPv4/IPv6 都成功后再替换 ENTRY 第一条 jump。
// 任一协议族切换或校验失败都会恢复旧 jump,旧代际仅在双栈提交后删除。
//
//   OUTPUT → ADK_NFQ_ENTRY → ADK_NFQ_OUT_A/B (scope=0)
//                          └→ ADK_NFQ_APPS_A/B (scope=1)
//                                uid → 同代 ADK_NFQ_OUT_A/B
//
// 全部路由事务只持有一把 .route.lock；安装拿不到锁立即失败，停止清理等待锁。
// ============================================================================
#pragma once

#include <sys/types.h>
#include <cstdint>
#include <string>
#include <vector>

struct IptablesConfig {
    std::string chain = "ADK_NFQ_OUT";
    std::string apps_chain = "ADK_NFQ_APPS";
    bool block_quic = false;
    int scope = 0;
    std::vector<uid_t> bypass_uids;
    std::vector<uid_t> intercept_uids;
};

struct IptablesGeneration {
    std::string slot;
    std::string main_chain;
    std::string apps_chain;
    std::string target_chain;
};

// iptables -S 的只读快照。纯验证函数不执行系统命令,便于覆盖缺失/重复规则。
struct IptablesRulesetSnapshot {
    std::string target_chain;
    std::vector<std::string> output_lines;
    std::vector<std::string> entry_lines;
    std::vector<std::string> main_lines;
    std::vector<std::string> apps_lines;
};

// 纯规划函数:依据当前入口目标选择非活动的 A/B 代际。
IptablesGeneration iptables_plan_generation(const IptablesConfig &cfg,
                                             const std::string &active_target);

bool iptables_validate_ruleset(const IptablesConfig &cfg,
                               const IptablesRulesetSnapshot &snapshot,
                               std::string *error = nullptr);

std::vector<std::string> iptables_build_whitelist_commands(
    const char *table_binary, const IptablesConfig &cfg);

// iptables-save 快照中的外部连接标记读写不得使用本模块的位。
// 返回冲突规则，空串表示该快照未发现冲突；不涵盖 vendor/BPF 的隐式写入。
std::string iptables_connmark_conflict(const std::vector<std::string> &lines,
                                      uint32_t mask);

class FileLock {
public:
    explicit FileLock(const char *path, bool wait = false);
    ~FileLock();
    bool ok() const { return fd_ >= 0; }
private:
    int fd_ = -1;
};

std::string iptables_install(const IptablesConfig &cfg);
void iptables_remove_all();
bool iptables_verify(const IptablesConfig &cfg);
bool iptables_chain_active(const IptablesConfig &cfg);
// Read-only two-command check, explicitly not a full generation verification.
bool iptables_entry_attached();
bool iptables_validate_output_attachment(const std::vector<std::string> &lines);
bool iptables_any_left();
std::string iptables_sync_app_routes(const IptablesConfig &cfg);
int run_shell(const char *cmd);
