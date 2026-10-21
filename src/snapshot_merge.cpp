#include "snapshot_merge.h"
#include "source_validator.h"
#include "source_config.h"
#include "rules.h"
#include "logging.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

int merge_rule_snapshot(const std::string &input, const std::string &output) {
    if (input == output || output.empty()) return 1;
    struct stat info {};
    if (stat(input.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) return 1;
    // Output must be a new directory: never overwrite a user's files.
    if (mkdir(output.c_str(), 0700) != 0) {
        fprintf(stderr, "输出必须是尚不存在的目录\n");
        return 1;
    }
    configure_process_log((output + "/merge.log").c_str(), 128u * 1024u, 0);
    std::string manifest;
    std::string manifest_path = input + "/sources.tsv";
    if (stat(manifest_path.c_str(), &info) != 0 || info.st_size < 0 ||
        static_cast<size_t>(info.st_size) > MAX_SOURCE_CONFIG_BYTES ||
        !read_file(manifest_path.c_str(), manifest) ||
        manifest.size() > MAX_SOURCE_CONFIG_BYTES) return 1;
    std::istringstream entries(manifest);
    std::string line, hosts, adguard;
    std::vector<SourceUpdateReport> reports;
    size_t content_bytes = 0, source_lines = 0;
    while (std::getline(entries, line)) {
        if (line.empty() || line[0] == '#') continue;
        if (reports.size() >= MAX_RULE_SOURCES) return 1;
        size_t first = line.find('\t'), second = line.find('\t', first == std::string::npos ? 0 : first + 1);
        if (first == std::string::npos || second == std::string::npos) return 1;
        std::string format_name = line.substr(0, first);
        std::string file = line.substr(first + 1, second - first - 1);
        std::string url = line.substr(second + 1);
        SourceFormat format = SourceFormat::AUTO;
        if (format_name == "adguard") format = SourceFormat::ADGUARD;
        else if (format_name == "hosts-as-adguard") format = SourceFormat::HOSTS_AS_ADGUARD;
        else if (format_name != "auto") return 1;
        if (!validate_download_url(url) || file.empty() || file.find("..") != std::string::npos ||
            file.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos) return 1;
        SourceUpdateReport report;
        report.url = url;
        report.format = format_name;
        std::string path = input + "/" + file;
        if (file == "-") {
            report.status = "download_error";
            report.reason = "snapshot_download_failed";
        } else {
            RuleStreamLimits limits;
            limits.output_bytes -= content_bytes;
            limits.lines -= source_lines;
            auto validation = validate_rule_source_file(path.c_str(), limits, format);
            report.stats = validation.stats;
            report.status = validation.accepted ? "accepted" : "content_rejected";
            report.reason = validation.reason;
            if (stat(path.c_str(), &info) == 0) report.downloaded_bytes = static_cast<size_t>(info.st_size);
            size_t generated = validation.hosts_rules.size() + validation.adguard_rules.size();
            size_t existing = hosts.size() + adguard.size();
            if (validation.accepted && generated > MAX_RULE_DECOMPRESSED_BYTES - existing) {
                report.status = "content_rejected";
                report.reason = "aggregate_resource_limit";
            } else if (validation.accepted) {
                hosts += validation.hosts_rules;
                adguard += validation.adguard_rules;
                content_bytes += validation.stats.bytes;
                source_lines += validation.stats.total_lines;
            }
        }
        reports.push_back(report);
    }
    if (hosts.empty() && adguard.empty()) return 1;
    deduplicate_rule_text(hosts);
    deduplicate_rule_text(adguard);
    std::string hosts_file = "# AD_Killer_By_NFQUEUE generated Hosts rules\n" + hosts;
    std::string adguard_file = "# AD_Killer_By_NFQUEUE generated AdGuard rules\n" + adguard;
    std::string gz_hosts, gz_adguard;
    if (!gz_compress_string(hosts_file, gz_hosts) || !gz_compress_string(adguard_file, gz_adguard) ||
        !write_file_atomic((output + "/hosts_rules.txt").c_str(), hosts_file) ||
        !write_file_atomic((output + "/adguard_rules.txt").c_str(), adguard_file) ||
        !write_file_atomic((output + "/hosts_rules.gz").c_str(), gz_hosts) ||
        !write_file_atomic((output + "/adguard_rules.gz").c_str(), gz_adguard)) return 1;
    RuleDB db;
    if (!load_rule_file_checked((output + "/hosts_rules.gz").c_str(), false, db, nullptr, true) ||
        !load_rule_file_checked((output + "/adguard_rules.gz").c_str(), true, db, nullptr, true)) return 1;
    PublishedSourceStats published{"snapshot", db.black.size(), db.white.size()};
    if (!write_file_atomic((output + "/source_stats.conf").c_str(),
                           serialize_source_reports(reports, "snapshot_only", 0, published))) return 1;
    std::string stats = "mode=2\nblack=" + std::to_string(db.black.size()) +
                        "\nwhite=" + std::to_string(db.white.size()) + "\nupdated_at=0\n";
    if (!write_file_atomic((output + "/rule_stats.conf").c_str(), stats)) return 1;
    printf("{\"status\":\"snapshot_only\",\"black\":%zu,\"white\":%zu,\"total\":%zu}\n",
           db.black.size(), db.white.size(), db.black.size() + db.white.size());
    return 0;
}
