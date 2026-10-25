#include "metrics.h"

#include <cstdlib>
#include <sstream>

namespace {

uint64_t load(const std::atomic<uint64_t> &value) {
    return value.load(std::memory_order_relaxed);
}

void parse_value(MetricsSnapshot &snapshot, const std::string &key,
                 uint64_t value) {
#define METRIC_FIELD(name) if (key == #name) { snapshot.name = value; return; }
    METRIC_FIELD(updated_at)
    METRIC_FIELD(dns_packets)
    METRIC_FIELD(tls_packets)
    METRIC_FIELD(transport_parse_failures)
    METRIC_FIELD(domain_parse_failures)
    METRIC_FIELD(truncated_packets)
    METRIC_FIELD(whitelist_hits)
    METRIC_FIELD(block_suffix_hits)
    METRIC_FIELD(block_exact_hits)
    METRIC_FIELD(default_passes)
    METRIC_FIELD(blocked_retransmits)
    METRIC_FIELD(verdict_failures)
    METRIC_FIELD(queue_errors)
    METRIC_FIELD(stale_verdicts)
    METRIC_FIELD(dns_negative_replies)
    METRIC_FIELD(tcp_resets)
    METRIC_FIELD(quic_rejects)
    METRIC_FIELD(response_failures)
    METRIC_FIELD(ipv4_fragments)
    METRIC_FIELD(ipv6_fragments)
    METRIC_FIELD(ipv6_extension_errors)
    METRIC_FIELD(reassembly_flows)
    METRIC_FIELD(reassembly_bytes)
    METRIC_FIELD(reassembly_completed)
    METRIC_FIELD(reassembly_expired)
    METRIC_FIELD(reassembly_evicted)
    METRIC_FIELD(reassembly_limit_rejected)
    METRIC_FIELD(dns_log_enqueued)
    METRIC_FIELD(dns_log_written)
    METRIC_FIELD(dns_log_dropped)
    METRIC_FIELD(dns_log_write_failures)
    METRIC_FIELD(dns_log_batches)
    METRIC_FIELD(dns_log_pending)
    METRIC_FIELD(dns_log_pending_bytes)
    METRIC_FIELD(dns_log_active)
#undef METRIC_FIELD
}

}  // namespace

void DataPlaneMetrics::record_packet(bool dns) {
    (dns ? dns_packets_ : tls_packets_).fetch_add(1, std::memory_order_relaxed);
}

void DataPlaneMetrics::record_transport_failure() {
    transport_parse_failures_.fetch_add(1, std::memory_order_relaxed);
}

void DataPlaneMetrics::record_domain_failure() {
    domain_parse_failures_.fetch_add(1, std::memory_order_relaxed);
}

void DataPlaneMetrics::record_truncated() {
    truncated_packets_.fetch_add(1, std::memory_order_relaxed);
}

void DataPlaneMetrics::record_match(MetricMatch match) {
    switch (match) {
        case MetricMatch::WHITELIST:
            whitelist_hits_.fetch_add(1, std::memory_order_relaxed); break;
        case MetricMatch::BLOCK_SUFFIX:
            block_suffix_hits_.fetch_add(1, std::memory_order_relaxed); break;
        case MetricMatch::BLOCK_EXACT:
            block_exact_hits_.fetch_add(1, std::memory_order_relaxed); break;
        case MetricMatch::PASS_DEFAULT:
            default_passes_.fetch_add(1, std::memory_order_relaxed); break;
    }
}

void DataPlaneMetrics::record_blocked_retransmit() {
    blocked_retransmits_.fetch_add(1, std::memory_order_relaxed);
}

void DataPlaneMetrics::record_verdict_failure() {
    verdict_failures_.fetch_add(1, std::memory_order_relaxed);
}

void DataPlaneMetrics::record_queue_error() {
    queue_errors_.fetch_add(1, std::memory_order_relaxed);
}

