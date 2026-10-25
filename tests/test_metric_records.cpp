#include "metrics.h"
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
int main() {
    const ProcessIdentity live {123, "12345678-1234-1234-1234-123456789abc", 100};
    MetricsSnapshot sample;
    sample.owner = live; sample.updated_at = 200; sample.observed_monotonic_ms = 100000;
    sample.block_exact_hits = 9; sample.dns_drop_verdicts = 4;
    const auto encoded = serialize_metrics(sample);
    MetricsSnapshot parsed;
    assert(parse_metrics_checked(encoded, parsed) && parsed.owner == live && parsed.observed_monotonic_ms == 100000);
    assert(parsed.block_exact_hits == 9 && parsed.dns_drop_verdicts == 4);
    assert(observe_metrics(parsed, live, live, true, 145000).status == "current");
    const auto stale = observe_metrics(parsed, live, live, true, 145001);
    assert(stale.status == "stale" && stale.owner_verified && stale.age_known && stale.age_ms == 45001);
    auto other = live; ++other.started;
    assert(!observe_metrics(parsed, other, other, true, 100001).owner_verified);
    assert(!observe_metrics(parsed, live, other, true, 100001).owner_verified);
    other = live; other.boot = "ffffffff-ffff-ffff-ffff-ffffffffffff";
    assert(!observe_metrics(parsed, other, other, true, 100001).owner_verified);
    assert(observe_metrics(parsed, live, live, false, 100001).status == "inactive");
    assert(observe_metrics(parsed, live, live, true, 99999).status == "clock_invalid");
    const auto json = metrics_view_json(parsed, stale);
    assert(json.find("\"owner_verified\":true") != std::string::npos && json.find("\"age_ms\":45001") != std::string::npos);
    for (const auto &extra : {"block_exact_hits=99\n", "owner_pid=1\n", "dns_packets=-1\n"})
        assert(!parse_metrics_checked(encoded + extra, parsed) && parsed.block_exact_hits == 0);
    const auto changed = [&](const std::string &key, const std::string &value) {
        auto text = encoded; const auto at = text.find(key + '='); assert(at != std::string::npos);
        text.replace(at, text.find('\n', at) - at, key + '=' + value); return text;
    };
    for (const auto &text : {changed("dns_packets", "-1"), changed("dns_packets", "18446744073709551616"),
                            changed("owner_boot", "bad"), changed("metrics_format", "3"),
                            encoded.substr(0, encoded.find("observed_monotonic_ms=")), std::string(16385, 'x')})
        assert(!parse_metrics_checked(text, parsed));
    assert(parse_metrics_checked(encoded + "future_field=ignored\n", parsed));
    assert(parse_metrics_checked("updated_at=1\nblock_exact_hits=9\n", parsed));
    assert(!observe_metrics(parsed, live, live, true, 100001).owner_verified); // legacy is not current-session proof

    MetricsPublisher publisher;
    std::string emitted;
    const auto store = [&](const std::string &text) { emitted = text; return true; };
    assert(publisher.publish(sample, 1, 200000, store) == MetricsPublication::SAVED);
    assert(parse_metrics_checked(emitted, parsed) && parsed.observed_monotonic_ms == 200000);
    assert(publisher.publish(sample, 999, 201000, store) == MetricsPublication::SKIPPED);
    sample.owner = other;
    assert(publisher.publish(sample, 999, 201000, store) == MetricsPublication::SAVED); // new owner never inherits an idle checkpoint

    char pattern[] = "/tmp/adk_metric_records.XXXXXX";
    const char *created = mkdtemp(pattern); assert(created);
    const fs::path base(created), path = base / "metrics.conf";
    assert(read_metrics(path, parsed) == MetricsRead::MISSING);
    std::ofstream(path) << encoded;
    assert(chmod(path.c_str(), 0600) == 0);
    assert(read_metrics(path, parsed) == MetricsRead::OK && parsed.owner == live);
    fs::create_symlink(path, base / "alias");
    assert(read_metrics(base / "alias", parsed) == MetricsRead::UNREADABLE && parsed.block_exact_hits == 0);
    fs::create_hard_link(path, base / "hardlink");
    assert(read_metrics(path, parsed) == MetricsRead::UNREADABLE);
    fs::remove(base / "hardlink");
    assert(mkfifo((base / "fifo").c_str(), 0600) == 0);
    assert(read_metrics(base / "fifo", parsed) == MetricsRead::UNREADABLE);
    assert(chmod(path.c_str(), 0666) == 0 && read_metrics(path, parsed) == MetricsRead::UNREADABLE);
    assert(chmod(path.c_str(), 0600) == 0);
    std::ofstream(path) << "dns_packets=-1\n";
    assert(read_metrics(path, parsed) == MetricsRead::INVALID);
    fs::remove_all(base);
    std::cout << "metric records: strict numbers/schema, bounded trusted files, session/boot/PID reuse, clock age and idle publication passed\n";
}
