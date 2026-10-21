#include "tcp_reassembly.h"

#include <algorithm>
#include <cstring>
#include <iterator>

bool TcpFlowKey::operator==(const TcpFlowKey &other) const {
    return ip_version == other.ip_version && src_addr == other.src_addr &&
           dst_addr == other.dst_addr && src_port == other.src_port &&
           dst_port == other.dst_port;
}

bool tcp_flow_key_from_view(const TransportView &view, TcpFlowKey &key) {
    if (view.protocol != TransportProtocol::TCP ||
        (view.ip_version != 4 && view.ip_version != 6)) return false;
    key.ip_version = view.ip_version;
    key.src_addr = view.src_addr;
    key.dst_addr = view.dst_addr;
    key.src_port = view.src_port;
    key.dst_port = view.dst_port;
    return true;
}

size_t HandshakeReassembler::KeyHash::operator()(const TcpFlowKey &key) const {
    size_t hash = 1469598103934665603ull;
    auto add = [&](uint8_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    add(key.ip_version);
    size_t address_bytes = key.ip_version == 4 ? 4 : 16;
    for (size_t i = 0; i < address_bytes; ++i) add(key.src_addr[i]);
    for (size_t i = 0; i < address_bytes; ++i) add(key.dst_addr[i]);
    add(static_cast<uint8_t>(key.src_port >> 8));
    add(static_cast<uint8_t>(key.src_port));
    add(static_cast<uint8_t>(key.dst_port >> 8));
    add(static_cast<uint8_t>(key.dst_port));
    return hash;
}

HandshakeReassembler::HandshakeReassembler(ReassemblyLimits limits)
    : limits_(limits) {}

void HandshakeReassembler::erase(FlowMap::iterator it) {
    total_bytes_ -= it->second.stored_bytes;
    flows_.erase(it);
}

bool HandshakeReassembler::evict_oldest() {
    if (flows_.empty()) return false;
    auto oldest = flows_.begin();
    for (auto it = flows_.begin(); it != flows_.end(); ++it) {
        if (it->second.last_seen_ms < oldest->second.last_seen_ms) oldest = it;
    }
    erase(oldest);
    evicted_++;
    return true;
}

void HandshakeReassembler::prune_unlocked(uint64_t now_ms) {
    for (auto it = flows_.begin(); it != flows_.end();) {
        uint64_t timeout = it->second.blocked
            ? limits_.blocked_timeout_ms : limits_.idle_timeout_ms;
        uint64_t age = now_ms >= it->second.last_seen_ms
            ? now_ms - it->second.last_seen_ms : 0;
        if (age <= timeout) {
            ++it;
            continue;
        }
        auto expired = it++;
        erase(expired);
        expired_++;
    }
}

void HandshakeReassembler::prune(uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_unlocked(now_ms);
}

bool HandshakeReassembler::insert_segment(FlowState &flow, uint32_t sequence,
                                          const uint8_t *payload,
                                          size_t payload_len) {
    if (!payload || payload_len == 0) return true;
    int32_t offset = static_cast<int32_t>(sequence - flow.anchor_sequence);
    if (offset > static_cast<int32_t>(limits_.per_flow_bytes) ||
        offset < -static_cast<int32_t>(limits_.per_flow_bytes)) return false;

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
        int64_t last_end = static_cast<int64_t>(last.offset) + last.bytes.size();
        if (segment.offset > last_end) {
            merged.push_back(std::move(segment));
            continue;
        }
        size_t overlap = segment.offset < last_end
            ? static_cast<size_t>(last_end - segment.offset) : 0;
        if (overlap < segment.bytes.size()) {
            last.bytes.insert(last.bytes.end(), segment.bytes.begin() + overlap,
                              segment.bytes.end());
        }
    }
    flow.segments.swap(merged);
    size_t stored = 0;
    for (const auto &segment : flow.segments) stored += segment.bytes.size();
    if (stored > limits_.per_flow_bytes) return false;
    total_bytes_ -= flow.stored_bytes;
    flow.stored_bytes = stored;
    total_bytes_ += stored;
    return true;
}

