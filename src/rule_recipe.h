#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "builtin_sources.h"

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
inline SourceFormat recipe_source_format(const std::string &url) {
    for (const auto &source : default_rule_recipe())
        if (url == source.url) return source.format;
    return SourceFormat::AUTO;
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

// One-time source-defaults migration to version 2. Preserve every unrelated
// line verbatim; do not download/publish rules here. Changes:
//   - drop the retired https://adrules.top/adrules.txt landing-page URL;
//   - switch 乘风 rule.txt to the video list mv.txt;
//   - append newly introduced defaults (HaGeZi Light, AdRules DNS, hostsVN
//     Threat, banad and four Hosts lists) unless the URL is already present;
//   - keep the built-in HaGeZi pair enabled exactly once.
// Later user choices are protected by the persisted source_defaults_version
// marker written by --install.
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
    constexpr size_t new_count = sizeof(new_sources) / sizeof(new_sources[0]);
    const std::string old_adrules = "https://adrules.top/adrules.txt";
    const std::string old_chengfeng = "https://raw.githubusercontent.com/xinggsf/Adblock-Plus-Rule/master/rule.txt";
    const std::string new_chengfeng = "https://raw.githubusercontent.com/xinggsf/Adblock-Plus-Rule/master/mv.txt";
    const std::string disabled = "# disabled: ";

    std::vector<std::string> input;
    for (size_t begin = 0; begin < configured.size();) {
        const auto end = configured.find('\n', begin);
        const auto stop = end == std::string::npos ? configured.size() : end + 1;
        input.push_back(configured.substr(begin, stop - begin));
        begin = stop;
    }
    const auto trimmed = [](const std::string &line) {
        const auto first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string{};
        const auto last = line.find_last_not_of(" \t\r\n");
        return line.substr(first, last - first + 1);
    };

    std::vector<std::string> output;
    bool present[new_count] = {};
    bool builtin = false;
    size_t builtin_at = std::string::npos;
    for (const auto &raw : input) {
        const std::string line = trimmed(raw);
        if (line.empty()) { output.push_back(raw); continue; }
        if (line.rfind("# AD_Killer_By_NFQUEUE:", 0) == 0) {
            output.push_back("# AD_Killer_By_NFQUEUE: " +
                             std::to_string(default_rule_recipe().size()) +
                             " ordinary sources; built-in HaGeZi pair enabled by default\n");
            continue;
        }
        const bool old_adrules_line = line == old_adrules || line == disabled + old_adrules;
        if (old_adrules_line) {
            if (!output.empty() && trimmed(output.back()) == "# AdRules DNS List") output.pop_back();
            continue;
        }
        if (line == old_chengfeng || line == disabled + old_chengfeng) {
            if (!output.empty() && trimmed(output.back()) == "# 乘风广告过滤规则")
                output.back() = "# 乘风 视频过滤规则\n";
            output.push_back((line == old_chengfeng ? "" : disabled) + new_chengfeng + "\n");
            continue;
        }
        if (line == HAGEZI_SOURCE_ID || line == disabled + HAGEZI_SOURCE_ID) {
            if (!builtin) {
                builtin_at = output.size();
                output.push_back(std::string(HAGEZI_SOURCE_ID) + "\n");
                builtin = true;
            }
            continue;
        }
        std::string probe = line;
        if (probe.compare(0, disabled.size(), disabled) == 0) probe = probe.substr(disabled.size());
        for (size_t k = 0; k < new_count; ++k)
            if (probe == new_sources[k].url) present[k] = true;
        output.push_back(raw);
    }
    std::vector<std::string> missing;
    for (size_t k = 0; k < new_count; ++k) {
        if (present[k]) continue;
        missing.push_back(std::string("# ") + new_sources[k].name + "\n");
        missing.push_back(std::string(new_sources[k].url) + "\n");
    }
    if (builtin_at != std::string::npos) {
        output.insert(output.begin() + static_cast<std::ptrdiff_t>(builtin_at),
                      missing.begin(), missing.end());
    } else {
        output.insert(output.end(), missing.begin(), missing.end());
        output.push_back(std::string(HAGEZI_SOURCE_ID) + "\n");
    }
    std::string result;
    for (const auto &line : output) result += line;
    return result;
}
