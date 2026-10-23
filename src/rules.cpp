#include "rules.h"
#include "logging.h"
#include "common.h"
#include "rule_bundle.h"
#include "rule_stream.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <cerrno>
#include <sys/stat.h>

// ============================================================================
// FNV-1a 64 位哈希(小写归一)
uint64_t RuleSet::fnv1a64(const std::string &s) {
    uint64_t h = 0xcbf29ce484222325ULL;   // FNV offset basis
    for (unsigned char c : s) {
        if (c >= 'A' && c <= 'Z') c += 32;   // 统一小写
        h ^= c;
        h *= 0x100000001b3ULL;               // FNV prime
    }
    return h;
}

void RuleSet::clear() {
    exact_.clear();
    suffix_.clear();
    finalized_ = true;
    bad_lines = 0;
}

void RuleSet::add_exact(const std::string &domain) {
    if (domain.empty()) return;
    exact_.push_back(fnv1a64(domain));
    finalized_ = false;
}

void RuleSet::add_suffix(const std::string &domain) {
    if (domain.empty()) return;
    std::string d = domain;
    // 归一:去首尾点(*.example.com. → example.com)
    if (!d.empty() && d[0] == '.') d.erase(0, 1);
    if (!d.empty() && d.back() == '.') d.pop_back();
    suffix_.push_back(fnv1a64(d));
    finalized_ = false;
}

void RuleSet::merge_from(const RuleSet &other) {
    exact_.insert(exact_.end(), other.exact_.begin(), other.exact_.end());
    suffix_.insert(suffix_.end(), other.suffix_.begin(), other.suffix_.end());
    finalized_ = false;
    bad_lines += other.bad_lines;
}

void RuleSet::compact(std::vector<uint64_t> &values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    values.shrink_to_fit();
}

void RuleSet::finalize() const {
    if (finalized_) return;
    compact(exact_);
    compact(suffix_);
    finalized_ = true;
}

size_t RuleSet::size() const {
    finalize();
    return exact_.size() + suffix_.size();
}

size_t RuleSet::storage_bytes() const {
    finalize();
    return (exact_.capacity() + suffix_.capacity()) * sizeof(uint64_t);
}

// 逐级后缀匹配:tracker.ads.example.com
//   → ads.example.com → example.com → com
bool RuleSet::match(const std::string &domain) const {
    return classify(domain) != Hit::NONE;
}

RuleSet::Hit RuleSet::classify(const std::string &domain) const {
    if (domain.empty()) return Hit::NONE;
    finalize();
    // 精确命中
    if (std::binary_search(exact_.begin(), exact_.end(), fnv1a64(domain)))
        return Hit::EXACT;
    // suffix 规则同时匹配规则域名自身及其子域名。
    if (std::binary_search(suffix_.begin(), suffix_.end(), fnv1a64(domain)))
        return Hit::SUFFIX;
    // 逐级检查父域名后缀。
    size_t pos = 0;
    while ((pos = domain.find('.', pos)) != std::string::npos) {
        pos++;
        if (pos >= domain.size()) break;
        if (std::binary_search(suffix_.begin(), suffix_.end(),
                               fnv1a64(domain.substr(pos)))) return Hit::SUFFIX;
    }
    return Hit::NONE;
}

// ============================================================================
// 工具函数
static bool is_ascii_alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

// 域名合法性(至少两级标签,总长 ≤ 253)
static bool valid_domain(const std::string &d) {
    // A trailing dot in an Adblock hostname mask is a prefix pattern, not a
    // complete domain. Never trim it and accidentally broaden its meaning.
    if (!d.empty() && d.back() == '.') return false;
    if (d.empty() || d.size() > 253) return false;
    if (d.find('.') == std::string::npos) return false;
    size_t label_start = 0;
    while (label_start < d.size()) {
        size_t dot = d.find('.', label_start);
        size_t label_end = dot == std::string::npos ? d.size() : dot;
        size_t label_len = label_end - label_start;
        if (label_len == 0 || label_len > 63) return false;
        if (!is_ascii_alnum(d[label_start]) || !is_ascii_alnum(d[label_end - 1]))
            return false;
        for (size_t i = label_start + 1; i + 1 < label_end; i++) {
            if (!is_ascii_alnum(d[i]) && d[i] != '-') return false;
        }
        if (dot == std::string::npos) break;
        label_start = dot + 1;
    }
    return true;
}