void DataPlaneMetrics::record_stale_verdicts(uint64_t count) { stale_verdicts_.fetch_add(count, std::memory_order_relaxed); }
void DataPlaneMetrics::record_dns_negative() { dns_negative_replies_.fetch_add(1, std::memory_order_relaxed); }
void DataPlaneMetrics::record_tcp_reset() { tcp_resets_.fetch_add(1, std::memory_order_relaxed); }
void DataPlaneMetrics::record_quic_reject() { quic_rejects_.fetch_add(1, std::memory_order_relaxed); }
void DataPlaneMetrics::record_response_failure() { response_failures_.fetch_add(1, std::memory_order_relaxed); }

MetricsSnapshot DataPlaneMetrics::snapshot() const {
    MetricsSnapshot result;
    result.dns_packets = load(dns_packets_);
    result.tls_packets = load(tls_packets_);
    result.transport_parse_failures = load(transport_parse_failures_);
    result.domain_parse_failures = load(domain_parse_failures_);
    result.truncated_packets = load(truncated_packets_);
    result.whitelist_hits = load(whitelist_hits_);
    result.block_suffix_hits = load(block_suffix_hits_);
    result.block_exact_hits = load(block_exact_hits_);
    result.default_passes = load(default_passes_);
    result.blocked_retransmits = load(blocked_retransmits_);
    result.verdict_failures = load(verdict_failures_);
    result.queue_errors = load(queue_errors_);
    result.stale_verdicts = load(stale_verdicts_);
    result.dns_negative_replies = load(dns_negative_replies_);
    result.tcp_resets = load(tcp_resets_);
    result.quic_rejects = load(quic_rejects_);
    result.response_failures = load(response_failures_);
    return result;
}

std::string serialize_metrics(const MetricsSnapshot &snapshot) {
    std::ostringstream output;
#define METRIC_FIELD(name) output << #name "=" << snapshot.name << "\n";
    METRIC_FIELD(updated_at)
    METRIC_FIELD(dns_packets)
    METRIC_FIELD(tls_packets)
    METRIC_FIELD(transport_parse_failures)
    METRIC_FIELD(domain_parse_failures)
    METRIC_FIELD(truncated_packets)
    METRIC_FIELD(whitelist_hits)
    METRIC_FIELD(block_suffix_hits)
    METRIC_FIELD(block_exact_hits)
    METRIC_FIELD(default_passes)
    METRIC_FIELD(blocked_retransmits)
    METRIC_FIELD(verdict_failures)
    METRIC_FIELD(queue_errors)
    METRIC_FIELD(stale_verdicts)
    METRIC_FIELD(dns_negative_replies)
    METRIC_FIELD(tcp_resets)
    METRIC_FIELD(quic_rejects)
    METRIC_FIELD(response_failures)
    METRIC_FIELD(ipv4_fragments)
    METRIC_FIELD(ipv6_fragments)
    METRIC_FIELD(ipv6_extension_errors)
    METRIC_FIELD(reassembly_flows)
    METRIC_FIELD(reassembly_bytes)
    METRIC_FIELD(reassembly_completed)
    METRIC_FIELD(reassembly_expired)
    METRIC_FIELD(reassembly_evicted)
    METRIC_FIELD(reassembly_limit_rejected)
    METRIC_FIELD(dns_log_enqueued)
    METRIC_FIELD(dns_log_written)
    METRIC_FIELD(dns_log_dropped)
    METRIC_FIELD(dns_log_write_failures)
    METRIC_FIELD(dns_log_batches)
    METRIC_FIELD(dns_log_pending)
    METRIC_FIELD(dns_log_pending_bytes)
    METRIC_FIELD(dns_log_active)
#undef METRIC_FIELD
    return output.str();
}

