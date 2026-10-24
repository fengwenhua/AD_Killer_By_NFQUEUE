#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

enum class MetricMatch {
    WHITELIST,
    BLOCK_SUFFIX,
    BLOCK_EXACT,
    PASS_DEFAULT,
};

struct MetricsSnapshot {
    uint64_t updated_at = 0;
    uint64_t dns_packets = 0;
    uint64_t tls_packets = 0;
    uint64_t transport_parse_failures = 0;
    uint64_t domain_parse_failures = 0;
    uint64_t truncated_packets = 0;
    uint64_t whitelist_hits = 0;
    uint64_t block_suffix_hits = 0;
    uint64_t block_exact_hits = 0;
    uint64_t default_passes = 0;
    uint64_t blocked_retransmits = 0;
    uint64_t verdict_failures = 0;
    uint64_t queue_errors = 0;
    uint64_t stale_verdicts = 0;
    uint64_t dns_negative_replies = 0;
    uint64_t tcp_resets = 0;
    uint64_t quic_rejects = 0;
    uint64_t response_failures = 0;
    uint64_t ipv4_fragments = 0;
    uint64_t ipv6_fragments = 0;
    uint64_t ipv6_extension_errors = 0;
    uint64_t reassembly_flows = 0;
    uint64_t reassembly_bytes = 0;
    uint64_t reassembly_completed = 0;
    uint64_t reassembly_expired = 0;
    uint64_t reassembly_evicted = 0;
    uint64_t reassembly_limit_rejected = 0;
};

class DataPlaneMetrics {
public:
    void record_packet(bool dns);
    void record_transport_failure();
    void record_domain_failure();
    void record_truncated();
    void record_match(MetricMatch match);
    void record_blocked_retransmit();
    void record_verdict_failure();
    void record_queue_error();
    void record_stale_verdicts(uint64_t count);
    void record_dns_negative();
    void record_tcp_reset();
    void record_quic_reject();
    void record_response_failure();
    MetricsSnapshot snapshot() const;

private:
    std::atomic<uint64_t> dns_packets_{0};
    std::atomic<uint64_t> tls_packets_{0};
    std::atomic<uint64_t> transport_parse_failures_{0};
    std::atomic<uint64_t> domain_parse_failures_{0};
    std::atomic<uint64_t> truncated_packets_{0};
    std::atomic<uint64_t> whitelist_hits_{0};
    std::atomic<uint64_t> block_suffix_hits_{0};
    std::atomic<uint64_t> block_exact_hits_{0};
    std::atomic<uint64_t> default_passes_{0};
    std::atomic<uint64_t> blocked_retransmits_{0};
    std::atomic<uint64_t> verdict_failures_{0};
    std::atomic<uint64_t> queue_errors_{0};
    std::atomic<uint64_t> stale_verdicts_{0};
    std::atomic<uint64_t> dns_negative_replies_{0};
    std::atomic<uint64_t> tcp_resets_{0};
    std::atomic<uint64_t> quic_rejects_{0};
    std::atomic<uint64_t> response_failures_{0};
};

std::string serialize_metrics(const MetricsSnapshot &snapshot);
MetricsSnapshot parse_metrics(const std::string &content);
std::string metrics_json(const MetricsSnapshot &snapshot);

enum class MetricsPublication { SKIPPED, SAVED, FAILED };
// The owner samples at most once/second. Unchanged data only emits a 15 s
// heartbeat; failed writes do not advance the checkpoint. This is volatile
// observability, never a durability policy for config/rules/job journals.
class MetricsPublisher {
public:
    MetricsPublication publish(MetricsSnapshot snapshot, uint64_t wall_seconds, uint64_t monotonic_ms,
        const std::function<bool(const std::string &)> &write, bool force = false);
private:
    bool saved_ = false;
    uint64_t saved_ms_ = 0;
    std::string payload_;
};