// ============================================================================
// Hosts 行解析:
//   "0.0.0.0 domain" / "127.0.0.1 domain" / 纯 domain(裸域名清单,如 oisd)
bool parse_hosts_rule(const std::string &raw, ParsedDomainRule &rule) {
    rule = {};
    std::string line = raw;
    // 去注释
    size_t h = line.find('#');
    if (h != std::string::npos) line = line.substr(0, h);
    // trim
    size_t b = line.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return 0;
    size_t e = line.find_last_not_of(" \t\r\n");
    line = line.substr(b, e - b + 1);
    if (line.empty()) return 0;

    std::string domain;
    // 形如 "0.0.0.0 xxx"(IP 打头取第二列)
    size_t sp = line.find_first_of(" \t");
    if (sp != std::string::npos) {
        std::string first = line.substr(0, sp);
        if (!first.empty() && (isdigit((unsigned char)first[0]) || first == "::")) {
            size_t dstart = line.find_first_not_of(" \t", sp);
            if (dstart == std::string::npos) return 0;
            std::string rest = line.substr(dstart);
            size_t dend = rest.find_first_of(" \t");
            domain = (dend == std::string::npos ? rest : rest.substr(0, dend));
            for (char &c : domain) if (c >= 'A' && c <= 'Z') c += 32;
        } else {
            return 0;   // 非 hosts 形态,交给 AdGuard 解析
        }
    } else {
        domain = line;
        for (char &c : domain) if (c >= 'A' && c <= 'Z') c += 32;
    }
    if (!valid_domain(domain)) { rule.invalid_domain = true; return false; }
    rule = {domain, false, false};
    return true;
}

int parse_hosts_line(const std::string &raw, RuleDB &db) {
    ParsedDomainRule rule;
    if (!parse_hosts_rule(raw, rule)) {
        if (rule.invalid_domain) db.black.bad_lines++;
        return 0;
    }
    db.black.add_exact(rule.domain);
    return 1;
}

bool parse_recipe_hosts_rule(const std::string &raw, ParsedDomainRule &rule) {
    size_t begin = raw.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return false;
    std::string line = raw.substr(begin, raw.find_last_not_of(" \t\r\n") - begin + 1);
    if (line.size() > 1024 || line.find_first_of("!#;") == 0) return false;
    if (line.compare(0, 2, "*.") == 0) line.erase(0, 2);
    else if (line.find_first_of(" \t") != std::string::npos) {
        std::istringstream input(line);
        std::string ip, domain;
        input >> ip >> domain;
        if ((ip != "0.0.0.0" && ip != "127.0.0.1") || domain.empty()) return false;
        line = domain;
    }
    for (char &c : line) if (c >= 'A' && c <= 'Z') c += 32;
    // Accept an absolute FQDN only in the explicitly typed Hosts input.
    if (!line.empty() && line.back() == '.') line.pop_back();
    if (!valid_domain(line)) return false;
    rule = {line, false, false};
    return true;
}