MetricsSnapshot parse_metrics(const std::string &content) {
    MetricsSnapshot result;
    std::istringstream lines(content);
    std::string line;
    while (std::getline(lines, line)) {
        size_t equals = line.find('=');
        if (equals == std::string::npos || equals == 0) continue;
        std::string value_text = line.substr(equals + 1);
        char *end = nullptr;
        unsigned long long value = strtoull(value_text.c_str(), &end, 10);
        if (end == value_text.c_str() || *end != '\0') continue;
        parse_value(result, line.substr(0, equals), static_cast<uint64_t>(value));
    }
    return result;
}

std::string metrics_json(const MetricsSnapshot &snapshot) {
    std::ostringstream output;
    output << "{"
           << "\"updated_at\":" << snapshot.updated_at
           << ",\"dns_packets\":" << snapshot.dns_packets
           << ",\"tls_packets\":" << snapshot.tls_packets
           << ",\"transport_parse_failures\":" << snapshot.transport_parse_failures
           << ",\"domain_parse_failures\":" << snapshot.domain_parse_failures
           << ",\"truncated_packets\":" << snapshot.truncated_packets
           << ",\"whitelist_hits\":" << snapshot.whitelist_hits
           << ",\"block_suffix_hits\":" << snapshot.block_suffix_hits
           << ",\"block_exact_hits\":" << snapshot.block_exact_hits
           << ",\"default_passes\":" << snapshot.default_passes
           << ",\"blocked_retransmits\":" << snapshot.blocked_retransmits
           << ",\"verdict_failures\":" << snapshot.verdict_failures
           << ",\"queue_errors\":" << snapshot.queue_errors
           << ",\"stale_verdicts\":" << snapshot.stale_verdicts
           << ",\"dns_negative_replies\":" << snapshot.dns_negative_replies
           << ",\"tcp_resets\":" << snapshot.tcp_resets
           << ",\"quic_rejects\":" << snapshot.quic_rejects
           << ",\"response_failures\":" << snapshot.response_failures
           << ",\"ipv4_fragments\":" << snapshot.ipv4_fragments
           << ",\"ipv6_fragments\":" << snapshot.ipv6_fragments
           << ",\"ipv6_extension_errors\":" << snapshot.ipv6_extension_errors
           << ",\"reassembly_flows\":" << snapshot.reassembly_flows
           << ",\"reassembly_bytes\":" << snapshot.reassembly_bytes
           << ",\"reassembly_completed\":" << snapshot.reassembly_completed
           << ",\"reassembly_expired\":" << snapshot.reassembly_expired
           << ",\"reassembly_evicted\":" << snapshot.reassembly_evicted
           << ",\"reassembly_limit_rejected\":"
           << snapshot.reassembly_limit_rejected
           << ",\"dns_log_enqueued\":" << snapshot.dns_log_enqueued
           << ",\"dns_log_written\":" << snapshot.dns_log_written
           << ",\"dns_log_dropped\":" << snapshot.dns_log_dropped
           << ",\"dns_log_write_failures\":" << snapshot.dns_log_write_failures
           << ",\"dns_log_batches\":" << snapshot.dns_log_batches
           << ",\"dns_log_pending\":" << snapshot.dns_log_pending
           << ",\"dns_log_pending_bytes\":" << snapshot.dns_log_pending_bytes
           << ",\"dns_log_active\":" << snapshot.dns_log_active << "}";
    return output.str();
}

MetricsPublication MetricsPublisher::publish(MetricsSnapshot snapshot, uint64_t wall_seconds, uint64_t monotonic_ms,
        const std::function<bool(const std::string &)> &write, bool force) {
    snapshot.updated_at = 0;
    const auto payload = serialize_metrics(snapshot);
    if (!force && saved_ && payload == payload_ && monotonic_ms >= saved_ms_ && monotonic_ms - saved_ms_ < 15000)
        return MetricsPublication::SKIPPED;
    snapshot.updated_at = wall_seconds;
    if (!write(serialize_metrics(snapshot))) return MetricsPublication::FAILED;
    payload_ = payload; saved_ms_ = monotonic_ms; saved_ = true;
    return MetricsPublication::SAVED;
}
