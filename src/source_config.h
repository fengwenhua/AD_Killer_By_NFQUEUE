#pragma once

#include "net_policy.h"
#include "builtin_sources.h"
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

struct RuleSource {
    std::string url;
    bool enabled = true;
};

constexpr size_t MAX_SOURCE_CONFIG_BYTES = 256u * 1024u;
constexpr size_t MAX_RULE_SOURCES = 64;

// Existing URL-per-line files remain valid. Only our explicit marker denotes
// a disabled entry; ordinary user comments must not become subscriptions.
inline bool parse_rule_sources(const std::string &text,
                               std::vector<RuleSource> &entries,
                               std::string &error, bool strict = false) {
    entries.clear();
    error.clear();
    if (text.size() > MAX_SOURCE_CONFIG_BYTES) {
        error = "来源配置过大";
        return false;
    }
    std::istringstream input(text);
    std::unordered_set<std::string> urls;
    std::string line;
    size_t ordinary_count = 0, builtin_count = 0;
    while (std::getline(input, line)) {
        size_t start = line.find_first_not_of(" \t\r");
        if (start == std::string::npos) continue;
        line = line.substr(start, line.find_last_not_of(" \t\r") - start + 1);
        bool enabled = true;
        const std::string marker = "# disabled: ";
        if (line.compare(0, marker.size(), marker) == 0) {
            enabled = false;
            line = line.substr(marker.size());
        } else if (line[0] == '#') continue;
        const bool builtin = is_builtin_source(line);
        if (!builtin && line.compare(0, 8, "builtin:") == 0) {
            error = "未知内置来源，特殊配方不允许编辑";
            return false;
        }
        if (strict && ((!builtin && !validate_download_url(line, &error)) || !urls.insert(line).second)) {
            if (error.empty()) error = "来源地址重复";
            return false;
        }
        entries.push_back({line, enabled});
        if (builtin) ++builtin_count; else ++ordinary_count;
        if (ordinary_count > MAX_RULE_SOURCES || builtin_count > 1) {
            error = "最多支持 64 个普通来源和 1 个内置特殊来源，内置项不得重复";
            return false;
        }
    }
    return true;
}

// Old/custom URL lists get the immutable built-in row without opting into it.
// Omitting it on save cannot delete it or change an existing enabled state.
inline void retain_builtin_source(std::vector<RuleSource> &entries,
                                  const std::vector<RuleSource> &previous = {}) {
    for (const auto &entry : entries) if (is_builtin_source(entry.url)) return;
    bool enabled = false;
    for (const auto &entry : previous) if (is_builtin_source(entry.url)) enabled = entry.enabled;
    entries.push_back({HAGEZI_SOURCE_ID, enabled});
}

inline std::string serialize_rule_sources(const std::vector<RuleSource> &entries) {
    std::string text = "# AD_Killer_By_NFQUEUE rule sources; disabled entries are preserved\n";
    for (const auto &entry : entries)
        text += (entry.enabled ? "" : "# disabled: ") + entry.url + "\n";
    return text;
}

inline std::string source_json_string(const std::string &text) {
    std::string result = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { result += '\\'; result += c; }
        else if (c < 32 || c == 127) {
            result += "\\u00";
            result += hex[c >> 4]; result += hex[c & 15];
        } else result += c;
    }
    return result + '"';
}
