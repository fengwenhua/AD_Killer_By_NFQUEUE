#pragma once

#include "packet.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct TcpFlowKey {
    uint8_t ip_version = 0;
    std::array<uint8_t, 16> src_addr{};
    std::array<uint8_t, 16> dst_addr{};
    uint16_t src_port = 0;
    uint16_t dst_port = 0;

    bool operator==(const TcpFlowKey &other) const;
};

bool tcp_flow_key_from_view(const TransportView &view, TcpFlowKey &key);

struct ReassemblyLimits {
    size_t per_flow_bytes = 16u * 1024u;
    size_t total_bytes = 2u * 1024u * 1024u;
    size_t max_flows = 1024;
    uint64_t idle_timeout_ms = 10000;
    uint64_t blocked_timeout_ms = 30000;
};

enum class ReassemblyDecision {
    NEED_MORE,
    DOMAIN_FOUND,
    BLOCKED,
    GIVE_UP,
};

struct ReassemblyResult {
    ReassemblyDecision decision = ReassemblyDecision::GIVE_UP;
    std::string domain;
};

struct ReassemblyStats {
    size_t flows = 0;
    size_t bytes = 0;
    uint64_t completed = 0;
    uint64_t expired = 0;
    uint64_t evicted = 0;
    uint64_t limit_rejected = 0;
};

class HandshakeReassembler {
public:
    explicit HandshakeReassembler(ReassemblyLimits limits = {});

    ReassemblyResult inspect(const TcpFlowKey &key, uint32_t sequence,
                             uint8_t tcp_flags, const uint8_t *payload,
                             size_t payload_len, uint64_t now_ms);
    void mark_blocked(const TcpFlowKey &key, uint64_t now_ms);
    void forget(const TcpFlowKey &key);
    void clear();
    void prune(uint64_t now_ms);
    ReassemblyStats stats() const;

private:
    struct KeyHash {
        size_t operator()(const TcpFlowKey &key) const;
    };
    struct Segment {
        int32_t offset = 0;
        std::vector<uint8_t> bytes;
    };
    struct FlowState {
        uint32_t anchor_sequence = 0;
        uint64_t last_seen_ms = 0;
        size_t stored_bytes = 0;
        unsigned packets_seen = 0;
        bool blocked = false;
        std::vector<Segment> segments;
    };

    using FlowMap = std::unordered_map<TcpFlowKey, FlowState, KeyHash>;
    void erase(FlowMap::iterator it);
    void prune_unlocked(uint64_t now_ms);
    bool evict_oldest();
    bool insert_segment(FlowState &flow, uint32_t sequence,
                        const uint8_t *payload, size_t payload_len);
    ReassemblyResult inspect_contiguous(const FlowState &flow) const;

    ReassemblyLimits limits_;
    FlowMap flows_;
    size_t total_bytes_ = 0;
    uint64_t completed_ = 0;
    uint64_t expired_ = 0;
    uint64_t evicted_ = 0;
    uint64_t limit_rejected_ = 0;
    mutable std::mutex mutex_;
};
