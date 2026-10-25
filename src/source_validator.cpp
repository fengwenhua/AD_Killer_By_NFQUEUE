#include "source_validator.h"

#include "rule_stream.h"
#include "rules.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

namespace {

constexpr uint32_t MIN_VALID_RATIO_PPM = 50000;  // 5%
constexpr size_t RATIO_THRESHOLD_LINES = 20;

std::string lower_prefix(const std::string &content) {
    size_t length = std::min<size_t>(content.size(), 8192);
    std::string prefix = content.substr(0, length);
    for (char &ch : prefix) {
        unsigned char value = static_cast<unsigned char>(ch);
        if (value >= 'A' && value <= 'Z') ch = static_cast<char>(value + 32);
    }
    return prefix;
}

bool looks_like_html(const std::string &content) {
    std::string prefix = lower_prefix(content);
    size_t first = prefix.find_first_not_of(" \t\r\n");
    if (first != std::string::npos && prefix.compare(first, 3, "\xef\xbb\xbf") == 0)
        first = prefix.find_first_not_of(" \t\r\n", first + 3);
    if (first != std::string::npos &&
        (prefix.compare(first, 9, "<!doctype") == 0 ||
         prefix.compare(first, 5, "<html") == 0))
        return true;
    return prefix.find("<head") != std::string::npos ||
           prefix.find("<body") != std::string::npos ||
           prefix.find("</html>") != std::string::npos;
}

std::string trim(const std::string &line) {
    size_t begin = line.find_first_not_of(" \t\r");
    if (begin == std::string::npos) return "";
    size_t end = line.find_last_not_of(" \t\r");
    return line.substr(begin, end - begin + 1);
}

std::string safe_field(std::string value) {
    for (char &ch : value) {
        if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
    }
    return value;
}

class ValidationBuilder {
public:
    explicit ValidationBuilder(SourceFormat format) : format_(format) {}
    void add_line(const std::string &raw, bool overlong) {
        result_.stats.total_lines++;
        if (prefix_.size() < 8192) {
            size_t available = 8192 - prefix_.size();
            prefix_.append(raw, 0, std::min(available, raw.size()));
            if (prefix_.size() < 8192) prefix_.push_back('\n');
        }
        std::string line = trim(raw);
        if (line.empty() || line[0] == '#' || line[0] == '!' || line[0] == '[') {
            result_.stats.ignored_lines++;
            return;
        }
        result_.stats.candidate_lines++;
        if (overlong || line.size() > MAX_RULE_LINE_BYTES) {
            result_.stats.rejected_too_long++;
            return;
        }
        if (format_ != SourceFormat::HOSTS_AS_ADGUARD &&
            (line.find("##") != std::string::npos ||
            line.find("#@#") != std::string::npos ||
            line.find("#$#") != std::string::npos ||
            line.find("#%#") != std::string::npos ||
            line.find("#?#") != std::string::npos)) {
            result_.stats.rejected_unsupported++;
            return;
        }

        ParsedDomainRule parsed;
        if (format_ == SourceFormat::AUTO && parse_hosts_rule(line, parsed)) {
            result_.hosts_rules += parsed.canonical() + "\n";
            result_.stats.accepted_hosts++;
            return;
        }
        bool valid = format_ == SourceFormat::HOSTS_AS_ADGUARD
                         ? parse_recipe_hosts_rule(line, parsed)
                         : parse_adguard_rule(line, parsed);
        if (valid) {
            result_.adguard_rules += parsed.canonical() + "\n";
            result_.stats.accepted_adguard++;
            return;
        }
        result_.stats.rejected_unsupported++;
    }

    SourceValidationResult finish(size_t bytes) {
        result_.stats.bytes = bytes;
        if (bytes == 0) {
            result_.reason = "empty_content";
            return std::move(result_);
        }
        if (looks_like_html(prefix_)) {
            result_.reason = "html_content";
            result_.hosts_rules.clear();
            result_.adguard_rules.clear();
            return std::move(result_);
        }
        size_t accepted = result_.stats.accepted_hosts +
                          result_.stats.accepted_adguard;
        if (result_.stats.candidate_lines != 0) {
            result_.stats.valid_ratio_ppm = static_cast<uint32_t>(
                accepted * 1000000ull / result_.stats.candidate_lines);
        }
        if (accepted == 0) {
            result_.reason = "no_valid_rules";
            return std::move(result_);
        }
        // Browser lists legitimately contain mostly cosmetic/request rules.
        // An explicit input format opts into extracting the supported subset;
        // AUTO keeps its defensive low-ratio rejection. HTML remains rejected.
        if (format_ == SourceFormat::AUTO &&
            result_.stats.candidate_lines >= RATIO_THRESHOLD_LINES &&
            result_.stats.valid_ratio_ppm < MIN_VALID_RATIO_PPM) {
            result_.reason = "valid_ratio_below_5_percent";
            result_.hosts_rules.clear();
            result_.adguard_rules.clear();
            return std::move(result_);
        }
        result_.accepted = true;
        result_.reason = "accepted";
        return std::move(result_);
    }

private:
    SourceFormat format_;
    SourceValidationResult result_;
    std::string prefix_;
};

}  // namespace

