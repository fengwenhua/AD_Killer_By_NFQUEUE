#include "nfnetlink.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <thread>
#include <vector>

namespace {

constexpr uint16_t MSG_PACKET = (3 << 8) | 0;
constexpr uint16_t NLMSG_ERROR = 2;
constexpr uint16_t NLMSG_OVERRUN = 4;
constexpr uint16_t QA_PACKET_HDR = 1;
constexpr uint16_t QA_PAYLOAD = 10;
constexpr uint16_t QA_CAP_LEN = 13;

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        failures++;
    }
}

template <typename T>
void append_native(std::vector<uint8_t> &buffer, const T &value) {
    size_t old_size = buffer.size();
    buffer.resize(old_size + sizeof(value));
    memcpy(buffer.data() + old_size, &value, sizeof(value));
}

void pad4(std::vector<uint8_t> &buffer) {
    while ((buffer.size() & 3u) != 0) buffer.push_back(0);
}

void append_attr(std::vector<uint8_t> &message, uint16_t type,
                 const void *data, size_t size) {
    uint16_t len = static_cast<uint16_t>(4 + size);
    append_native(message, len);
    append_native(message, type);
    const auto *bytes = static_cast<const uint8_t *>(data);
    message.insert(message.end(), bytes, bytes + size);
    pad4(message);
}

std::vector<uint8_t> packet_message(bool with_header,
                                    const std::vector<uint8_t> &payload = {},
                                    uint32_t cap_len = 0,
                                    uint16_t queue_num = 7) {
    std::vector<uint8_t> message(16, 0);  // nlmsghdr
    message[4] = static_cast<uint8_t>(MSG_PACKET);
    message[5] = static_cast<uint8_t>(MSG_PACKET >> 8);
    message.resize(20, 0);                // nfgenmsg
    uint16_t wire_queue = htons(queue_num);
    memcpy(message.data() + 18, &wire_queue, sizeof(wire_queue));

    if (with_header) {
        uint8_t packet_header[7] = {};
        uint32_t id = htonl(0x12345678u);
        uint16_t protocol = htons(0x0800);
        memcpy(packet_header, &id, sizeof(id));
        memcpy(packet_header + 4, &protocol, sizeof(protocol));
        packet_header[6] = 3;
        append_attr(message, QA_PACKET_HDR, packet_header, sizeof(packet_header));
    }
    if (!payload.empty()) append_attr(message, QA_PAYLOAD, payload.data(), payload.size());
    if (cap_len != 0) {
        uint32_t wire_len = htonl(cap_len);
        append_attr(message, QA_CAP_LEN, &wire_len, sizeof(wire_len));
    }
    uint32_t message_len = static_cast<uint32_t>(message.size());
    memcpy(message.data(), &message_len, sizeof(message_len));
    return message;
}

std::vector<uint8_t> error_message(uint32_t sequence, int32_t error,
                                   uint16_t type = NLMSG_ERROR) {
    std::vector<uint8_t> message(16, 0);
    memcpy(message.data() + 4, &type, sizeof(type));
    memcpy(message.data() + 8, &sequence, sizeof(sequence));
    if (type == NLMSG_ERROR) {
        append_native(message, error);
        // struct nlmsgerr includes the original request header even for a
        // capped success ACK with no echoed request payload.
        message.resize(message.size() + 16, 0);
        const uint32_t request_len = 28;
        memcpy(message.data() + 20, &request_len, sizeof(request_len));
        memcpy(message.data() + 28, &sequence, sizeof(sequence));
    }
    uint32_t message_len = static_cast<uint32_t>(message.size());
    memcpy(message.data(), &message_len, sizeof(message_len));
    return message;
}

struct CallbackState {
    int calls = 0;
    bool accept = true;
    uint32_t id = 0;
    uint16_t queue_num = 0;
    uint16_t protocol = 0;
    uint8_t hook = 0;
    size_t payload_len = 0;
    bool payload_present = false;
    bool truncated = false;
};

bool capture_packet(const NfqPacket &packet, void *userdata) {
    auto *state = static_cast<CallbackState *>(userdata);
    state->calls++;
    state->id = packet.id;
    state->queue_num = packet.queue_num;
    state->protocol = packet.hw_proto;
    state->hook = packet.hook;
    state->payload_len = packet.payload_len;
    state->payload_present = packet.payload != nullptr;
    state->truncated = packet.truncated;
    return state->accept;
}

}  // namespace

