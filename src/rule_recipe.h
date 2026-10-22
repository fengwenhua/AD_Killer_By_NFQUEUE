#pragma once
#include <string>
#include <vector>
#include "builtin_sources.h"

enum class SourceFormat { AUTO, ADGUARD, HOSTS_AS_ADGUARD };
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
    return "auto";
}
inline std::string default_rule_sources_text() {
    std::string text = "# AD_Killer_By_NFQUEUE: 12 ordinary sources; built-in HaGeZi pair enabled by default\n";
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

inline bool should_refresh_default_rule_sources(const std::string &configured) {
    std::string previous = "# AD_Killer_By_NFQUEUE: 13 ordinary sources; built-in HaGeZi pair disabled by default\n";
    size_t index=0;
    for (const auto &source : default_rule_recipe()) {
        if (index++ == 7) previous += "# AdRules DNS List\nhttps://adrules.top/adrules.txt\n";
        previous += std::string("# ") + source.name + "\n" + source.url + "\n";
    }
    previous += std::string("# disabled: ") + HAGEZI_SOURCE_ID + "\n";
    return configured == superseded_default_rule_sources_text() || configured == previous;
}

// One-time targeted policy migration. Preserve every unrelated line verbatim;
// do not download/publish rules here. Later user choices are protected by the
// persisted source_defaults_version marker written by --install.
inline std::string migrate_source_defaults(const std::string &configured) {
    std::string output;
    bool builtin=false;
    size_t begin=0;
    while (begin<configured.size()) {
        auto end=configured.find('\n',begin);
        if (end==std::string::npos) end=configured.size(); else ++end;
        auto line=configured.substr(begin,end-begin);
        auto first=line.find_first_not_of(" \t\r\n"), last=line.find_last_not_of(" \t\r\n");
        auto trimmed=first==std::string::npos ? "" : line.substr(first,last-first+1);
        const std::string disabled="# disabled: ";
        if (trimmed=="https://adrules.top/adrules.txt" || trimmed==disabled+"https://adrules.top/adrules.txt") {
            begin=end; continue;
        }
        if (trimmed==HAGEZI_SOURCE_ID || trimmed==disabled+HAGEZI_SOURCE_ID) {
            if (!builtin) output+=std::string(HAGEZI_SOURCE_ID)+"\n";
            builtin=true;
        } else output+=line;
        begin=end;
    }
    if (!builtin) {
        if (!output.empty() && output.back()!='\n') output+='\n';
        output+=std::string(HAGEZI_SOURCE_ID)+"\n";
    }
    return output;
}