SourceValidationResult validate_rule_source(const std::string &content, SourceFormat format) {
    ValidationBuilder builder(format);
    std::istringstream lines(content);
    std::string raw;
    while (std::getline(lines, raw)) {
        builder.add_line(raw, raw.size() > MAX_RULE_LINE_BYTES);
    }
    return builder.finish(content.size());
}

SourceValidationResult validate_rule_source_file(
    const char *path, const RuleStreamLimits &limits, SourceFormat format) {
    ValidationBuilder builder(format);
    RuleStreamResult stream;
    bool ok = stream_rule_file(path, false,
                               [&](const std::string &line, bool overlong) {
        builder.add_line(line, overlong);
    }, &stream, limits);
    if (!ok) {
        SourceValidationResult result;
        result.stats.bytes = stream.output_bytes;
        result.stats.total_lines = stream.lines;
        result.reason = stream.error.empty() ? "stream_failed" : stream.error;
        return result;
    }
    return builder.finish(stream.output_bytes);
}

void deduplicate_rule_text(std::string &text) {
    std::vector<std::string> lines;
    std::istringstream input(text);
    std::string line;
    while (std::getline(input, line)) if (!line.empty()) lines.push_back(std::move(line));
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    text.clear();
    for (const auto &value : lines) text += value + "\n";
}

std::string serialize_source_reports(const std::vector<SourceUpdateReport> &reports,
                                     const std::string &publish_status,
                                     int64_t updated_at,
                                     const PublishedSourceStats &published,
                                     const std::string &job_id) {
    size_t accepted_sources = 0;
    size_t accepted_lines = 0, filtered_lines = 0, cached_sources = 0, revalidated_sources = 0;
    for (const auto &report : reports) {
        if (report.status == "accepted") {
            accepted_sources++;
            if (report.provenance == "cache_fallback") ++cached_sources;
            if (report.provenance == "revalidated") ++revalidated_sources;
            accepted_lines += report.stats.accepted_hosts + report.stats.accepted_adguard;
        }
        filtered_lines += report.stats.rejected_unsupported + report.stats.rejected_too_long;
    }
    std::ostringstream output;
    output << "updated_at=" << updated_at << "\n"
           << "job_id=" << safe_field(job_id) << "\n"
           << "publish_status=" << safe_field(publish_status) << "\n"
           << "source_count=" << reports.size() << "\n"
           << "accepted_sources=" << accepted_sources << "\n"
           << "cached_sources=" << cached_sources << "\n"
           << "revalidated_sources=" << revalidated_sources << "\n"
           << "rejected_sources=" << reports.size() - accepted_sources << "\n"
           << "accepted_lines=" << accepted_lines << "\n"
           << "filtered_lines=" << filtered_lines << "\n";
    if (!published.generation.empty()) {
        size_t unique = published.unique_black + published.unique_white;
        output << "generation=" << safe_field(published.generation) << "\n"
               << "unique_black=" << published.unique_black << "\n"
               << "unique_white=" << published.unique_white << "\n"
               << "unique_total=" << unique << "\n"
               << "duplicate_lines=" << (accepted_lines >= unique ? accepted_lines - unique : 0) << "\n";
    }
    for (size_t i = 0; i < reports.size(); i++) {
        const auto &report = reports[i];
        output << "\n[source_" << i << "]\n"
               << "url=" << safe_field(report.url) << "\n"
               << "format=" << safe_field(report.format) << "\n"
               << "status=" << safe_field(report.status) << "\n"
               << "checked_at=" << (report.checked_at > 0 ? report.checked_at : updated_at) << "\n"
               << "reason=" << safe_field(report.reason) << "\n"
               << "provenance=" << safe_field(report.provenance) << "\n"
               << "warning=" << safe_field(report.warning) << "\n"
               << "cache_verified_at=" << report.cache_verified_at << "\n"
               << "cache_fetched_at=" << report.cache_fetched_at << "\n"
               << "cache_age_seconds=" << report.cache_age_seconds << "\n"
               << "downloaded_bytes=" << report.downloaded_bytes << "\n"
               << "content_bytes=" << report.stats.bytes << "\n"
               << "total_lines=" << report.stats.total_lines << "\n"
               << "ignored_lines=" << report.stats.ignored_lines << "\n"
               << "candidate_lines=" << report.stats.candidate_lines << "\n"
               << "accepted_hosts=" << report.stats.accepted_hosts << "\n"
               << "accepted_adguard=" << report.stats.accepted_adguard << "\n"
               << "rejected_unsupported=" << report.stats.rejected_unsupported << "\n"
               << "rejected_too_long=" << report.stats.rejected_too_long << "\n"
               << "valid_ratio_ppm=" << report.stats.valid_ratio_ppm << "\n";
        if (report.format == "hagezi-pro-minus-fake")
            output << "difference_base_unique=" << report.stats.difference_base_unique << "\n"
                   << "difference_exclude_unique=" << report.stats.difference_exclude_unique << "\n"
                   << "difference_removed=" << report.stats.difference_removed << "\n";
    }
    return output.str();
}
