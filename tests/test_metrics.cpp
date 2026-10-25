#include "metrics.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

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
    DataPlaneMetrics metrics;
    std::vector<std::thread> writers;
    for (int i = 0; i < 8; ++i) {
        writers.emplace_back([&]() {
            for (int n = 0; n < 1000; ++n) {
                metrics.record_packet((n & 1) == 0);
                metrics.record_match(MetricMatch::BLOCK_SUFFIX);
            }
        });
    }
    for (auto &writer : writers) writer.join();
    metrics.record_transport_failure();
    metrics.record_domain_failure();
    metrics.record_truncated();
    metrics.record_match(MetricMatch::WHITELIST);
    metrics.record_match(MetricMatch::BLOCK_EXACT);
    metrics.record_match(MetricMatch::PASS_DEFAULT);
    metrics.record_blocked_retransmit();
    metrics.record_verdict_failure();
    metrics.record_queue_error();
    metrics.record_stale_verdicts(2);
    metrics.record_dns_negative(); metrics.record_tcp_reset(); metrics.record_quic_reject(); metrics.record_response_failure();

    MetricsSnapshot snapshot = metrics.snapshot();
    expect(snapshot.dns_packets == 4000 && snapshot.tls_packets == 4000,
           "packet counters should be atomic across queue threads");
    expect(snapshot.stale_verdicts == 2 && snapshot.dns_negative_replies == 1 &&
               snapshot.tcp_resets == 1 && snapshot.quic_rejects == 1 && snapshot.response_failures == 1,
           "fast response and recoverable error counters must be independent");
    expect(snapshot.block_suffix_hits == 8000 && snapshot.whitelist_hits == 1 &&
               snapshot.block_exact_hits == 1 && snapshot.default_passes == 1,
           "all four rule verdicts should have separate counters");
    expect(snapshot.transport_parse_failures == 1 &&
               snapshot.domain_parse_failures == 1 &&
               snapshot.truncated_packets == 1 &&
               snapshot.blocked_retransmits == 1 &&
               snapshot.verdict_failures == 1 && snapshot.queue_errors == 1,
           "failure counters should remain independent");

    snapshot.updated_at = 123456;
    snapshot.ipv6_fragments = 7;
    snapshot.ipv6_extension_errors = 8;
    snapshot.reassembly_bytes = 4096;
    snapshot.reassembly_limit_rejected = 9;
    snapshot.dns_log_dropped = 10; snapshot.dns_log_pending_bytes = 4096; snapshot.dns_log_batches = 11;
    std::string encoded = serialize_metrics(snapshot);
    MetricsSnapshot decoded = parse_metrics(encoded + "unknown=42\nbad=nope\n");
    expect(decoded.updated_at == 123456 && decoded.dns_packets == 4000 &&
               decoded.stale_verdicts == 2 && decoded.dns_negative_replies == 1 &&
               decoded.ipv6_fragments == 7 &&
               decoded.ipv6_extension_errors == 8 &&
               decoded.reassembly_bytes == 4096 &&
               decoded.reassembly_limit_rejected == 9 && decoded.dns_log_dropped == 10 && decoded.dns_log_pending_bytes == 4096 && decoded.dns_log_batches == 11,
           "metrics persistence should round-trip every metric family");
    std::string json = metrics_json(decoded);
    expect(json.find("\"dns_packets\":4000") != std::string::npos &&
               json.find("\"verdict_failures\":1") != std::string::npos &&
               json.find("\"reassembly_bytes\":4096") != std::string::npos && json.find("\"dns_log_dropped\":10") != std::string::npos,
           "status JSON should expose packet, failure and reassembly metrics");

    MetricsPublisher publisher;
    unsigned writes = 0;
    bool writable = true;
    std::string last;
    const auto store = [&](const std::string &text) {
        ++writes; if (!writable) return false; last = text; return true;
    };
    MetricsSnapshot idle;
    for (uint64_t second = 0; second <= 3600; ++second) {
        idle.updated_at = second; // A timestamp alone never makes data dirty.
        publisher.publish(idle, 10000 + second, second * 1000, store);
    }
    expect(writes == 241 && parse_metrics(last).updated_at == 13600,
           "one simulated idle hour should write 241 heartbeats instead of 3601 synced snapshots");
    idle.dns_packets = 1;
    expect(publisher.publish(idle, 13601, 3601000, store) == MetricsPublication::SAVED,
           "changed counters must be visible on the next one-second sample");
    writable = false; idle.dns_packets = 2;
    expect(publisher.publish(idle, 13602, 3602000, store) == MetricsPublication::FAILED &&
               parse_metrics(last).dns_packets == 1, "failed publication cannot advance the saved checkpoint");
    writable = true;
    expect(publisher.publish(idle, 13603, 3603000, store) == MetricsPublication::SAVED &&
               parse_metrics(last).dns_packets == 2, "a failed publication retries the unchanged unsaved data");
    expect(publisher.publish(idle, 13604, 3604000, store, true) == MetricsPublication::SAVED,
           "lifecycle shutdown can force a final snapshot even without changed data");
    expect(publisher.publish(idle, 9000, 1, store) == MetricsPublication::SAVED,
           "a restarted/rolled-back monotonic clock must not stall the heartbeat");

    if (failures != 0) {
        std::cerr << failures << " metrics test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "metrics tests passed\n";
    return EXIT_SUCCESS;
}
