#include "hagezi_source.h"
#include "logging.h"
#include <algorithm>
#include <iterator>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace {
std::vector<std::string> unique_domains(const std::string &text) {
    std::istringstream input(text);
    std::vector<std::string> result;
    std::string line;
    while (std::getline(input, line)) if (!line.empty()) result.push_back(std::move(line));
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}
}

SourceValidationResult merge_hagezi_pair(const std::string &pro_file,
                                        const std::string &fake_file,
                                        const RuleStreamLimits &limits) {
    // Match the original Python recipe: strip *. and emit exact Hosts domains.
    // Never add Fake as allow rules or subtract it from any other source.
    auto pro = validate_rule_source_file(pro_file.c_str(), limits, SourceFormat::HOSTS_AS_ADGUARD);
    if (!pro.accepted) { pro.reason = "pro: " + pro.reason; return pro; }
    RuleStreamLimits remaining = limits;
    remaining.output_bytes -= pro.stats.bytes;
    remaining.lines -= pro.stats.total_lines;
    auto fake = validate_rule_source_file(fake_file.c_str(), remaining, SourceFormat::HOSTS_AS_ADGUARD);
    if (!fake.accepted) { fake.reason = "fake: " + fake.reason; return fake; }
    auto base = unique_domains(pro.adguard_rules), excluded = unique_domains(fake.adguard_rules);
    std::vector<std::string> kept;
    std::set_difference(base.begin(), base.end(), excluded.begin(), excluded.end(), std::back_inserter(kept));
    SourceValidationResult result;
    result.accepted = true;
    result.reason = "accepted";
    for (const auto &domain : kept) result.hosts_rules += domain + "\n";
    auto &s = result.stats;
    s.bytes = pro.stats.bytes + fake.stats.bytes;
    s.total_lines = pro.stats.total_lines + fake.stats.total_lines;
    s.ignored_lines = pro.stats.ignored_lines + fake.stats.ignored_lines;
    s.candidate_lines = pro.stats.candidate_lines + fake.stats.candidate_lines;
    s.rejected_unsupported = pro.stats.rejected_unsupported + fake.stats.rejected_unsupported;
    s.rejected_too_long = pro.stats.rejected_too_long + fake.stats.rejected_too_long;
    s.accepted_hosts = kept.size();
    s.valid_ratio_ppm = s.candidate_lines ? static_cast<uint32_t>(
        (pro.stats.accepted_adguard + fake.stats.accepted_adguard) * 1000000ull / s.candidate_lines) : 0;
    s.difference_base_unique = base.size();
    s.difference_exclude_unique = excluded.size();
    s.difference_removed = base.size() - kept.size();
    return result;
}

SourceValidationResult fetch_hagezi_source(const std::string &directory,
                                          const RuleStreamLimits &limits,
                                          const RuleSourceDownload &download,
                                          size_t &downloaded_bytes) {
    downloaded_bytes = 0;
    const std::string pro = directory + "/hagezi-pro.part", fake = directory + "/hagezi-fake.part";
    struct Cleanup {
        const std::string &a, &b;
        ~Cleanup() { unlink(a.c_str()); unlink(b.c_str()); }
    } cleanup{pro, fake};
    for (const auto &component : {std::make_pair(HAGEZI_PRO_URL, pro), std::make_pair(HAGEZI_FAKE_URL, fake)}) {
        log_line("HaGeZi", "拉取固定依赖: %s", component.first);
        std::string error = download(component.first, component.second);
        if (!error.empty()) {
            SourceValidationResult failure;
            failure.reason = std::string(component.first == HAGEZI_PRO_URL ? "pro_download: " : "fake_download: ") + error;
            log_line("HaGeZi", "整组拒绝，不使用未完成的 Pro − Fake：%s", failure.reason.c_str());
            return failure;
        }
        struct stat info {};
        if (stat(component.second.c_str(), &info) == 0 && info.st_size > 0)
            downloaded_bytes += static_cast<size_t>(info.st_size);
    }
    auto result = merge_hagezi_pair(pro, fake, limits);
    if (result.accepted)
        log_line("HaGeZi", "Pro 唯一 %zu / Fake 唯一 %zu / 差集剔除 %zu / 保留 %zu；仅本组精确 Hosts 规则",
                 result.stats.difference_base_unique, result.stats.difference_exclude_unique,
                 result.stats.difference_removed, result.stats.accepted_hosts);
    else log_line("HaGeZi", "整组校验失败：%s", result.reason.c_str());
    return result;
}
