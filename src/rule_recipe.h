#pragma once
#include <cstddef>
#include <algorithm>
#include <string>
#include <vector>
#include "builtin_sources.h"
#include "source_config.h"

// Formats:
//   AUTO              user URL: try Hosts syntax into hosts_rules, then AdGuard
//   ADGUARD           AdGuard syntax -> adguard_rules
//   HOSTS_AS_ADGUARD  Hosts syntax normalized to exact domains -> adguard_rules
//   HOSTS             Hosts syntax -> hosts_rules (multi-source Hosts union)
enum class SourceFormat { AUTO, ADGUARD, HOSTS_AS_ADGUARD, HOSTS };
struct RecipeSource {
    const char *name;
    const char *url;
    SourceFormat format;
};
inline const std::vector<RecipeSource> &default_rule_recipe() {
    static const std::vector<RecipeSource> sources = {
#define RULE_SOURCE(name, url, format) {name, url, SourceFormat::format},
#include "rule_recipe.inc"
#undef RULE_SOURCE
    };
    return sources;
}
// Only literal addresses from our recipe (and the locked dependencies) gain
// mirrors. Never turn an arbitrary user URL into a different repository/path.
inline std::string github_rule_cdn_url(const std::string &url, const std::string &host = "cdn.jsdelivr.net") {
    const std::string prefix = "https://raw.githubusercontent.com/";
    if (url.compare(0, prefix.size(), prefix) != 0) return "";
    const auto repository = url.find('/', prefix.size());
    const auto branch = repository == std::string::npos ? repository : url.find('/', repository + 1);
    const auto file = branch == std::string::npos ? branch : url.find('/', branch + 1);
    if (file == std::string::npos || file + 1 == url.size()) return "";
    return "https://" + host + "/gh/" + url.substr(prefix.size(), branch - prefix.size()) +
           "@" + url.substr(branch + 1, file - branch - 1) + url.substr(file);
}
inline std::string canonical_rule_source_url(const std::string &url) {
    const auto matches = [&](const std::string &original) {
        const auto cdn = github_rule_cdn_url(original);
        return url == original || (!cdn.empty() && (url == cdn || url == github_rule_cdn_url(original, "fastly.jsdelivr.net")));
    };
    for (const auto &source : default_rule_recipe()) if (matches(source.url)) return source.url;
    for (const char *dependency : {HAGEZI_PRO_URL, HAGEZI_FAKE_URL}) if (matches(dependency)) return dependency;
    return url;
}
inline std::vector<std::string> rule_source_endpoints(const std::string &component, const std::string &configured = "") {
    const auto canonical = canonical_rule_source_url(component);
    bool known = canonical == HAGEZI_PRO_URL || canonical == HAGEZI_FAKE_URL;
    for (const auto &source : default_rule_recipe()) known = known || canonical == source.url;
    const auto cdn = github_rule_cdn_url(canonical);
    if (!known || cdn.empty()) return {component};
#if defined(ADK_NFQ_TEST_SOURCE_MIRROR_BASE)
    // Disposable loopback HTTP fixtures. Production build.sh never defines it.
    const std::string base = ADK_NFQ_TEST_SOURCE_MIRROR_BASE;
    const auto suffix = canonical.substr(std::string("https://raw.githubusercontent.com/").size());
    std::vector<std::string> endpoints = {base + "/cdn/" + suffix, base + "/fastly/" + suffix, base + "/origin/" + suffix};
#else
    std::vector<std::string> endpoints = {cdn, github_rule_cdn_url(canonical, "fastly.jsdelivr.net"), canonical};
#endif
    // An explicitly saved CDN alias remains the first choice for that source.
    if (!configured.empty() && configured != canonical) {
        if (configured == github_rule_cdn_url(canonical, "fastly.jsdelivr.net"))
            std::rotate(endpoints.begin(), endpoints.begin() + 1, endpoints.begin() + 2);
    }
    return endpoints;
}
inline SourceFormat recipe_source_format(const std::string &url) {
    const auto canonical = canonical_rule_source_url(url);
    for (const auto &source : default_rule_recipe())
        if (canonical == source.url) return source.format;
    return SourceFormat::AUTO;
}
inline const char *recipe_source_name(const std::string &url) {
    if (is_builtin_source(url)) return HAGEZI_SOURCE_NAME;
    const auto canonical = canonical_rule_source_url(url);
    for (const auto &source : default_rule_recipe())
        if (canonical == source.url) return source.name;
    return "";
}
inline const char *source_format_name(SourceFormat format) {
    if (format == SourceFormat::ADGUARD) return "adguard";
    if (format == SourceFormat::HOSTS_AS_ADGUARD) return "hosts-as-adguard";
    if (format == SourceFormat::HOSTS) return "hosts";
    return "auto";
}
inline std::string default_rule_sources_text() {
    std::string text = "# AD_Killer_By_NFQUEUE: " +
                       std::to_string(default_rule_recipe().size()) +
                       " ordinary sources; built-in HaGeZi pair enabled by default\n";
    for (const auto &source : default_rule_recipe())
        text += std::string("# ") + source.name + "\n" + source.url + "\n";
    text += std::string(HAGEZI_SOURCE_ID) + "\n";
    return text;
}

