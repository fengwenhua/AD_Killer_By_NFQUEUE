#include "source_config.h"
#include "rule_recipe.h"
#include <cassert>
#include <iostream>

int main() {
    std::vector<RuleSource> entries;
    std::string error;
    const auto count_of = [](const std::string &text, const std::string &needle) {
        size_t count = 0;
        for (size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += needle.size()) ++count;
        return count;
    };
    assert(default_rule_recipe().size() == 20);
    assert(std::string(recipe_source_name(HAGEZI_SOURCE_ID)) == HAGEZI_SOURCE_NAME);
    assert(std::string(recipe_source_name("https://example.org/custom")).empty());
    for (const auto &source : default_rule_recipe())
        assert(std::string(recipe_source_name(source.url)) == source.name);
    const std::string github_source = "https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts";
    const std::string cdn_source = "https://cdn.jsdelivr.net/gh/StevenBlack/hosts@master/hosts";
    const std::string fastly_source = "https://fastly.jsdelivr.net/gh/StevenBlack/hosts@master/hosts";
    assert(rule_source_endpoints(github_source) == std::vector<std::string>({cdn_source, fastly_source, github_source}));
    assert(rule_source_endpoints(github_source, fastly_source).front() == fastly_source);
    assert(canonical_rule_source_url(cdn_source) == github_source && canonical_rule_source_url(fastly_source) == github_source);
    assert(recipe_source_format(cdn_source) == SourceFormat::HOSTS);
    assert(std::string(recipe_source_name(fastly_source)) == "StevenBlack hosts");
    for (const std::string unknown : {"https://raw.githubusercontent.com/StevenBlack/hosts/master/custom.txt",
                                    "https://raw.githubusercontent.com/evil/repo/main/hosts",
                                    "https://cdn.jsdelivr.net/gh/StevenBlack/hosts@master/other.txt"}) {
        assert(rule_source_endpoints(unknown) == std::vector<std::string>({unknown}));
        assert(canonical_rule_source_url(unknown) == unknown);
        assert(recipe_source_format(unknown) == SourceFormat::AUTO);
    }
    assert(parse_rule_sources(default_rule_sources_text(), entries, error, true));
    assert(entries.size() == 21 && entries.back().url == HAGEZI_SOURCE_ID && entries.back().enabled);
    assert(default_rule_sources_text().find("https://adrules.top/adrules.txt") == std::string::npos);
    assert(default_rule_sources_text().find("https://adrules.top/dns.txt") != std::string::npos);
    assert(default_rule_sources_text().find("https://anti-ad.net/domains.txt") != std::string::npos);
    assert(recipe_source_format("https://anti-ad.net/domains.txt") == SourceFormat::HOSTS);
    assert(std::string(source_format_name(SourceFormat::HOSTS)) == "hosts");
    assert(superseded_default_rule_sources_text() ==
           "# 每行一个规则源 URL,支持 Hosts / AdGuard / gzip(.gz)\n"
           "# 以下为默认源,可自行增删\n"
           "https://anti-ad.net/easylist.txt\n"
           "https://adguardteam.github.io/AdGuardSDNSFilter/Filters/filter.txt\n");
    assert(should_refresh_default_rule_sources(superseded_default_rule_sources_text()));
    assert(should_refresh_default_rule_sources(legacy_default_rule_sources_text_v1()));
    assert(should_refresh_default_rule_sources(legacy_default_rule_sources_text_v1b()));
    assert(!should_refresh_default_rule_sources(default_rule_sources_text()));
    assert(!should_refresh_default_rule_sources("# my preferences\n"+default_rule_sources_text()));
    auto migrated=migrate_source_defaults("# mine\n  https://example.org/user  \n# disabled: https://example.org/off\nhttps://adrules.top/adrules.txt\n# disabled: builtin:hagezi-pro-minus-fake\n");
    assert(migrated.find("# mine\n  https://example.org/user  \n# disabled: https://example.org/off\n") == 0);
    assert(migrated.find("https://adrules.top/adrules.txt") == std::string::npos);
    for (const auto &source : default_rule_recipe())
        assert(default_rule_sources_text().find(std::string("# ") + source.name + "\n" + source.url + "\n") != std::string::npos);
    for (const char *url : {
             "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/adblock/light.txt",
             "https://adrules.top/dns.txt",
             "https://raw.githubusercontent.com/bigdargon/hostsVN/master/extensions/threat/filter.txt",
             "https://raw.githubusercontent.com/damengzhu/banad/main/jiekouAD.txt",
             "https://anti-ad.net/domains.txt", "https://big.oisd.nl/domainswild2",
             "https://raw.githubusercontent.com/badmojr/1Hosts/master/Lite/wildcards.txt",
             "https://raw.githubusercontent.com/StevenBlack/hosts/master/hosts"})
        assert(migrated.find(url) != std::string::npos);
    assert(count_of(migrated, std::string(HAGEZI_SOURCE_ID) + "\n") == 1);
    assert(migrated.find(std::string(HAGEZI_SOURCE_ID) + "\n") > migrated.find("https://adrules.top/dns.txt"));
    assert(migrate_source_defaults(migrated) == migrated);
    assert(migrate_source_defaults("# disabled: https://adrules.top/adrules.txt\n").find("https://adrules.top/adrules.txt") == std::string::npos);
    auto cheng = migrate_source_defaults("# 乘风广告过滤规则\nhttps://raw.githubusercontent.com/xinggsf/Adblock-Plus-Rule/master/rule.txt\n");
    assert(cheng.find("# 乘风 视频过滤规则\nhttps://raw.githubusercontent.com/xinggsf/Adblock-Plus-Rule/master/mv.txt\n") != std::string::npos);
    assert(cheng.find("master/rule.txt") == std::string::npos);
    auto cheng_disabled = migrate_source_defaults("# disabled: https://raw.githubusercontent.com/xinggsf/Adblock-Plus-Rule/master/rule.txt\n");
    assert(cheng_disabled.find("# disabled: https://raw.githubusercontent.com/xinggsf/Adblock-Plus-Rule/master/mv.txt\n") != std::string::npos);
    // A custom list's explicit opt-outs survive the v2 migration.
    assert(migrated.find("# disabled: builtin:hagezi-pro-minus-fake\n") != std::string::npos);
    auto disabled_new = migrate_source_defaults("# disabled: https://adrules.top/dns.txt\n# disabled: " + std::string(HAGEZI_SOURCE_ID) + "\n");
    assert(count_of(disabled_new, "https://adrules.top/dns.txt\n") == 1);
    assert(disabled_new.find("# disabled: https://adrules.top/dns.txt\n") != std::string::npos);
    auto disabled_old = migrate_source_defaults("# disabled: https://adrules.top/adrules.txt\n");
    assert(disabled_old.find("# disabled: https://adrules.top/dns.txt\n") != std::string::npos);
    auto collision = migrate_source_defaults("https://raw.githubusercontent.com/xinggsf/Adblock-Plus-Rule/master/rule.txt\n# disabled: https://raw.githubusercontent.com/xinggsf/Adblock-Plus-Rule/master/mv.txt\nhttps://adrules.top/adrules.txt\n# disabled: https://adrules.top/dns.txt\n");
    assert(parse_rule_sources(collision, entries, error, true));
    assert(count_of(collision, "master/mv.txt\n") == 1 && count_of(collision, "https://adrules.top/dns.txt\n") == 1);
    assert(collision.find("# disabled: https://adrules.top/dns.txt\n") != std::string::npos);
    assert(collision.find("# disabled: https://raw.githubusercontent.com/xinggsf/Adblock-Plus-Rule/master/mv.txt\n") != std::string::npos);
    auto no_newline = migrate_source_defaults("# custom heading\nhttps://example.org/no-newline");
    assert(no_newline.find("https://example.org/no-newline\n") != std::string::npos);
    assert(parse_rule_sources(no_newline, entries, error, true));
    std::string full;
    for (size_t i = 0; i < MAX_RULE_SOURCES; ++i) full += "https://example.org/" + std::to_string(i) + "\n";
    auto full_migration = migrate_source_defaults(full);
    assert(parse_rule_sources(full_migration, entries, error, true) && entries.size() == MAX_RULE_SOURCES + 1);
    assert(!entries.back().enabled && entries.back().url == HAGEZI_SOURCE_ID);
    assert(full_migration.find("https://adrules.top/dns.txt") == std::string::npos);
    const std::string ending = "\nhttps://example.org/near-byte-limit\n";
    auto near_limit = std::string("#") + std::string(MAX_SOURCE_CONFIG_BYTES - ending.size() - 1, 'x') + ending;
    auto bounded = migrate_source_defaults(near_limit);
    assert(bounded == near_limit && bounded.size() == MAX_SOURCE_CONFIG_BYTES);
    assert(parse_rule_sources(bounded, entries, error, true));
    assert(!should_refresh_default_rule_sources("# custom\nhttps://example.org/user\n"));
    for (const auto &source : default_rule_recipe()) {
        assert(std::string(source.url).find("hagezi") == std::string::npos ||
               std::string(source.url).find("light") != std::string::npos);
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
    entries[0].enabled=false; previous=entries;
    entries.clear(); retain_builtin_source(entries,previous);
    assert(entries.size()==1 && !entries[0].enabled);
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
