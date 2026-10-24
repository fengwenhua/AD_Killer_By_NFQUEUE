#include "dns_tcp_reassembly.h"

#include "packet.h"

#include <algorithm>
#include <iterator>

namespace {

uint16_t read_u16(const uint8_t *p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}

}  // namespace

size_t DnsTcpReassembler::KeyHash::operator()(const TcpFlowKey &key) const {
    size_t hash = 1469598103934665603ull;
    auto add = [&](uint8_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    add(key.ip_version);
    const size_t address_bytes = key.ip_version == 4 ? 4 : 16;
    for (size_t i = 0; i < address_bytes; ++i) add(key.src_addr[i]);
    for (size_t i = 0; i < address_bytes; ++i) add(key.dst_addr[i]);
    add(static_cast<uint8_t>(key.src_port >> 8));
    add(static_cast<uint8_t>(key.src_port));
    add(static_cast<uint8_t>(key.dst_port >> 8));
    add(static_cast<uint8_t>(key.dst_port));
    return hash;
}

void DnsTcpReassembler::erase(FlowMap::iterator it) {
    total_bytes_ -= it->second.stored_bytes;
    flows_.erase(it);
}

bool DnsTcpReassembler::evict_oldest() {
    if (flows_.empty()) return false;
    auto oldest = flows_.begin();
    for (auto it = flows_.begin(); it != flows_.end(); ++it) {
        if (it->second.last_seen_ms < oldest->second.last_seen_ms) oldest = it;
    }
    erase(oldest);
    return true;
}

void DnsTcpReassembler::prune(uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_unlocked(now_ms);
}
std::pair<size_t, size_t> DnsTcpReassembler::retained() {
    std::lock_guard<std::mutex> lock(mutex_);
    return {flows_.size(), total_bytes_};
}
void DnsTcpReassembler::prune_unlocked(uint64_t now_ms) {
    for (auto it = flows_.begin(); it != flows_.end();) {
        const uint64_t timeout = it->second.blocked
            ? BLOCKED_TIMEOUT_MS : IDLE_TIMEOUT_MS;
        const uint64_t age = now_ms >= it->second.last_seen_ms
            ? now_ms - it->second.last_seen_ms : 0;
        if (age <= timeout) {
            ++it;
            continue;
        }
        auto expired = it++;
        erase(expired);
    }
}

bool DnsTcpReassembler::insert_segment(FlowState &flow, uint32_t sequence,
                                       const uint8_t *payload,
                                       size_t payload_len) {
    int32_t offset = static_cast<int32_t>(sequence - flow.anchor_sequence);
    if (offset > static_cast<int32_t>(MAX_FLOW_BYTES) ||
        offset < -static_cast<int32_t>(MAX_FLOW_BYTES)) return false;
    if (flow.stream_started && offset < 0) {
        const size_t consumed = std::min(static_cast<size_t>(-offset), payload_len);
        payload += consumed;
        payload_len -= consumed;
        offset += static_cast<int32_t>(consumed);
        if (payload_len == 0) return true;  // already-inspected retransmission
    }

    Segment incoming;
    incoming.offset = offset;
    incoming.bytes.assign(payload, payload + payload_len);
    flow.segments.push_back(std::move(incoming));
    std::stable_sort(flow.segments.begin(), flow.segments.end(),
                     [](const Segment &a, const Segment &b) {
                         return a.offset < b.offset;
                     });

    std::vector<Segment> merged;
    for (auto &segment : flow.segments) {
        if (merged.empty()) {
            merged.push_back(std::move(segment));
            continue;
        }
        Segment &last = merged.back();
        const int64_t last_end = static_cast<int64_t>(last.offset) +
                                 static_cast<int64_t>(last.bytes.size());
        if (segment.offset > last_end) {
            merged.push_back(std::move(segment));
            continue;
        }
        const size_t overlap = segment.offset < last_end
            ? static_cast<size_t>(last_end - segment.offset) : 0;
        if (overlap < segment.bytes.size()) {
            last.bytes.insert(last.bytes.end(), segment.bytes.begin() + overlap,
                              segment.bytes.end());
        }
    }
    flow.segments.swap(merged);
    size_t stored = 0;
    for (const auto &segment : flow.segments) stored += segment.bytes.size();
    if (stored > MAX_FLOW_BYTES) return false;
    total_bytes_ -= flow.stored_bytes;
    flow.stored_bytes = stored;
    total_bytes_ += stored;
    return true;
}

DnsTcpResult DnsTcpReassembler::inspect_contiguous(const FlowState &flow) const {
    if (flow.segments.empty()) return {DnsTcpDecision::NEED_MORE, {}};
    if (flow.stream_started && flow.segments.front().offset != 0)
        return {DnsTcpDecision::NEED_MORE, {}};
    const auto &data = flow.segments.front().bytes;
    size_t offset = 0;
    std::vector<std::string> domains;
    while (offset < data.size()) {
        if (data.size() - offset < 2)
            return offset != 0
                ? DnsTcpResult{DnsTcpDecision::MESSAGES_READY, std::move(domains), offset}
                : DnsTcpResult{DnsTcpDecision::NEED_MORE, {}};
        const size_t message_len = read_u16(data.data() + offset);
        if (message_len < 12 || message_len > MAX_FLOW_BYTES - 2) {
            if (offset != 0)
                return {DnsTcpDecision::MESSAGES_READY, std::move(domains), offset};
            return flow.packets_seen < 3
                ? DnsTcpResult{DnsTcpDecision::NEED_MORE, {}}
                : DnsTcpResult{DnsTcpDecision::GIVE_UP, {}};
        }
        const size_t frame_len = message_len + 2;
        if (frame_len > data.size() - offset) {
            if (offset != 0)
                return {DnsTcpDecision::MESSAGES_READY, std::move(domains), offset};
            return data.size() < MAX_FLOW_BYTES
                ? DnsTcpResult{DnsTcpDecision::NEED_MORE, {}}
                : DnsTcpResult{DnsTcpDecision::GIVE_UP, {}};
        }
        std::vector<std::string> frame_domains = dns_extract_qnames(
            data.data() + offset, frame_len, true);
        domains.insert(domains.end(),
                       std::make_move_iterator(frame_domains.begin()),
                       std::make_move_iterator(frame_domains.end()));
        offset += frame_len;
    }
    return {DnsTcpDecision::MESSAGES_READY, std::move(domains), offset};
}

DnsTcpResult DnsTcpReassembler::inspect(
    const TcpFlowKey &key, uint32_t sequence, uint8_t tcp_flags,
    const uint8_t *payload, size_t payload_len, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_unlocked(now_ms);
    auto found = flows_.find(key);
    if ((tcp_flags & 0x02) != 0) {
        if (found != flows_.end()) erase(found);
        found = flows_.end();
        ++sequence;  // SYN occupies one sequence number before TCP Fast Open data.
    }
    if (found != flows_.end() && found->second.blocked) {
        found->second.last_seen_ms = now_ms;
        return {DnsTcpDecision::BLOCKED, {}};
    }
    if (!payload || payload_len == 0) {
        if (found != flows_.end() && (tcp_flags & 0x05) != 0) erase(found);
        return {DnsTcpDecision::NEED_MORE, {}};
    }
    if (payload_len > MAX_FLOW_BYTES) {
        if (found != flows_.end()) erase(found);
        return {DnsTcpDecision::GIVE_UP, {}};
    }
    if (found == flows_.end()) {
        while (flows_.size() >= MAX_FLOWS && evict_oldest()) {}
        FlowState state;
        state.anchor_sequence = sequence;
        state.last_seen_ms = now_ms;
        found = flows_.emplace(key, std::move(state)).first;
    }
    FlowState &flow = found->second;
    flow.last_seen_ms = now_ms;
    flow.packets_seen++;
    if (!insert_segment(flow, sequence, payload, payload_len)) {
        erase(found);
        return {DnsTcpDecision::GIVE_UP, {}};
    }
    while (total_bytes_ > MAX_TOTAL_BYTES && evict_oldest()) {}
    found = flows_.find(key);
    if (found == flows_.end()) return {DnsTcpDecision::GIVE_UP, {}};
    DnsTcpResult result = inspect_contiguous(found->second);
    found->second.ready_bytes = result.consumed_bytes;
    if ((tcp_flags & 0x05) != 0 && result.decision != DnsTcpDecision::MESSAGES_READY) {
        erase(found);
        return {DnsTcpDecision::GIVE_UP, {}};
    }
    if (result.decision == DnsTcpDecision::GIVE_UP) erase(found);
    return result;
}

void DnsTcpReassembler::finish(const TcpFlowKey &key, bool blocked,
                               uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = flows_.find(key);
    if (found == flows_.end()) return;
    if (!blocked) {
        FlowState &flow = found->second;
        if (flow.ready_bytes == 0 || flow.segments.empty()) return;
        const int32_t consumed_end = flow.segments.front().offset +
                                     static_cast<int32_t>(flow.ready_bytes);
        total_bytes_ -= flow.stored_bytes;
        flow.stored_bytes = 0;
        std::vector<Segment> pending;
        for (auto &segment : flow.segments) {
            const int64_t end = static_cast<int64_t>(segment.offset) +
                                static_cast<int64_t>(segment.bytes.size());
            if (end <= consumed_end) continue;
            if (segment.offset < consumed_end) {
                const size_t prefix = static_cast<size_t>(consumed_end - segment.offset);
                segment.bytes.erase(segment.bytes.begin(), segment.bytes.begin() + prefix);
                segment.offset = consumed_end;
            }
            segment.offset -= consumed_end;
            flow.stored_bytes += segment.bytes.size();
            pending.push_back(std::move(segment));
        }
        flow.segments.swap(pending);
        flow.anchor_sequence += static_cast<uint32_t>(consumed_end);
        flow.stream_started = true;
        flow.ready_bytes = 0;
        flow.last_seen_ms = now_ms;
        total_bytes_ += flow.stored_bytes;
        return;
    }
    total_bytes_ -= found->second.stored_bytes;
    found->second.stored_bytes = 0;
    found->second.segments.clear();
    found->second.blocked = true;
    found->second.last_seen_ms = now_ms;
}

void DnsTcpReassembler::forget(const TcpFlowKey &key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = flows_.find(key);
    if (found != flows_.end()) erase(found);
}

void DnsTcpReassembler::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    flows_.clear();
    total_bytes_ = 0;
}