// Replace only exact untouched historical templates. Any user edit, including
// comments or enablement changes, must be preserved byte-for-byte.
inline std::string superseded_default_rule_sources_text() {
    return "# 每行一个规则源 URL,支持 Hosts / AdGuard / gzip(.gz)\n"
           "# 以下为默认源,可自行增删\n"
           "https://anti-ad.net/easylist.txt\n"
           "https://adguardteam.github.io/AdGuardSDNSFilter/Filters/filter.txt\n";
}

// Frozen v1 template: 13 ordinary sources (AdRules present), built-in disabled.
inline std::string legacy_default_rule_sources_text_v1() {
    return "# AD_Killer_By_NFQUEUE: 13 ordinary sources; built-in HaGeZi pair disabled by default\n"
           "# AdGuard DNS filter\nhttps://adguardteam.github.io/AdGuardSDNSFilter/Filters/filter.txt\n"
           "# AdGuard Mobile Ads filter\nhttps://adguardteam.github.io/AdguardFilters/MobileFilter/sections/adservers.txt\n"
           "# AdGuard Tracking Protection filter\nhttps://filters.adtidy.org/extension/ublock/filters/3.txt\n"
           "# EasyList\nhttps://easylist-downloads.adblockplus.org/easylist.txt\n"
           "# EasyList China\nhttps://easylist-downloads.adblockplus.org/easylistchina.txt\n"
           "# EasyPrivacy\nhttps://easylist-downloads.adblockplus.org/easyprivacy.txt\n"
           "# oisd small\nhttps://small.oisd.nl/abp\n"
           "# AdRules DNS List\nhttps://adrules.top/adrules.txt\n"
           "# CJX's Annoyance List\nhttps://raw.githubusercontent.com/cjx82630/cjxlist/master/cjx-annoyance.txt\n"
           "# 乘风广告过滤规则\nhttps://raw.githubusercontent.com/xinggsf/Adblock-Plus-Rule/master/rule.txt\n"
           "# anti-AD for AdGuard\nhttps://anti-ad.net/adguard.txt\n"
           "# AWAvenue Ads Rule\nhttps://raw.githubusercontent.com/TG-Twilight/AWAvenue-Ads-Rule/main/AWAvenue-Ads-Rule.txt\n"
           "# hostsVN\nhttps://raw.githubusercontent.com/bigdargon/hostsVN/master/hosts\n"
           "# disabled: builtin:hagezi-pro-minus-fake\n";
}

// Frozen v1b template: 12 ordinary sources, built-in enabled.
inline std::string legacy_default_rule_sources_text_v1b() {
    return "# AD_Killer_By_NFQUEUE: 12 ordinary sources; built-in HaGeZi pair enabled by default\n"
           "# AdGuard DNS filter\nhttps://adguardteam.github.io/AdGuardSDNSFilter/Filters/filter.txt\n"
           "# AdGuard Mobile Ads filter\nhttps://adguardteam.github.io/AdguardFilters/MobileFilter/sections/adservers.txt\n"
           "# AdGuard Tracking Protection filter\nhttps://filters.adtidy.org/extension/ublock/filters/3.txt\n"
           "# EasyList\nhttps://easylist-downloads.adblockplus.org/easylist.txt\n"
           "# EasyList China\nhttps://easylist-downloads.adblockplus.org/easylistchina.txt\n"
           "# EasyPrivacy\nhttps://easylist-downloads.adblockplus.org/easyprivacy.txt\n"
           "# oisd small\nhttps://small.oisd.nl/abp\n"
           "# CJX's Annoyance List\nhttps://raw.githubusercontent.com/cjx82630/cjxlist/master/cjx-annoyance.txt\n"
           "# 乘风广告过滤规则\nhttps://raw.githubusercontent.com/xinggsf/Adblock-Plus-Rule/master/rule.txt\n"
           "# anti-AD for AdGuard\nhttps://anti-ad.net/adguard.txt\n"
           "# AWAvenue Ads Rule\nhttps://raw.githubusercontent.com/TG-Twilight/AWAvenue-Ads-Rule/main/AWAvenue-Ads-Rule.txt\n"
           "# hostsVN\nhttps://raw.githubusercontent.com/bigdargon/hostsVN/master/hosts\n"
           "builtin:hagezi-pro-minus-fake\n";
}

inline bool should_refresh_default_rule_sources(const std::string &configured) {
    return configured == superseded_default_rule_sources_text() ||
           configured == legacy_default_rule_sources_text_v1() ||
           configured == legacy_default_rule_sources_text_v1b();
}

