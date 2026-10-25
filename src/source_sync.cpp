#include "source_sync.h"
#include "hagezi_source.h"
#include "net.h"
#include "logging.h"
#include "task_context.h"
#include "work_budget.h"

#include <algorithm>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

std::vector<std::string> source_component_urls(const std::string &url) {
    if (is_builtin_source(url)) return {HAGEZI_PRO_URL, HAGEZI_FAKE_URL};
    return {url};
}
std::string source_identity_key(const std::string &url) {
    return source_cache_key(url, is_builtin_source(url) ? "hagezi-pro-minus-fake" : source_format_name(recipe_source_format(url)), source_component_urls(url));
}

SourceSyncResult synchronize_rule_source(const std::string &directory, const std::string &url,
                                         const RuleStreamLimits &limits, SourceCache *cache,
                                         const std::set<std::string> &protected_keys) {
    SourceSyncResult result;
    const auto urls = source_component_urls(url);
    const auto key = source_identity_key(url);
    CachedSource previous;
    std::string cache_error;
    const bool cached = cache && cache->available() && cache->load(key, urls, time(nullptr), previous, cache_error);
    std::vector<std::string> parts;
    for (size_t i = 0; i < urls.size(); ++i) parts.push_back(directory + "/source-download-" + std::to_string(i) + ".part");
    struct Cleanup {
        const std::vector<std::string> &parts;
        ~Cleanup() { if (!task_unconfirmed_network_writer) for (const auto &path : parts) unlink(path.c_str()); }
    } cleanup {parts};
    std::vector<CachedComponent> components;
    std::string fresh_error;
    bool revalidated = false;
    {
        // Keep part of the source deadline for local fallback validation. An
        // inner network timeout must not expire the enclosing cache budget.
        const auto remaining = WorkBudget::remaining_ms(90000);
        WorkBudget network(cache ? std::min<uint64_t>(70000, remaining * 4 / 5) : remaining, UINT64_MAX);
        for (size_t i = 0; i < urls.size(); ++i) {
            log_line(is_builtin_source(url) ? "HaGeZi" : "更新", "拉取: %s", urls[i].c_str());
            HttpValidators validators = cached ? previous.components[i].validators : HttpValidators{};
            auto response = download_file_response(urls[i], parts[i], validators);
            result.downloaded_bytes += response.bytes;
            if (response.error.empty() && response.not_modified && cached &&
                response.effective_url != previous.components[i].effective_url) {
                log_line("更新", "304 最终地址与缓存不匹配，预算内重新完整下载: %s", urls[i].c_str());
                response = download_file_response(urls[i], parts[i]);
                result.downloaded_bytes += response.bytes;
            }
            if (!response.error.empty()) { fresh_error = response.error; result.download_failed = true; break; }
            CachedComponent component;
            if (response.not_modified) {
                if (!cached) { fresh_error = "304_without_verified_cache"; result.download_failed = true; break; }
                component = previous.components[i];
                if (!response.validators.etag.empty()) component.validators.etag = response.validators.etag;
                if (!response.validators.last_modified.empty()) component.validators.last_modified = response.validators.last_modified;
                revalidated = true;
            } else {
                component.path = parts[i]; component.url = urls[i]; component.effective_url = response.effective_url;
                component.bytes = response.bytes; component.validators = response.validators; component.fetched_at = time(nullptr);
                // response.bytes includes retry bodies; the cached file is the
                // final successful representation, not the request's total.
                struct stat info {};
                if (stat(component.path.c_str(), &info) || info.st_size <= 0) { fresh_error = "downloaded_file_unavailable"; result.download_failed = true; break; }
                component.bytes = static_cast<uint64_t>(info.st_size);
            }
            components.push_back(std::move(component));
        }
    }
    const auto validate = [&](const std::vector<CachedComponent> &selected) {
        RuleStreamLimits remaining = limits;
        remaining.output_bytes -= std::min(remaining.output_bytes, result.consumed_bytes);
        remaining.lines -= std::min(remaining.lines, result.consumed_lines);
        auto value = is_builtin_source(url) ? merge_hagezi_pair(selected[0].path, selected[1].path, remaining) :
            validate_rule_source_file(selected[0].path.c_str(), remaining, recipe_source_format(url));
        result.consumed_bytes += value.stats.bytes;
        result.consumed_lines += value.stats.total_lines;
        if (value.accepted && cache && cache->available()) for (const auto &component : selected) {
            if (!source_component_intact(component)) {
                value.accepted = false; value.reason = "source_digest_changed";
                value.hosts_rules.clear(); value.adguard_rules.clear(); break;
            }
        }
        return value;
    };
    if (fresh_error.empty() && cache && cache->available()) for (auto &component : components) {
        if (component.digest.empty() && !hash_source_component(component)) { fresh_error = "source_digest_unavailable"; break; }
    }
    if (fresh_error.empty() && components.size() == urls.size() && !WorkBudget::expired() &&
        !operation_closing() && !task_unconfirmed_network_writer && task_progress("validating")) {
        result.validation = validate(components);
        if (!result.validation.accepted) fresh_error = result.validation.reason;
    } else if (fresh_error.empty()) fresh_error = "source_budget_or_cancelled";

    if (fresh_error.empty() && result.validation.accepted) {
        result.provenance = revalidated ? "revalidated" : "downloaded";
        if (revalidated) {
            result.cache_verified_at = time(nullptr);
            result.cache_fetched_at = components.front().fetched_at;
            for (const auto &component : components) result.cache_fetched_at = std::min(result.cache_fetched_at, component.fetched_at);
        }
        if (cache && cache->available() && !cache->commit(key, components, time(nullptr), protected_keys, cache_error))
            log_line("缓存", "来源已校验，但缓存未能确认保存: %s (%s)", url.c_str(), cache_error.c_str());
    } else {
        result.validation.accepted = false;
        result.validation.reason = fresh_error;
        if (cached && !previous.fresh(time(nullptr), cache->policy().fallback_seconds)) result.validation.reason += "; cache_expired";
        if (cached && previous.fresh(time(nullptr), cache->policy().fallback_seconds) && !WorkBudget::expired() &&
            !operation_closing() && !task_unconfirmed_network_writer && task_progress("validating")) {
            auto fallback = validate(previous.components);
            if (fallback.accepted) {
                result.validation = std::move(fallback);
                result.provenance = "cache_fallback";
                result.warning = fresh_error;
                result.cache_verified_at = previous.verified_at;
                result.cache_age_seconds = time(nullptr) - previous.verified_at;
                result.cache_fetched_at = previous.components.front().fetched_at;
                for (const auto &component : previous.components) result.cache_fetched_at = std::min(result.cache_fetched_at, component.fetched_at);
                log_line("缓存", "来源失败，整组复用 %lld 秒前已核验的缓存: %s (%s)",
                    static_cast<long long>(result.cache_age_seconds), url.c_str(), fresh_error.c_str());
            } else result.validation.reason = fresh_error + "; cached_validation: " + fallback.reason;
        }
    }
    if (result.validation.accepted && is_builtin_source(url)) {
        const auto &stats = result.validation.stats;
        log_line("HaGeZi", "Pro 唯一 %zu / Fake 唯一 %zu / 差集剔除 %zu / 保留 %zu；仅本组精确 Hosts 规则",
            stats.difference_base_unique, stats.difference_exclude_unique, stats.difference_removed, stats.accepted_hosts);
    }
    return result;
}
