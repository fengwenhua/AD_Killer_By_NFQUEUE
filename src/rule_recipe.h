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
    std::string text = "# AD_Killer_By_NFQUEUE: 13 ordinary sources; built-in HaGeZi pair disabled by default\n";
    for (const auto &source : default_rule_recipe())
        text += std::string("# ") + source.name + "\n" + source.url + "\n";
    text += std::string("# disabled: ") + HAGEZI_SOURCE_ID + "\n";
    return text;
}

// Replace only this exact early two-source template. Any user edit, including
// comments or enablement changes, must be preserved byte-for-byte.
inline std::string superseded_default_rule_sources_text() {
    return "# 每行一个规则源 URL,支持 Hosts / AdGuard / gzip(.gz)\n"
           "# 以下为默认源,可自行增删\n"
           "https://anti-ad.net/easylist.txt\n"
           "https://adguardteam.github.io/AdGuardSDNSFilter/Filters/filter.txt\n";
}

inline bool should_refresh_default_rule_sources(const std::string &configured) {
    return configured == superseded_default_rule_sources_text();
}