int main() {
    NfqVerdictHeader verdict_header =
        nfq_make_verdict_header(0x12345678u, NF_ACCEPT_);
    expect(ntohl(verdict_header.verdict_be) == NF_ACCEPT_,
           "verdict header should encode the decision in network byte order");
    expect(ntohl(verdict_header.packet_id_be) == 0x12345678u,
           "verdict header should carry the queued packet ID");

    int32_t kernel_error = 0;
    auto ack = error_message(77, 0);
    expect(nfq_parse_ack_messages(ack.data(), ack.size(), 77, kernel_error) ==
               NfqAckStatus::SUCCESS && kernel_error == 0,
           "matching zero-error netlink ACK should succeed");
    uint32_t local_port = 4321;
    memcpy(ack.data() + 12, &local_port, sizeof(local_port));
    expect(nfq_parse_ack_messages(ack.data(), ack.size(), 77, kernel_error) ==
               NfqAckStatus::SUCCESS,
           "kernel ACK addressed to a local netlink port should succeed");
    auto rejected = error_message(78, -22);
    expect(nfq_parse_ack_messages(rejected.data(), rejected.size(), 78,
                                  kernel_error) == NfqAckStatus::KERNEL_ERROR &&
               kernel_error == -22,
           "matching netlink errors should preserve the kernel errno");
    auto invalid_errno = error_message(78, INT32_MIN);
    expect(nfq_parse_ack_messages(invalid_errno.data(), invalid_errno.size(), 78,
                                  kernel_error) == NfqAckStatus::MALFORMED,
           "invalid errno must be rejected before converting it to a positive int");
    expect(nfq_parse_ack_messages(ack.data(), ack.size(), 79, kernel_error) ==
               NfqAckStatus::NO_MATCH,
           "unrelated ACK sequences should be ignored");
    auto short_ack = ack;
    short_ack.resize(16);
    uint32_t short_len = static_cast<uint32_t>(short_ack.size());
    memcpy(short_ack.data(), &short_len, sizeof(short_len));
    expect(nfq_parse_ack_messages(short_ack.data(), short_ack.size(), 77,
                                  kernel_error) == NfqAckStatus::MALFORMED,
           "NLMSG_ERROR without an error field should be rejected");

    CallbackState state;
    std::vector<uint8_t> no_payload = packet_message(true);
    expect(nfq_parse_packet_messages(no_payload.data(), no_payload.size(),
                                     capture_packet, &state) == 1,
           "packet without payload should still be delivered for ACCEPT verdict");
    expect(state.calls == 1 && state.id == 0x12345678u &&
               state.queue_num == 7 && state.protocol == 0x0800 && state.hook == 3,
           "packet header fields should decode from network byte order");
    expect(!state.payload_present && state.payload_len == 0,
           "missing payload should be represented as an empty view");

    state = CallbackState{};
    std::vector<uint8_t> copied = packet_message(true, {1, 2, 3, 4}, 10);
    expect(nfq_parse_packet_messages(copied.data(), copied.size(),
                                     capture_packet, &state) == 1,
           "packet with payload should parse");
    expect(state.payload_present && state.payload_len == 4 && state.truncated,
           "CAP_LEN larger than payload should mark the packet truncated");

    state = CallbackState{};
    std::vector<uint8_t> unpadded_final = packet_message(true, {1, 2});
    unpadded_final.resize(unpadded_final.size() - 2);
    uint32_t unpadded_len = static_cast<uint32_t>(unpadded_final.size());
    memcpy(unpadded_final.data(), &unpadded_len, sizeof(unpadded_len));
    expect(nfq_parse_packet_messages(unpadded_final.data(), unpadded_final.size(),
                                     capture_packet, &state) == 1 &&
               state.payload_len == 2,
           "final netlink attributes should not require trailing alignment bytes");

    state = CallbackState{};
    state.accept = false;
    expect(nfq_parse_packet_messages(no_payload.data(), no_payload.size(),
                                     capture_packet, &state) == -1,
           "callback/verdict failure should become a fatal receive result");

    state = CallbackState{};
    std::vector<uint8_t> missing_header = packet_message(false, {1, 2, 3});
    expect(nfq_parse_packet_messages(missing_header.data(), missing_header.size(),
                                     capture_packet, &state) == -1,
           "packet message without PACKET_HDR should fail safely");
    expect(state.calls == 0, "malformed packet must not reach the callback");

    std::vector<uint8_t> malformed = no_payload;
    malformed[20] = 3;  // nla_len smaller than attribute header
    malformed[21] = 0;
    expect(nfq_parse_packet_messages(malformed.data(), malformed.size(),
                                     capture_packet, &state) == -1,
           "malformed attribute length should fail safely");

    auto wrong_version = no_payload;
    wrong_version[17] = 1;
    expect(nfq_parse_packet_messages(wrong_version.data(), wrong_version.size(),
                                     capture_packet, &state) == -1,
           "unknown nfnetlink versions should be rejected");

    state = CallbackState{};
    auto addressed_packet = no_payload;
    uint32_t destination_port = 1234;
    memcpy(addressed_packet.data() + 12, &destination_port,
           sizeof(destination_port));
    expect(nfq_parse_packet_messages(addressed_packet.data(),
                                     addressed_packet.size(), capture_packet,
                                     &state) == 1,
           "netlink destination port IDs should not be treated as sender IDs");

    auto duplicate_header = no_payload;
    uint8_t second_header[7] = {};
    append_attr(duplicate_header, QA_PACKET_HDR, second_header,
                sizeof(second_header));
    uint32_t duplicate_len = static_cast<uint32_t>(duplicate_header.size());
    memcpy(duplicate_header.data(), &duplicate_len, sizeof(duplicate_len));
    expect(nfq_parse_packet_messages(duplicate_header.data(),
                                     duplicate_header.size(), capture_packet,
                                     &state) == -1,
           "duplicate packet header attributes should be rejected");

    auto impossible_cap_len = packet_message(true, {1, 2, 3, 4}, 2);
    expect(nfq_parse_packet_messages(impossible_cap_len.data(),
                                     impossible_cap_len.size(), capture_packet,
                                     &state) == -1,
           "CAP_LEN smaller than copied payload should be rejected");

    auto runtime_error = error_message(0, -105);
    expect(nfq_parse_packet_messages(runtime_error.data(), runtime_error.size(),
                                     capture_packet, &state) == -1,
           "asynchronous netlink errors should fail the receive loop");
    expect(nfq_receive_report().error == 105 && nfq_receive_report().kind == NfqReceiveKind::KERNEL,
           "fatal errors must retain their exact kernel errno");
    auto stale = error_message(101, -ENOENT);
    uint32_t original_length = 32;
    uint16_t original_type = (3 << 8) | 1, request_flags = 1;
    memcpy(stale.data() + 20, &original_length, 4);
    memcpy(stale.data() + 24, &original_type, 2);
    memcpy(stale.data() + 26, &request_flags, 2);
    expect(nfq_parse_packet_messages(stale.data(), stale.size(), capture_packet, &state) == 0 &&
               nfq_receive_report().stale_verdicts == 1,
           "ENOENT of an exact sequenced verdict is a recoverable expired packet ID");
    uint16_t config_type = (3 << 8) | 2;
    memcpy(stale.data() + 24, &config_type, 2);
    expect(nfq_parse_packet_messages(stale.data(), stale.size(), capture_packet, &state) == -1,
           "configuration ENOENT must never be ignored");
    memcpy(stale.data() + 24, &original_type, 2);
    uint32_t wrong_sequence = 102;
    memcpy(stale.data() + 28, &wrong_sequence, 4);
    expect(nfq_parse_packet_messages(stale.data(), stale.size(), capture_packet, &state) == -1,
           "mismatched echoed requests must not be classified as recoverable");
    auto overrun = error_message(0, 0, NLMSG_OVERRUN);
    expect(nfq_parse_packet_messages(overrun.data(), overrun.size(),
                                     capture_packet, &state) == -1,
           "NLMSG_OVERRUN should trigger fail-safe recovery");

    constexpr size_t thread_count = 8;
    constexpr size_t per_thread = 4000;
    std::vector<uint32_t> sequences(thread_count * per_thread);
    std::vector<std::thread> threads;
    for (size_t thread = 0; thread < thread_count; thread++) {
        threads.emplace_back([thread, &sequences]() {
            size_t begin = thread * per_thread;
            for (size_t i = 0; i < per_thread; i++) {
                sequences[begin + i] = nfq_next_sequence();
            }
        });
    }
    for (auto &thread : threads) thread.join();
    std::sort(sequences.begin(), sequences.end());
    expect(sequences.front() != 0,
           "netlink sequence should reserve zero on normal startup");
    expect(std::adjacent_find(sequences.begin(), sequences.end()) == sequences.end(),
           "concurrent sequence generation must not produce duplicates");

    if (failures != 0) {
        std::cerr << failures << " NFQUEUE parser test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "NFQUEUE parser tests passed\n";
    return EXIT_SUCCESS;
}