ReassemblyResult HandshakeReassembler::inspect_contiguous(
    const FlowState &flow) const {
    if (flow.segments.empty()) return {ReassemblyDecision::NEED_MORE, ""};
    const std::vector<uint8_t> &data = flow.segments.front().bytes;
    if (data.size() < 5) return {ReassemblyDecision::NEED_MORE, ""};

    if (data[0] == 0x16 && data[1] == 0x03) {
        DomainParseResult parsed = tls_inspect_client_hello(data.data(), data.size());
        if (parsed.status == DomainParseStatus::NEED_MORE &&
            data.size() < limits_.per_flow_bytes)
            return {ReassemblyDecision::NEED_MORE, ""};
        if (parsed.status == DomainParseStatus::FOUND)
            return {ReassemblyDecision::DOMAIN_FOUND, std::move(parsed.domain)};
        return {ReassemblyDecision::GIVE_UP, ""};
    }

    static const char *methods[] = {
        "GET ", "POST ", "PUT ", "HEAD ", "DELETE ", "OPTIONS ",
        "PATCH ", "CONNECT ", "TRACE ",
    };
    bool http = false;
    for (const char *method : methods) {
        const size_t method_len = strlen(method);
        if (data.size() >= method_len &&
            memcmp(data.data(), method, method_len) == 0) {
            http = true;
            break;
        }
    }
    if (http) {
        std::string domain = http_extract_host(data.data(), data.size());
        if (!domain.empty())
            return {ReassemblyDecision::DOMAIN_FOUND, std::move(domain)};
        static const uint8_t end[] = {'\r', '\n', '\r', '\n'};
        if (std::search(data.begin(), data.end(), std::begin(end), std::end(end)) !=
            data.end()) return {ReassemblyDecision::GIVE_UP, ""};
        if (data.size() >= 8192) return {ReassemblyDecision::GIVE_UP, ""};
        return {ReassemblyDecision::NEED_MORE, ""};
    }

    // 第一段可能因乱序并非流起点，给最多三个包的向前补齐机会。
    return flow.packets_seen < 3
        ? ReassemblyResult{ReassemblyDecision::NEED_MORE, ""}
        : ReassemblyResult{ReassemblyDecision::GIVE_UP, ""};
}

ReassemblyResult HandshakeReassembler::inspect(
    const TcpFlowKey &key, uint32_t sequence, uint8_t tcp_flags,
    const uint8_t *payload, size_t payload_len, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    prune_unlocked(now_ms);
    auto found = flows_.find(key);
    if ((tcp_flags & 0x02) != 0) {
        if (found != flows_.end()) erase(found);
        found = flows_.end();
        ++sequence;  // RFC 9293: SYN consumes one sequence number before data.
    }
    if (found != flows_.end() && found->second.blocked) {
        found->second.last_seen_ms = now_ms;
        return {ReassemblyDecision::BLOCKED, ""};
    }
    if (payload_len == 0) {
        if (found != flows_.end() && (tcp_flags & 0x05) != 0) erase(found);
        return {ReassemblyDecision::NEED_MORE, ""};
    }
    if (payload_len > limits_.per_flow_bytes || limits_.max_flows == 0) {
        limit_rejected_++;
        if (found != flows_.end()) erase(found);
        return {ReassemblyDecision::GIVE_UP, ""};
    }
    if (found == flows_.end()) {
        while (flows_.size() >= limits_.max_flows && evict_oldest()) {}
        FlowState state;
        state.anchor_sequence = sequence;
        state.last_seen_ms = now_ms;
        found = flows_.emplace(key, std::move(state)).first;
    }
    FlowState &flow = found->second;
    flow.last_seen_ms = now_ms;
    flow.packets_seen++;
    if (!insert_segment(flow, sequence, payload, payload_len)) {
        limit_rejected_++;
        erase(found);
        return {ReassemblyDecision::GIVE_UP, ""};
    }
    while (total_bytes_ > limits_.total_bytes && evict_oldest()) {}
    found = flows_.find(key);
    if (found == flows_.end()) {
        limit_rejected_++;
        return {ReassemblyDecision::GIVE_UP, ""};
    }

    ReassemblyResult result = inspect_contiguous(found->second);
    if (result.decision == ReassemblyDecision::DOMAIN_FOUND) completed_++;
    if (result.decision == ReassemblyDecision::GIVE_UP ||
        ((tcp_flags & 0x05) != 0 &&
         result.decision != ReassemblyDecision::DOMAIN_FOUND)) {
        erase(found);
        result.decision = ReassemblyDecision::GIVE_UP;
        result.domain.clear();
    }
    return result;
}

void HandshakeReassembler::mark_blocked(const TcpFlowKey &key, uint64_t now_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = flows_.find(key);
    if (found == flows_.end()) return;
    total_bytes_ -= found->second.stored_bytes;
    found->second.stored_bytes = 0;
    found->second.segments.clear();
    found->second.blocked = true;
    found->second.last_seen_ms = now_ms;
}

void HandshakeReassembler::forget(const TcpFlowKey &key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = flows_.find(key);
    if (found != flows_.end()) erase(found);
}

void HandshakeReassembler::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    flows_.clear();
    total_bytes_ = 0;
}

ReassemblyStats HandshakeReassembler::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {flows_.size(), total_bytes_, completed_, expired_, evicted_,
            limit_rejected_};
}
