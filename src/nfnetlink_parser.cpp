#include "nfnetlink.h"

#include <atomic>
#include <cstring>
#include <cerrno>
#include <arpa/inet.h>
#if defined(__linux__)
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_queue.h>
#include <linux/netlink.h>
#endif

namespace {

#if defined(__linux__)
constexpr uint16_t MSG_PACKET = (NFNL_SUBSYS_QUEUE << 8) | NFQNL_MSG_PACKET;
constexpr uint16_t MSG_NOOP = NLMSG_NOOP;
constexpr uint16_t MSG_ERROR = NLMSG_ERROR;
constexpr uint16_t MSG_DONE = NLMSG_DONE;
constexpr uint16_t MSG_OVERRUN = NLMSG_OVERRUN;
constexpr uint16_t QA_PACKET_HDR = NFQA_PACKET_HDR;
constexpr uint16_t QA_PAYLOAD = NFQA_PAYLOAD;
constexpr uint16_t QA_CAP_LEN = NFQA_CAP_LEN;
constexpr uint16_t ATTR_TYPE_MASK = static_cast<uint16_t>(NLA_TYPE_MASK);
#else
// macOS host tests have no Linux UAPI headers. Production Android/Linux builds
// above use the platform definitions; tests keep independent wire fixtures.
constexpr uint16_t MSG_PACKET = (3 << 8) | 0;
constexpr uint16_t MSG_NOOP = 1;
constexpr uint16_t MSG_ERROR = 2;
constexpr uint16_t MSG_DONE = 3;
constexpr uint16_t MSG_OVERRUN = 4;
constexpr uint16_t QA_PACKET_HDR = 1;
constexpr uint16_t QA_PAYLOAD = 10;
constexpr uint16_t QA_CAP_LEN = 13;
constexpr uint16_t ATTR_TYPE_MASK = 0x3fff;
#endif

struct NlMsgHeader {
    uint32_t len;
    uint16_t type;
    uint16_t flags;
    uint32_t seq;
    uint32_t pid;
};

struct NfGenMsg {
    uint8_t family;
    uint8_t version;
    uint16_t resource_id;
};

struct NlAttr {
    uint16_t len;
    uint16_t type;
};

struct PacketHeader {
    uint32_t packet_id;
    uint16_t hw_protocol;
    uint8_t hook;
} __attribute__((packed));

static_assert(sizeof(NlMsgHeader) == 16, "netlink header layout changed");
static_assert(sizeof(NfGenMsg) == 4, "nfgenmsg layout changed");
static_assert(sizeof(NlAttr) == 4, "netlink attribute layout changed");
static_assert(sizeof(PacketHeader) == 7, "NFQUEUE packet header layout changed");
static_assert(sizeof(NfqVerdictHeader) == 8, "NFQUEUE verdict header layout changed");
#if defined(__linux__)
static_assert(sizeof(NlMsgHeader) == sizeof(nlmsghdr) &&
              sizeof(NfGenMsg) == sizeof(nfgenmsg) &&
              sizeof(NlAttr) == sizeof(nlattr) &&
              sizeof(PacketHeader) == sizeof(nfqnl_msg_packet_hdr),
              "portable parser differs from Linux UAPI layout");
#endif

constexpr size_t ERROR_MESSAGE_SIZE = sizeof(NlMsgHeader) +
                                      sizeof(int32_t) + sizeof(NlMsgHeader);

constexpr size_t align4(size_t value) { return (value + 3u) & ~size_t(3u); }

std::atomic<uint32_t> sequence{0};

}  // namespace

uint32_t nfq_next_sequence() {
    return sequence.fetch_add(1, std::memory_order_relaxed) + 1;
}

NfqVerdictHeader nfq_make_verdict_header(uint32_t packet_id, uint32_t verdict) {
    return {htonl(verdict), htonl(packet_id)};
}

