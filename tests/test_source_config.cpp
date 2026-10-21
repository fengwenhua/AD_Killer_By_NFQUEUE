#include "source_config.h"
#include "rule_recipe.h"
#include <cassert>
#include <iostream>

int main() {
    std::vector<RuleSource> entries;
    std::string error;
    assert(default_rule_recipe().size() == 13);
    assert(parse_rule_sources(default_rule_sources_text(), entries, error, true));
    assert(entries.size() == 14 && entries.back().url == HAGEZI_SOURCE_ID && !entries.back().enabled);
    assert(superseded_default_rule_sources_text() ==
           "# 每行一个规则源 URL,支持 Hosts / AdGuard / gzip(.gz)\n"
           "# 以下为默认源,可自行增删\n"
           "https://anti-ad.net/easylist.txt\n"
           "https://adguardteam.github.io/AdGuardSDNSFilter/Filters/filter.txt\n");
    assert(should_refresh_default_rule_sources(superseded_default_rule_sources_text()));
    assert(!should_refresh_default_rule_sources(default_rule_sources_text()));
    assert(!should_refresh_default_rule_sources("# custom\nhttps://example.org/user\n"));
    for (const auto &source : default_rule_recipe()) {
        assert(std::string(source.url).find("hagezi") == std::string::npos);
        assert(recipe_source_format(source.url) != SourceFormat::AUTO);
    }
    assert(parse_rule_sources("# comment\nhttps://example.org/a\n# disabled: https://example.org/b\n", entries, error, true));
    assert(entries.size() == 2 && entries[0].enabled && !entries[1].enabled);
    std::string saved = serialize_rule_sources(entries);
    assert(parse_rule_sources(saved, entries, error, true) && entries.size() == 2 && !entries[1].enabled);
    for (const auto &invalid : {"file:///tmp/a\n", "https://user:pass@example.org/a\n",
                               "https://example.org/a\n# disabled: https://example.org/a\n",
                               "https://example.org/a\tbad\n"})
        assert(!parse_rule_sources(invalid, entries, error, true));
    assert(parse_rule_sources("# comment\n", entries, error, true) && entries.empty());
    assert(!parse_rule_sources(std::string(MAX_SOURCE_CONFIG_BYTES + 1, 'a'), entries, error));
    std::string many;
    for (size_t i = 0; i <= MAX_RULE_SOURCES; i++) many += "https://example.org/" + std::to_string(i) + "\n";
    assert(!parse_rule_sources(many, entries, error, true));
    assert(source_json_string("a\"\n\\") == "\"a\\\"\\u000a\\\\\"");
    assert(parse_rule_sources(std::string(HAGEZI_SOURCE_ID) + "\n", entries, error, true));
    assert(entries.size() == 1 && entries[0].enabled);
    auto previous = entries;
    entries.clear(); retain_builtin_source(entries, previous);
    assert(entries.size() == 1 && entries[0].url == HAGEZI_SOURCE_ID && entries[0].enabled);
    entries.clear(); retain_builtin_source(entries);
    assert(entries.size() == 1 && !entries[0].enabled);
    assert(!parse_rule_sources("builtin:edited-hagezi\n", entries, error, true));
    assert(!parse_rule_sources(std::string(HAGEZI_SOURCE_ID) + "\n" + HAGEZI_SOURCE_ID + "\n", entries, error));
    many.clear();
    for (size_t i = 0; i < MAX_RULE_SOURCES; ++i)
        many += "https://example.org/" + std::to_string(i) + "\n";
    assert(parse_rule_sources(many, entries, error, true));
    retain_builtin_source(entries);
    assert(entries.size() == MAX_RULE_SOURCES + 1);
    assert(parse_rule_sources(serialize_rule_sources(entries), entries, error, true));
    assert(entries.size() == MAX_RULE_SOURCES + 1 && !entries.back().enabled);
    std::cout << "source configuration tests passed\n";
}