// ============================================================================
// AdGuard 域名级规则解析:
//   支持:domain、||domain^、*.domain 以及对应的 @@ 白名单
//   跳过:cosmetic/scriptlet、URL/路径、正则、$ 请求级修饰符和 badfilter
// 模块只能看到域名，不能把请求级规则静默扩大为整域名拦截。
bool parse_adguard_rule(const std::string &raw, ParsedDomainRule &rule) {
    rule = {};
    std::string line = raw;
    size_t b = line.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return 0;
    size_t e = line.find_last_not_of(" \t\r\n");
    line = line.substr(b, e - b + 1);
    if (line.empty() || line[0] == '!' || line[0] == '#' || line[0] == '[') return 0;

    // cosmetic / scriptlet 规则跳过(## #@# #$# #%#)
    if (line.find("##") != std::string::npos ||
        line.find("#@#") != std::string::npos ||
        line.find("#$#") != std::string::npos ||
        line.find("#%#") != std::string::npos ||
        line.find("#?#") != std::string::npos) return 0;

    // 白名单前缀(@@)
    bool is_white = false;
    if (line.compare(0, 2, "@@") == 0) {
        is_white = true;
        line = line.substr(2);
        b = line.find_first_not_of(" \t");
        if (b != std::string::npos) line = line.substr(b);
    }
    // 通配符域名:*.example.com → 后缀规则
    bool suffix = false;
    if (line.compare(0, 2, "*.") == 0) {
        suffix = true;
        line = line.substr(2);
    } else if (line.compare(0, 2, "||") == 0) {
        // ||example.com → 后缀规则
        suffix = true;
        line = line.substr(2);
    } else if (!line.empty() && line[0] == '|') {
        return 0;
    }

    // 只允许域名锚点末尾的单个 ^；其它分隔符代表路径、正则或条件规则。
    if (!line.empty() && line.back() == '^') line.pop_back();
    // Modifiers (including $badfilter) are unsupported syntax. A domain label
    // containing the word "badfilter" is not a modifier and remains valid.
    if (line.find_first_of("^$*/| \t") != std::string::npos) return 0;

    // 小写归一
    for (char &c : line) if (c >= 'A' && c <= 'Z') c += 32;

    if (!valid_domain(line)) { rule.white = is_white; rule.invalid_domain = true; return false; }
    rule = {line, is_white, suffix};
    return true;
}

int parse_adguard_line(const std::string &raw, RuleDB &db) {
    ParsedDomainRule rule;
    if (!parse_adguard_rule(raw, rule)) {
        if (rule.invalid_domain) {
            if (rule.white) db.white.bad_lines++; else db.black.bad_lines++;
        }
        return 0;
    }
    if (rule.white) {
        if (rule.suffix) db.white.add_suffix(rule.domain);
        else        db.white.add_exact(rule.domain);
    } else {
        if (rule.suffix) db.black.add_suffix(rule.domain);
        else        db.black.add_exact(rule.domain);
    }
    return 1;
}

// ============================================================================
// 从文件加载规则(自动识别 gzip)
bool load_rule_file_checked(const char *path, bool adguard, RuleDB &db,
                            int *loaded, bool require_gzip) {
    if (loaded) *loaded = 0;
    RuleDB staged;
    int n = 0;
    RuleStreamResult stream;
    bool ok = stream_rule_file(path, require_gzip,
                               [&](const std::string &line, bool overlong) {
        if (overlong) { staged.black.bad_lines++; return; }
        n += adguard ? parse_adguard_line(line, staged)
                     : parse_hosts_line(line, staged);
    }, &stream);
    if (!ok) return false;
    // 只有完整通过 gzip 校验和全部资源上限后，才把该文件结果并入调用方。
    db.black.merge_from(staged.black);
    db.white.merge_from(staged.white);
    if (loaded) *loaded = n;
    return true;
}

int load_rule_file(const char *path, bool adguard, RuleDB &db) {
    int loaded = 0;
    return load_rule_file_checked(path, adguard, db, &loaded) ? loaded : 0;
}

// ============================================================================
// 加载全部规则(三文件:hosts_rules.gz + adguard_rules.gz + custom_rules.txt)
void load_all_rules(RuleDB &db) {
    // 模式(adblock_mode.txt 的 mode 键)
    int selected_mode = MODE_HOSTS_ADGUARD;
    {
        std::string mode = read_conf_value(FILE_STATE_CONF, "mode");
        if (mode.empty()) mode = read_conf_value(FILE_ADBLOCK_MODE, "mode");
        if (mode == "1") selected_mode = MODE_HOSTS_ONLY;
    }

    RuleBundlePaths bundle = resolve_rule_bundle(
        DATA_DIR, FILE_RULESET_CURRENT, FILE_HOSTS_RULES, FILE_ADGUARD_RULES);
    struct stat manifest {};
    if (!bundle.from_manifest &&
        (lstat(FILE_RULESET_CURRENT, &manifest) == 0 || errno != ENOENT)) {
        db = RuleDB{};
        db.mode = selected_mode;
        db.load_valid = false;
        db.load_error = "规则 generation 指针或分类文件不可用";
        return;
    }
    load_rule_files(db, selected_mode, bundle.hosts_path, bundle.adguard_path,
                    FILE_CUSTOM_RULES, bundle.from_manifest);
}