NfqAckStatus nfq_parse_ack_messages(const uint8_t *data, size_t len,
                                    uint32_t expected_sequence,
                                    int32_t &kernel_error) {
    kernel_error = 0;
    if ((!data && len != 0) || expected_sequence == 0)
        return NfqAckStatus::MALFORMED;
    size_t offset = 0;
    while (offset + sizeof(NlMsgHeader) <= len) {
        NlMsgHeader header;
        memcpy(&header, data + offset, sizeof(header));
        if (header.len < sizeof(NlMsgHeader) || header.len > len - offset)
            return NfqAckStatus::MALFORMED;
        if (header.type == MSG_ERROR && header.seq == expected_sequence) {
            // nlmsg_pid identifies the destination port for kernel replies on
            // current kernels; it is therefore commonly the local socket's
            // port ID rather than zero.  The recvfrom/recvmsg boundary checks
            // sockaddr_nl.nl_pid == 0 to authenticate the kernel sender.
            if (header.len < ERROR_MESSAGE_SIZE)
                return NfqAckStatus::MALFORMED;
            memcpy(&kernel_error, data + offset + sizeof(NlMsgHeader),
                   sizeof(kernel_error));
            if (kernel_error > 0 || kernel_error < -4095)
                return NfqAckStatus::MALFORMED;
            return kernel_error == 0 ? NfqAckStatus::SUCCESS
                                     : NfqAckStatus::KERNEL_ERROR;
        }
        const size_t remaining = len - offset;
        if (header.len == remaining) {
            offset += header.len;
        } else {
            const size_t step = align4(header.len);
            if (step > remaining) return NfqAckStatus::MALFORMED;
            offset += step;
        }
    }
    while (offset < len) {
        if (data[offset++] != 0) return NfqAckStatus::MALFORMED;
    }
    return NfqAckStatus::NO_MATCH;
}

NfqReceiveReport &nfq_receive_report() {
    static thread_local NfqReceiveReport report;
    return report;
}

