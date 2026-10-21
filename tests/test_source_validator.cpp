#include "source_validator.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>
#include <zlib.h>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

}  // namespace

int main() {
    std::string mixed =
        "! title\n"
        "# hosts comment\n"
        "0.0.0.0 ads.example.com\n"
        "tracker.example.net\n"
        "||telemetry.example.org^\n"
        "@@||allow.example.org^\n"
        "https://not-a-domain.example/path\n"
        "example.com##.banner\n";
    SourceValidationResult valid = validate_rule_source(mixed);
    expect(valid.accepted, "mixed domain-level source should be accepted");
    expect(valid.stats.accepted_hosts == 2 && valid.stats.accepted_adguard == 2,
           "source should classify hosts and AdGuard rules with engine semantics");
    expect(valid.stats.ignored_lines == 2 &&
               valid.stats.rejected_unsupported == 2,
           "comments and unsupported request/cosmetic rules should be counted separately");
    expect(valid.hosts_rules.find("ads.example.com") != std::string::npos &&
               valid.adguard_rules.find("telemetry.example.org") != std::string::npos,
           "only accepted rules should be emitted for aggregation");

    SourceValidationResult html = validate_rule_source(
        "<!DOCTYPE html><html><body>0.0.0.0 injected.example</body></html>");
    expect(!html.accepted && html.reason == "html_content" &&
               html.hosts_rules.empty() && html.adguard_rules.empty(),
           "HTTP 200 HTML error page must never become rules");

    SourceValidationResult json_error = validate_rule_source(
        "{\"error\":\"rate limited\",\"status\":200}\n");
    expect(!json_error.accepted && json_error.reason == "no_valid_rules",
           "non-HTML error payload without rules must be rejected");

    std::string low_ratio = "one.valid.example\n";
    for (int i = 0; i < 30; i++)
        low_ratio += "https://error.example/path/" + std::to_string(i) + "\n";
    SourceValidationResult noisy = validate_rule_source(low_ratio);
    expect(!noisy.accepted && noisy.reason == "valid_ratio_below_5_percent",
           "large source with very low valid ratio must be rejected");
    expect(noisy.hosts_rules.empty() && noisy.adguard_rules.empty(),
           "rejected low-ratio source must not leak its valid prefix into aggregation");

    SourceValidationResult small = validate_rule_source("small.example\n");
    expect(small.accepted && small.stats.valid_ratio_ppm == 1000000,
           "small valid custom source should not require an arbitrary minimum line count");
    auto typed = validate_rule_source(low_ratio, SourceFormat::ADGUARD);
    expect(typed.accepted && typed.stats.accepted_hosts == 0 && typed.stats.accepted_adguard == 1,
           "explicit browser lists must extract supported domains even below five percent");
    auto typed_html = validate_rule_source("<html><body>error</body></html>\n||ads.example^\n",
                                           SourceFormat::ADGUARD);
    expect(!typed_html.accepted && typed_html.reason == "html_content",
           "explicit formats must retain HTML rejection");
    auto typed_hosts = validate_rule_source("0.0.0.0 Ads.Example\n*.wild.example\n",
                                            SourceFormat::HOSTS_AS_ADGUARD);
    expect(typed_hosts.accepted && typed_hosts.stats.accepted_hosts == 0 &&
               typed_hosts.adguard_rules == "ads.example\nwild.example\n",
           "recipe Hosts sources must become exact rules in the AdGuard category");

    SourceValidationResult masks = validate_rule_source(
        "||valid.example^\n||adservice.google.\n||adserver1.\n@@||advertising.civitai.\n");
    expect(masks.accepted && masks.stats.accepted_adguard == 1 &&
               masks.stats.rejected_unsupported == 3,
           "hostname-prefix masks must be counted as unsupported, not domain rules");

    std::string overlong(1025, 'a');
    SourceValidationResult too_long = validate_rule_source(overlong + ".example\n");
    expect(!too_long.accepted && too_long.stats.rejected_too_long == 1,
           "overlong source line should be rejected and counted");

    char gzip_path[] = "/tmp/ad_killer_by_nfqueue_source.XXXXXX";
    int gzip_fd = mkstemp(gzip_path);
    expect(gzip_fd >= 0, "temporary source path should be created");
    if (gzip_fd >= 0) {
        close(gzip_fd);
        gzFile gzip = gzopen(gzip_path, "wb9");
        std::string gzip_content = "0.0.0.0 streamed.example\n||track.example^\n";
        expect(gzip && gzwrite(gzip, gzip_content.data(),
                               static_cast<unsigned>(gzip_content.size())) ==
                           static_cast<int>(gzip_content.size()) &&
                   gzclose(gzip) == Z_OK,
               "streaming validator gzip fixture should be written");
        SourceValidationResult streamed = validate_rule_source_file(gzip_path);
        expect(streamed.accepted && streamed.stats.accepted_hosts == 1 &&
                   streamed.stats.accepted_adguard == 1,
               "streaming source validation should preserve classification semantics");
        unlink(gzip_path);
    }

    SourceUpdateReport accepted;
    accepted.url = "https://example.com/list";
    accepted.status = "accepted";
    accepted.reason = "accepted";
    accepted.downloaded_bytes = 123;
    accepted.checked_at = 123456700;
    accepted.stats = valid.stats;
    SourceUpdateReport rejected;
    rejected.url = "https://example.com/error\nforged=1";
    rejected.status = "content_rejected";
    rejected.reason = "html_content\nstatus=accepted";
    std::string report = serialize_source_reports(
        {accepted, rejected}, "published", 123456789, {}, "123-456");
    expect(report.find("checked_at=123456700\n") != std::string::npos &&
               report.find("checked_at=123456789\n") != std::string::npos &&
               report.find("job_id=123-456\n") != std::string::npos,
           "per-source result timestamps and job identity must be persisted");
    expect(report.find("source_count=2") != std::string::npos &&
               report.find("accepted_sources=1") != std::string::npos &&
               report.find("valid_ratio_ppm=") != std::string::npos,
           "source report should persist aggregate and per-source validation statistics");
    expect(report.find("error\nforged") == std::string::npos &&
               report.find("html_content\nstatus") == std::string::npos,
           "source report fields must not permit newline injection");
    accepted.stats.accepted_hosts = 3;
    accepted.stats.accepted_adguard = 2;
    // A rejected source can contain a valid prefix, but none of its lines
    // reached the published bundle and must not inflate duplicate counts.
    rejected.stats.accepted_hosts = 500;
    report = serialize_source_reports({accepted, rejected}, "published", 123,
                                      {"generation-1", 2, 1});
    expect(report.find("accepted_lines=5\n") != std::string::npos &&
               report.find("unique_total=3\n") != std::string::npos &&
               report.find("duplicate_lines=2\n") != std::string::npos,
           "published summary must deduplicate only lines from accepted sources");

    if (failures != 0) {
        std::cerr << failures << " source validator test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "source validator tests passed\n";
    return EXIT_SUCCESS;
}
