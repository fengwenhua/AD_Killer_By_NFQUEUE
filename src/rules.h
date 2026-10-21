// ============================================================================
// AD_Killer_By_NFQUEUE - 规则引擎:Hosts / AdGuard / 自定义规则解析与匹配
//
// 规则语法与匹配语义:
//   - 存储:排序后的 FNV-1a 64 位哈希数组(精确匹配)+ 后缀数组(||domain 形态)
//   - AdGuard 域名级语法:@@(白名单)、||(后缀)、*. (通配域名)、末尾 ^
//   - URL/路径、正则、请求级 $ 修饰符、cosmetic/scriptlet 和 badfilter 均跳过
//   - DNS 查询日志四态判定文案:PASS(whitelist) / BLOCK(suffix) /
//     BLOCK(exact) / PASS(default)
// ============================================================================
#pragma once

#include "common.h"

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

// 一类规则的存储:域名不保留原文,只存紧凑的 FNV-1a 64 位哈希数组。
//   - exact_:精确匹配哈希(裸域名 / AdGuard 非 || 形态)
//   - suffix_:后缀匹配哈希(||domain 形态,匹配自身及任意子域名)
class RuleSet {
public:
    void clear();
    bool empty() const { return exact_.empty() && suffix_.empty(); }
    size_t size() const;
    size_t storage_bytes() const;

    void add_exact(const std::string &domain);     // 精确规则(裸域名)
    void add_suffix(const std::string &domain);    // 后缀规则(||domain / *.domain)
    void merge_from(const RuleSet &other);         // 成功读完整文件后再合并
    void finalize() const;                         // 排序去重，发布前调用

    // 匹配:先精确,再逐级后缀(a.b.com → b.com → com)
    bool match(const std::string &domain) const;

    // 判定形态(用于四态日志文案)
    enum class Hit { NONE, EXACT, SUFFIX };
    Hit classify(const std::string &domain) const;

    int bad_lines = 0;   // 解析失败的行数(统计用)

private:
    static uint64_t fnv1a64(const std::string &s);
    static void compact(std::vector<uint64_t> &values);
    mutable std::vector<uint64_t> exact_;
    mutable std::vector<uint64_t> suffix_;   // 存去掉前置 '.' 后的域名哈希
    mutable bool finalized_ = true;
};

// 完整规则库(可热重载)
struct RuleDB {
    RuleSet black;         // 黑名单(Hosts + AdGuard 黑 + 自定义黑)
    RuleSet white;         // 白名单(@@ 例外)
    long loaded_at = 0;    // 加载时间戳
    int  mode = MODE_HOSTS_ADGUARD;   // 1=仅Hosts 2=Hosts+AdGuard
    bool ok = false;       // 是否加载到至少一条规则
    bool load_valid = true; // 有效空分类与文件损坏必须区分
    std::string load_error;

    void finalize() const { black.finalize(); white.finalize(); }
};

// 单行解析(返回是否成功加入规则)
struct ParsedDomainRule {
    std::string domain;
    bool white = false;
    bool suffix = false;
    bool invalid_domain = false;
    std::string canonical() const {
        return std::string(white ? "@@" : "") + (suffix ? "||" : "") +
               domain + (suffix ? "^" : "");
    }
};
bool parse_adguard_rule(const std::string &line, ParsedDomainRule &rule);
bool parse_hosts_rule(const std::string &line, ParsedDomainRule &rule);
// Strict Hosts-source adapter for the Python recipe; outputs AdGuard exact.
bool parse_recipe_hosts_rule(const std::string &line, ParsedDomainRule &rule);
int parse_hosts_line(const std::string &line, RuleDB &db);
int parse_adguard_line(const std::string &line, RuleDB &db);

// 从文件加载(gzip 自动解压):adguard=true 按 AdGuard 语法,否则 Hosts 语法
int load_rule_file(const char *path, bool adguard, RuleDB &db);

// 严格加载结果供 staging 校验：文件必须可读、格式/解压完整；require_gzip
// 用于更新产物，允许“有效但 0 条”的单个分类文件。
bool load_rule_file_checked(const char *path, bool adguard, RuleDB &db,
                            int *loaded = nullptr, bool require_gzip = false);

// 加载全部规则(hosts_rules.gz + adguard_rules.gz + custom_rules.txt)
// 结果写入 rule_stats.conf(WebUI 读取的 black=/white= 来源)
void load_all_rules(RuleDB &db);
// Explicit paths for isolated tests/benchmarks. Absent legacy files are optional;
// published bundles require both selected gzip files to exist and be intact.
void load_rule_files(RuleDB &db, int mode, const std::string &hosts,
                     const std::string &adguard, const std::string &custom,
                     bool require_bundle_files = false);
void save_rule_stats(const RuleDB &db);

// 当前不可变规则快照。读线程获取 shared_ptr 后可无锁完成一次匹配；发布新快照时，
// 已经在用旧快照的线程继续安全持有旧对象，最后一个读者退出后自动释放。
using RuleSnapshot = std::shared_ptr<const RuleDB>;
RuleSnapshot rules_snapshot();
void publish_rules_snapshot(RuleSnapshot snapshot);

// 匹配结果(四态,用于日志)
enum class Verdict { PASS_WHITELIST, BLOCK_SUFFIX, BLOCK_EXACT, PASS_DEFAULT };

// 匹配(原子获取不可变快照，无全局互斥锁):返回四态判定
Verdict match_domain(const std::string &domain);

// Verdict → 四态日志文案
const char *verdict_text(Verdict v);