static int parse_packet_messages(const uint8_t *data, size_t len,
                              NfqPacketCb cb, void *userdata) {
    if ((!data && len != 0) || !cb) return -1;
    int count = 0;
    size_t offset = 0;
    while (offset + sizeof(NlMsgHeader) <= len) {
        NlMsgHeader header;
        memcpy(&header, data + offset, sizeof(header));
        if (header.len < sizeof(NlMsgHeader) || header.len > len - offset)
            return -1;

        if (header.type == MSG_ERROR) {
            if (header.len < ERROR_MESSAGE_SIZE) return -1;
            int32_t error;
            memcpy(&error, data + offset + sizeof(NlMsgHeader), sizeof(error));
            if (error > 0 || error < -4095) return -1;
            if (error != 0) {
                NlMsgHeader original;
                memcpy(&original, data + offset + sizeof(NlMsgHeader) + sizeof(error), sizeof(original));
                // Device/hook removal can flush an already delivered packet.
                // Only ENOENT for our exact sequenced VERDICT request is benign;
                // a missing queue (ENODEV), config errors and all other errors
                // still trigger fail-safe shutdown.
                if (error == -ENOENT && original.type == ((3 << 8) | 1) &&
                    original.len >= 32 && (original.flags & 1) &&
                    header.seq != 0 && original.seq == header.seq) {
                    nfq_receive_report().stale_verdicts++;
                } else {
                    auto &report = nfq_receive_report();
                    report.kind = NfqReceiveKind::KERNEL;
                    report.error = -error;
                    report.sequence = header.seq;
                    report.request_type = original.type;
                    return -1;
                }
            }
        } else if (header.type == MSG_OVERRUN) {
            nfq_receive_report().kind = NfqReceiveKind::KERNEL;
            nfq_receive_report().error = ENOBUFS;
            return -1;
        } else if (header.type == MSG_PACKET) {
            const size_t fixed = sizeof(NlMsgHeader) + sizeof(NfGenMsg);
            if (header.len < fixed) return -1;

            NfGenMsg generator;
            memcpy(&generator, data + offset + sizeof(NlMsgHeader),
                   sizeof(generator));
            if (generator.version != 0) return -1;

            NfqPacket packet;
            packet.queue_num = ntohs(generator.resource_id);
            bool has_packet_header = false;
            bool has_payload = false;
            bool has_cap_len = false;
            const uint8_t *attrs = data + offset + fixed;
            const size_t attrs_len = header.len - fixed;
            size_t attr_offset = 0;
            while (attr_offset < attrs_len) {
                if (attrs_len - attr_offset < sizeof(NlAttr)) return -1;
                NlAttr attr;
                memcpy(&attr, attrs + attr_offset, sizeof(attr));
                if (attr.len < sizeof(NlAttr) || attr.len > attrs_len - attr_offset)
                    return -1;

                const uint8_t *value = attrs + attr_offset + sizeof(NlAttr);
                const size_t value_len = attr.len - sizeof(NlAttr);
                switch (attr.type & ATTR_TYPE_MASK) {
                    case 5: { // NFQA_IFINDEX_OUTDEV
                        if (value_len != sizeof(uint32_t)) return -1;
                        uint32_t index;
                        memcpy(&index, value, sizeof(index));
                        packet.outdev = ntohl(index);
                        break;
                    }
                    case QA_PACKET_HDR: {
                        if (has_packet_header) return -1;
                        if (value_len < sizeof(PacketHeader)) return -1;
                        PacketHeader wire;
                        memcpy(&wire, value, sizeof(wire));
                        packet.id = ntohl(wire.packet_id);
                        packet.hw_proto = ntohs(wire.hw_protocol);
                        packet.hook = wire.hook;
                        has_packet_header = true;
                        break;
                    }
                    case QA_PAYLOAD:
                        if (has_payload) return -1;
                        packet.payload = value_len == 0 ? nullptr : value;
                        packet.payload_len = value_len;
                        has_payload = true;
                        break;
                    case QA_CAP_LEN: {
                        if (has_cap_len) return -1;
                        if (value_len < sizeof(uint32_t)) return -1;
                        uint32_t wire_len;
                        memcpy(&wire_len, value, sizeof(wire_len));
                        packet.cap_len = ntohl(wire_len);
                        has_cap_len = true;
                        break;
                    }
                    default:
                        break;
                }

                const size_t remaining = attrs_len - attr_offset;
                if (attr.len == remaining) {
                    // Netlink permits the final attribute to end exactly at
                    // nlmsg_len; trailing alignment bytes are optional.
                    attr_offset += attr.len;
                } else {
                    const size_t step = align4(attr.len);
                    if (step > remaining) return -1;
                    attr_offset += step;
                }
            }
            if (!has_packet_header) return -1;
            if (has_cap_len && packet.cap_len < packet.payload_len) return -1;
            packet.truncated = packet.cap_len > packet.payload_len;
            count++;
            if (!cb(packet, userdata)) {
                nfq_receive_report().kind = NfqReceiveKind::CALLBACK;
                nfq_receive_report().error = errno ? errno : EIO;
                return -1;
            }
        } else if (header.type != MSG_NOOP && header.type != MSG_DONE) {
            // 允许同一 socket 上出现将来扩展的其它 nfnetlink 消息；它们不包含
            // 当前队列的 packet，因此安全跳过。
        }

        const size_t remaining = len - offset;
        if (header.len == remaining) {
            offset += header.len;
        } else {
            const size_t step = align4(header.len);
            if (step > remaining) return -1;
            offset += step;
        }
    }
    // 尾部若不足一个 header,只能是 4 字节对齐填充且必须为零。
    while (offset < len) {
        if (data[offset++] != 0) return -1;
    }
    return count;
}

int nfq_parse_packet_messages(const uint8_t *data, size_t len,
                              NfqPacketCb cb, void *userdata) {
    nfq_receive_report() = {};
    int result = parse_packet_messages(data, len, cb, userdata);
    if (result < 0 && nfq_receive_report().kind == NfqReceiveKind::NONE) {
        nfq_receive_report().kind = NfqReceiveKind::MALFORMED;
        nfq_receive_report().error = EBADMSG;
    }
    return result;
}