void load_rule_files(RuleDB &db, int mode, const std::string &hosts,
                     const std::string &adguard, const std::string &custom,
                     bool require_bundle_files) {
    db = RuleDB{};
    db.mode = mode;
    auto load = [&](const std::string &path, bool adguard_syntax, bool gzip,
                    bool required) {
        if (!db.load_valid) return;
        struct stat info {};
        const int status = stat(path.c_str(), &info);
        if (status != 0 && errno == ENOENT && !required) return;
        // Clearing custom rules intentionally produces a zero-byte plain file.
        if (status == 0 && S_ISREG(info.st_mode) && info.st_size == 0 && !gzip) return;
        if (!load_rule_file_checked(path.c_str(), adguard_syntax, db, nullptr, gzip)) {
            db.load_valid = false;
            db.load_error = "规则文件读取或完整性校验失败: " + path;
        }
    };
    load(hosts, false, true, require_bundle_files);
    if (mode == MODE_HOSTS_ADGUARD) load(adguard, true, true, require_bundle_files);
    load(custom, true, false, false);
    if (!db.load_valid) { db.black.clear(); db.white.clear(); }
    db.loaded_at = (long)now_epoch();
    db.ok = db.load_valid && (db.black.size() + db.white.size()) > 0;
}

// 规则统计写入 rule_stats.conf(WebUI 规则总数的来源)
void save_rule_stats(const RuleDB &db) {
    char buf[256];
    std::string out;
    snprintf(buf, sizeof(buf), "mode=%d\n", db.mode);          out += buf;
    snprintf(buf, sizeof(buf), "black=%zu\n", db.black.size()); out += buf;
    snprintf(buf, sizeof(buf), "white=%zu\n", db.white.size()); out += buf;
    snprintf(buf, sizeof(buf), "updated_at=%ld\n", db.loaded_at); out += buf;
    write_file_atomic(FILE_RULE_STATS, out);
}

// ============================================================================
// 全局不可变规则快照。C++17 对 shared_ptr 提供原子 load/store 自由函数；匹配线程
// 持有自己的引用，因此发布线程无需等待旧规则上的在途查询结束。
static RuleSnapshot g_rules_snapshot = std::make_shared<const RuleDB>();

RuleSnapshot rules_snapshot() {
    return std::atomic_load_explicit(&g_rules_snapshot, std::memory_order_acquire);
}

void publish_rules_snapshot(RuleSnapshot snapshot) {
    if (!snapshot) snapshot = std::make_shared<const RuleDB>();
    // 在任何数据面线程取得指针前完成排序去重；发布后只读，不触发惰性整理。
    snapshot->finalize();
    std::atomic_store_explicit(
        &g_rules_snapshot, std::move(snapshot), std::memory_order_release);
}

// 四态匹配:白名单优先,其次黑名单(精确 → 后缀),未命中默认放行
Verdict match_domain(const std::string &domain) {
    RuleSnapshot snapshot = rules_snapshot();
    if (snapshot->white.classify(domain) != RuleSet::Hit::NONE)
        return Verdict::PASS_WHITELIST;
    RuleSet::Hit h = snapshot->black.classify(domain);
    if (h == RuleSet::Hit::EXACT) return Verdict::BLOCK_EXACT;
    if (h == RuleSet::Hit::SUFFIX) return Verdict::BLOCK_SUFFIX;
    return Verdict::PASS_DEFAULT;
}

// 四态日志文案
const char *verdict_text(Verdict v) {
    switch (v) {
        case Verdict::PASS_WHITELIST: return "PASS(whitelist)";
        case Verdict::BLOCK_SUFFIX:   return "BLOCK(suffix)";
        case Verdict::BLOCK_EXACT:    return "BLOCK(exact)";
        default:                      return "PASS(default)";
    }
}