// One-time v2 migration. Explicit enablement/deletions of old sources remain
// user policy. Only untouched frozen templates are replaced by the caller.
// Append new defaults within the same count/byte limits as the editor.
inline std::string migrate_source_defaults(const std::string &configured) {
    struct AddedSource { const char *name; const char *url; };
    static const AddedSource new_sources[] = {
        {"HaGeZi Light DNS Blocklist", "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/adblock/light.txt"},
        {"AdRules DNS List", "https://adrules.top/dns.txt"},
        {"hostsVN Threat", "https://raw.githubusercontent.com/bigdargon/hostsVN/master/extensions/threat/filter.txt"},
        {"轻量广告拦截规则", "https://raw.githubusercontent.com/damengzhu/banad/main/jiekouAD.txt"},
        {"anti-AD domains", "https://anti-ad.net/domains.txt"},
        {"oisd big", "https://big.oisd.nl/domainswild2"},
        {"1Hosts Lite", "https://raw.githubusercontent.com/badmojr/1Hosts/master/Lite/wildcards.txt"},
        {"StevenBlack hosts", "https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts"},
    };
    const std::string old_adrules = "https://adrules.top/adrules.txt";
    const std::string new_adrules = "https://adrules.top/dns.txt";
    const std::string old_chengfeng = "https://raw.githubusercontent.com/xinggsf/Adblock-Plus-Rule/master/rule.txt";
    const std::string new_chengfeng = "https://raw.githubusercontent.com/xinggsf/Adblock-Plus-Rule/master/mv.txt";
    const std::string disabled = "# disabled: ";
    const auto trimmed = [](const std::string &line) {
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string{};
        return line.substr(first, line.find_last_not_of(" \t\r\n") - first + 1);
    };
    const auto address = [&](const std::string &line) {
        const auto value = trimmed(line);
        if (value.compare(0, disabled.size(), disabled) == 0) return value.substr(disabled.size());
        return value.empty() || value[0] == '#' ? std::string{} : value;
    };
    std::vector<std::string> input;
    std::unordered_set<std::string> original;
    for (size_t begin = 0; begin < configured.size();) {
        const auto end = configured.find('\n', begin);
        const auto stop = end == std::string::npos ? configured.size() : end + 1;
        input.push_back(configured.substr(begin, stop - begin));
        const auto url = address(input.back());
        if (!url.empty()) original.insert(url);
        begin = stop;
    }
    std::vector<std::string> output;
    std::unordered_set<std::string> present;
    for (const auto &raw : input) {
        const auto url = address(raw);
        if (url == old_adrules || url == old_chengfeng) {
            const auto replacement = url == old_adrules ? new_adrules : new_chengfeng;
            // An explicitly configured replacement wins, including its disabled
            // state. Do not introduce duplicates during URL migration.
            if (original.count(replacement)) {
                if (!output.empty() && (trimmed(output.back()) == "# AdRules DNS List" ||
                    trimmed(output.back()) == "# 乘风广告过滤规则")) output.pop_back();
                continue;
            }
            if (url == old_chengfeng && !output.empty() && trimmed(output.back()) == "# 乘风广告过滤规则")
                output.back() = "# 乘风 视频过滤规则\n";
            output.push_back((trimmed(raw).compare(0, disabled.size(), disabled) == 0 ? disabled : "") + replacement + "\n");
            present.insert(replacement);
        } else {
            output.push_back(raw);
            if (!url.empty()) present.insert(url);
        }
    }
    std::string result;
    size_t builtin_at = std::string::npos;
    for (const auto &raw : output) {
        if (address(raw) == HAGEZI_SOURCE_ID) builtin_at = result.size();
        result += raw;
    }
    if (!present.count(HAGEZI_SOURCE_ID)) {
        const auto row = (result.empty() || result.back() == '\n' ? "" : "\n") + disabled + HAGEZI_SOURCE_ID + "\n";
        if (row.size() <= MAX_SOURCE_CONFIG_BYTES - std::min(result.size(), MAX_SOURCE_CONFIG_BYTES)) {
            builtin_at = result.size(); result += row; present.insert(HAGEZI_SOURCE_ID);
        }
    }
    size_t ordinary = present.size() - present.count(HAGEZI_SOURCE_ID);
    size_t insert_at = builtin_at == std::string::npos ? result.size() : builtin_at;
    for (const auto &source : new_sources) {
        if (present.count(source.url) || ordinary >= MAX_RULE_SOURCES) continue;
        const std::string row = (insert_at == 0 || result[insert_at - 1] == '\n' ? "" : "\n") +
            std::string("# ") + source.name + "\n" + source.url + "\n";
        if (row.size() > MAX_SOURCE_CONFIG_BYTES - std::min(result.size(), MAX_SOURCE_CONFIG_BYTES)) continue;
        result.insert(insert_at, row); insert_at += row.size();
        present.insert(source.url); ++ordinary;
    }
    return result;
}
