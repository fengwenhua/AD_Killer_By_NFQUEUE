#pragma once

#include "tcp_reassembly.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

enum class DnsTcpDecision {
    NEED_MORE,
    MESSAGES_READY,
    BLOCKED,
    GIVE_UP,
};

struct DnsTcpResult {
    DnsTcpDecision decision = DnsTcpDecision::GIVE_UP;
    std::vector<std::string> domains;
    size_t consumed_bytes = 0;
};

// DNS-over-TCP 使用两字节消息长度，查询可跨多个 TCP segment，同一 segment 也可
// 携带多个查询。此类只缓存尚未完成判定的有限前缀；调用方必须在 MESSAGES_READY 后
// 调用 finish()，放行时消费完整帧并保留尾部半帧，或短期记住 BLOCKED。
class DnsTcpReassembler {
public:
    DnsTcpResult inspect(const TcpFlowKey &key, uint32_t sequence,
                         uint8_t tcp_flags, const uint8_t *payload,
                         size_t payload_len, uint64_t now_ms);
    void finish(const TcpFlowKey &key, bool blocked, uint64_t now_ms);
    void forget(const TcpFlowKey &key);
    void clear();
    void prune(uint64_t now_ms);
    std::pair<size_t, size_t> retained(); // flows, bytes; observation never prunes

private:
    static constexpr size_t MAX_FLOW_BYTES = 16u * 1024u;
    static constexpr size_t MAX_FLOWS = 256;
    static constexpr size_t MAX_TOTAL_BYTES = 512u * 1024u;
    static constexpr uint64_t IDLE_TIMEOUT_MS = 10000;
    static constexpr uint64_t BLOCKED_TIMEOUT_MS = 30000;

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
        bool stream_started = false;
        size_t ready_bytes = 0;
        std::vector<Segment> segments;
    };

    using FlowMap = std::unordered_map<TcpFlowKey, FlowState, KeyHash>;
    void erase(FlowMap::iterator it);
    void prune_unlocked(uint64_t now_ms);
    bool evict_oldest();
    bool insert_segment(FlowState &flow, uint32_t sequence,
                        const uint8_t *payload, size_t payload_len);
    DnsTcpResult inspect_contiguous(const FlowState &flow) const;

    FlowMap flows_;
    size_t total_bytes_ = 0;
    std::mutex mutex_;
};
