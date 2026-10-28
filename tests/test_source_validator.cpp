#include "source_validator.h"
#include "logging.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <set>
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

std::string decode_gzip(const std::string &encoded, size_t expected_size) {
    std::string decoded(expected_size + 1, '\0');
    z_stream stream{};
    expect(inflateInit2(&stream, 15 + 16) == Z_OK, "gzip oracle should initialize");
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(encoded.data()));
    stream.avail_in = static_cast<uInt>(encoded.size());
    stream.next_out = reinterpret_cast<Bytef *>(decoded.data());
    stream.avail_out = static_cast<uInt>(decoded.size());
    const auto result = inflate(&stream, Z_FINISH);
    expect(result == Z_STREAM_END && stream.total_in == encoded.size(), "gzip must be a complete single member");
    decoded.resize(stream.total_out);
    inflateEnd(&stream);
    return decoded;
}

void check_chunks(std::vector<std::string> inputs) {
    std::string aggregate;
    std::set<std::string> oracle;
    CanonicalRuleChunks chunks;
    for (auto &input : inputs) {
        aggregate += input;
        for (size_t at = 0; at < input.size();) {
            const auto end = input.find('\n', at);
            if (end != at) oracle.insert(input.substr(at, end - at));
            at = end + 1;
        }
        expect(chunks.append(std::move(input)), "line-aligned validated chunks must be admitted");
    }
    const std::string header = "# fixed rule header\n";
    std::string expected = header;
    for (const auto &line : oracle) expected += line + '\n';
    deduplicate_rule_text(aggregate);
    std::string previous, actual;
    expect(gz_compress_parts({header, aggregate}, previous), "previous complete-text gzip must succeed");
    expect(chunks.compress_gzip(header, actual), "view-sorted gzip must succeed");
    expect(actual == previous, "chunk sorting must preserve complete gzip bytes");
    expect(decode_gzip(actual, expected.size()) == expected, "independent full ordered set must match decoded gzip");
    expect(chunks.empty() && chunks.size() == 0, "consumed source buffers must be released after compression");
}

void chunk_tests() {
    check_chunks({});
    check_chunks({"", "\n\n"});
    check_chunks({"z\nb\n", "\na\nb\n", "a\r\n", std::string("\0x\n", 3)});
    check_chunks({std::string(131071, 'x') + "\n", "a\n"});
    std::vector<std::string> inputs(17);
    for (unsigned n = 0; n < 30000; ++n) {
        inputs[n % inputs.size()] += (n % 31 == 0 ? "@@" : "") + std::string(n % 200, 'a' + n % 23) +
                                    std::to_string(n % 8000) + ".example\n";
    }
    check_chunks(std::move(inputs));
    CanonicalRuleChunks bounded(5, 2);
    expect(bounded.append("a\n") && bounded.append("b\n"), "chunks may fill line budget exactly");
    std::string partial = "z", too_many = "\n", oversized = "zz\n";
    expect(!bounded.append(std::move(partial)) && partial == "z", "partial lines must fail without consuming input");
    expect(!bounded.append(std::move(too_many)) && too_many == "\n", "line overflow must preserve the prior chunks");
    expect(!bounded.append(std::move(oversized)) && oversized == "zz\n", "byte overflow must preserve the prior chunks");
    expect(bounded.size() == 4, "refused chunks must not alter accounting");
    std::string encoded;
    expect(bounded.compress_gzip("", encoded) && decode_gzip(encoded, 4) == "a\nb\n", "failed admission cannot publish partial bytes");
    expect(bounded.append("c\n") && bounded.compress_gzip("", encoded) && decode_gzip(encoded, 2) == "c\n", "compression must reset both byte and line budgets");
}

}  // namespace

int main() {
    chunk_tests();
    for (const auto &item : std::vector<std::pair<std::string, std::string>>{
             {"", ""}, {"\n\n", ""}, {"z\nb\na", "a\nb\nz\n"},
             {"long-before-short\na\nlong-before-short\n\n", "a\nlong-before-short\n"},
             {"a\r\na\n", "a\na\r\n"},
             {std::string("z\n\0x\nz", 6), std::string("\0x\nz\n", 5)}}) {
        auto text = item.first;
        deduplicate_rule_text(text);
        expect(text == item.second, "dedup must retain byte semantics, final newline and reordered view safety");
    }
    std::string unordered;
    std::set<std::string> oracle;
    for (unsigned index = 0; index < 10000; ++index) {
        auto line = std::string(index % 200, 'a' + index % 23) + std::to_string(index % 129);
        unordered += line + '\n'; oracle.insert(line);
    }
    std::string expected;
    for (const auto &line : oracle) expected += line + '\n';
    deduplicate_rule_text(unordered);
    expect(unordered == expected, "dedup must equal the complete independent ordered set");

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
