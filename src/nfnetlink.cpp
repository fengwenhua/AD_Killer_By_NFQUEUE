#include "nfnetlink.h"
#include "packet.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <cstddef>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <linux/netfilter.h>
#include <linux/netfilter/nfnetlink.h>
#include <linux/netfilter/nfnetlink_conntrack.h>
#include <linux/netfilter/nfnetlink_queue.h>
#include <linux/netlink.h>

static_assert(NETLINK_NETFILTER == 12, "unexpected NETLINK_NETFILTER UAPI value");
static_assert(NFNL_SUBSYS_QUEUE == 3, "unexpected NFQUEUE subsystem value");
static_assert(NFQA_PACKET_HDR == 1 && NFQA_PAYLOAD == 10 && NFQA_CAP_LEN == 13,
              "unexpected NFQUEUE packet attribute values");
static_assert(sizeof(nfqnl_msg_config_cmd) == 4,
              "unexpected NFQUEUE config command layout");
static_assert(sizeof(nfqnl_msg_config_params) == 5,
              "unexpected NFQUEUE config params layout");
static_assert(sizeof(nfqnl_msg_verdict_hdr) == sizeof(NfqVerdictHeader),
              "unexpected NFQUEUE verdict layout");
static_assert(offsetof(nfqnl_msg_verdict_hdr, verdict) ==
                  offsetof(NfqVerdictHeader, verdict_be) &&
              offsetof(nfqnl_msg_verdict_hdr, id) ==
                  offsetof(NfqVerdictHeader, packet_id_be),
              "NFQUEUE verdict fields no longer match the portable encoder");

static constexpr uint16_t MSG_VERDICT =
    (NFNL_SUBSYS_QUEUE << 8) | NFQNL_MSG_VERDICT;
static constexpr uint16_t MSG_CONFIG =
    (NFNL_SUBSYS_QUEUE << 8) | NFQNL_MSG_CONFIG;

static_assert(CTA_MARK != CTA_MARK_MASK,
              "conntrack mark and mask attributes must be distinct");

// ---------------------------------------------------------------------------
// 消息构造辅助:nlh + nfgenmsg(res_id) 后跟 attrs
struct MsgBuilder {
    alignas(nlmsghdr) uint8_t buf[512];
    size_t len = 0;
    uint32_t seq_of_ = 0;

    void begin(uint16_t type, uint16_t res_id, uint16_t flags) {
        memset(buf, 0, sizeof(buf));
        nlmsghdr *h = reinterpret_cast<nlmsghdr *>(buf);
        h->nlmsg_type = type;
        h->nlmsg_flags = flags;
        h->nlmsg_seq = nfq_next_sequence();
        h->nlmsg_pid = 0;
        seq_of_ = h->nlmsg_seq;
        nfgenmsg *g = reinterpret_cast<nfgenmsg *>(buf + sizeof(nlmsghdr));
        g->nfgen_family = AF_UNSPEC;
        g->version = NFNETLINK_V0;
        g->res_id = htons(res_id);
        len = sizeof(nlmsghdr) + sizeof(nfgenmsg);
        h->nlmsg_len = (uint32_t)len;
    }
    void add_attr(uint16_t type, const void *data, size_t dlen) {
        nlattr *a = reinterpret_cast<nlattr *>(buf + len);
        a->nla_len = static_cast<uint16_t>(sizeof(nlattr) + dlen);
        a->nla_type = type;
        memcpy(buf + len + sizeof(nlattr), data, dlen);
        len = NLA_ALIGN(len + sizeof(nlattr) + dlen);
        reinterpret_cast<nlmsghdr *>(buf)->nlmsg_len = static_cast<uint32_t>(len);
    }
    size_t begin_nested(uint16_t type) {
        size_t offset = len;
        nlattr *a = reinterpret_cast<nlattr *>(buf + len);
        a->nla_len = sizeof(nlattr);
        a->nla_type = static_cast<uint16_t>(type | NLA_F_NESTED);
        len = NLA_ALIGN(len + sizeof(nlattr));
        reinterpret_cast<nlmsghdr *>(buf)->nlmsg_len = static_cast<uint32_t>(len);
        return offset;
    }
    void end_nested(size_t offset) {
        nlattr *a = reinterpret_cast<nlattr *>(buf + offset);
        a->nla_len = static_cast<uint16_t>(len - offset);
    }
    uint32_t seq_of() const { return seq_of_; }
};

// 发送并等待 ACK(error==0);跳过插队的非 ACK 消息(如异步 PACKET)
static bool send_and_wait_ack(int fd, MsgBuilder &m) {
    struct sockaddr_nl snl;
    memset(&snl, 0, sizeof(snl));
    snl.nl_family = AF_NETLINK;
    ssize_t sent;
    do {
        sent = sendto(fd, m.buf, m.len, 0,
                      reinterpret_cast<sockaddr *>(&snl), sizeof(snl));
    } while (sent < 0 && errno == EINTR);
    if (sent != static_cast<ssize_t>(m.len)) {
        if (sent >= 0) errno = EIO;
        return false;
    }
    // 读 ACK(最多读 8 条消息,过滤插队包)
    uint8_t rbuf[8192];
    for (int attempt = 0; attempt < 8; attempt++) {
        struct sockaddr_nl from;
        memset(&from, 0, sizeof(from));
        socklen_t flen = sizeof(from);
        ssize_t n;
        do {
            n = recvfrom(fd, rbuf, sizeof(rbuf), 0,
                         reinterpret_cast<sockaddr *>(&from), &flen);
        } while (n < 0 && errno == EINTR);
        if (n <= 0) {
            if (n == 0) errno = ECONNRESET;
            return false;
        }
        if (flen < sizeof(sockaddr_nl) || from.nl_family != AF_NETLINK ||
            from.nl_pid != 0) {
            errno = EPROTO;
            return false;
        }
        int32_t kernel_error = 0;
        NfqAckStatus status = nfq_parse_ack_messages(
            rbuf, static_cast<size_t>(n), m.seq_of(), kernel_error);
        if (status == NfqAckStatus::SUCCESS) return true;
        if (status == NfqAckStatus::KERNEL_ERROR) {
            errno = kernel_error < 0 ? -kernel_error : EPROTO;
            return false;
        }
        if (status == NfqAckStatus::MALFORMED) {
            errno = EBADMSG;
            return false;
        }
    }
    errno = ETIMEDOUT;
    return false;
}

// ---------------------------------------------------------------------------
int nfq_socket_open() {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_NETFILTER);
    if (fd < 0) return -1;
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    // SO_RCVTIMEO:setup 阶段的阻塞 recvfrom(等 ACK)加 2 秒超时,
    // 防止内核不回 ACK 时永久卡死。recv 线程用 MSG_DONTWAIT,不受此影响。
    struct timeval tv = {2, 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) {
        close(fd);
        return -1;
    }
    // netlink no-enobufs(尽力而为)
#ifdef NETLINK_NO_ENOBUFS
    int one = 1;
    setsockopt(fd, SOL_NETLINK, NETLINK_NO_ENOBUFS, &one, sizeof(one));
#endif
    struct sockaddr_nl snl;
    memset(&snl, 0, sizeof(snl));
    snl.nl_family = AF_NETLINK;
    if (bind(fd, (struct sockaddr *)&snl, sizeof(snl)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool nfq_pf_bind(int fd, bool bind_) {
    for (uint16_t pf : {(uint16_t)2 /*AF_INET*/, (uint16_t)10 /*AF_INET6*/}) {
        MsgBuilder m;
        m.begin(MSG_CONFIG, 0, NLM_F_REQUEST | NLM_F_ACK);
        nfqnl_msg_config_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.command = bind_ ? static_cast<uint8_t>(NFQNL_CFG_CMD_PF_BIND)
                            : static_cast<uint8_t>(NFQNL_CFG_CMD_PF_UNBIND);
        cmd.pf = htons(pf);
        m.add_attr(NFQA_CFG_CMD, &cmd, sizeof(cmd));
        // PF_BIND 失败可容忍(可能已被绑定),忽略结果
        send_and_wait_ack(fd, m);
    }
    return true;
}

bool nfq_queue_bind(int fd, uint16_t queue_num) {
    MsgBuilder m;
    m.begin(MSG_CONFIG, queue_num, NLM_F_REQUEST | NLM_F_ACK);
    nfqnl_msg_config_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.command = NFQNL_CFG_CMD_BIND;
    cmd.pf = 0;
    m.add_attr(NFQA_CFG_CMD, &cmd, sizeof(cmd));
    return send_and_wait_ack(fd, m);
}

bool nfq_queue_unbind(int fd, uint16_t queue_num) {
    MsgBuilder m;
    m.begin(MSG_CONFIG, queue_num, NLM_F_REQUEST | NLM_F_ACK);
    nfqnl_msg_config_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.command = NFQNL_CFG_CMD_UNBIND;
    cmd.pf = 0;
    m.add_attr(NFQA_CFG_CMD, &cmd, sizeof(cmd));
    return send_and_wait_ack(fd, m); // 队列不存在时 error!=0,忽略
}

bool nfq_set_params(int fd, uint16_t queue_num, NfqCopyMode copy_mode,
                    uint32_t copy_range, uint32_t queue_maxlen) {
    MsgBuilder m;
    m.begin(MSG_CONFIG, queue_num, NLM_F_REQUEST | NLM_F_ACK);
    nfqnl_msg_config_params p;
    p.copy_range = htonl(copy_range);
    p.copy_mode = static_cast<uint8_t>(copy_mode);
    m.add_attr(NFQA_CFG_PARAMS, &p, sizeof(p));
    if (queue_maxlen) {
        uint32_t ml = htonl(queue_maxlen);
        m.add_attr(NFQA_CFG_QUEUE_MAXLEN, &ml, sizeof(ml));
    }
    return send_and_wait_ack(fd, m);
}

bool nfq_set_fail_open(int fd, uint16_t queue_num, bool on) {
    MsgBuilder m;
    m.begin(MSG_CONFIG, queue_num, NLM_F_REQUEST | NLM_F_ACK);
    uint32_t mask = htonl(NFQA_CFG_F_FAIL_OPEN);
    uint32_t flags = htonl(on ? NFQA_CFG_F_FAIL_OPEN : 0);
    m.add_attr(NFQA_CFG_FLAGS, &flags, sizeof(flags));
    m.add_attr(NFQA_CFG_MASK, &mask, sizeof(mask));
    return send_and_wait_ack(fd, m);
}

bool nfq_set_conntrack_metadata(int fd, uint16_t queue_num, bool on) {
#ifdef ADK_NFQ_TEST_NO_CT_GLUE
    if (on) { errno = EOPNOTSUPP; return false; }
#endif
    MsgBuilder m;
    m.begin(MSG_CONFIG, queue_num, NLM_F_REQUEST | NLM_F_ACK);
    uint32_t mask = htonl(NFQA_CFG_F_CONNTRACK);
    uint32_t flags = htonl(on ? NFQA_CFG_F_CONNTRACK : 0);
    m.add_attr(NFQA_CFG_FLAGS, &flags, sizeof(flags));
    m.add_attr(NFQA_CFG_MASK, &mask, sizeof(mask));
    return send_and_wait_ack(fd, m);
}

static bool ct_message(MsgBuilder &m, const TransportView &flow, uint8_t command) {
    if ((flow.ip_version != 4 && flow.ip_version != 6) ||
        flow.protocol != TransportProtocol::TCP) {
        errno = EINVAL;
        return false;
    }
    // No CREATE/EXCL/DUMP: lookup or masked update of one existing connection.
    m.begin((NFNL_SUBSYS_CTNETLINK << 8) | command, 0, NLM_F_REQUEST | NLM_F_ACK);
    reinterpret_cast<nfgenmsg *>(m.buf + sizeof(nlmsghdr))->nfgen_family =
        flow.ip_version == 4 ? AF_INET : AF_INET6;
    const size_t tuple = m.begin_nested(CTA_TUPLE_ORIG);
    const size_t ip = m.begin_nested(CTA_TUPLE_IP);
    const bool ipv4 = flow.ip_version == 4;
    m.add_attr(ipv4 ? CTA_IP_V4_SRC : CTA_IP_V6_SRC, flow.src_addr.data(), ipv4 ? 4 : 16);
    m.add_attr(ipv4 ? CTA_IP_V4_DST : CTA_IP_V6_DST, flow.dst_addr.data(), ipv4 ? 4 : 16);
    m.end_nested(ip);
    const size_t proto = m.begin_nested(CTA_TUPLE_PROTO);
    const uint8_t tcp = IPPROTO_TCP;
    const uint16_t sport = htons(flow.src_port), dport = htons(flow.dst_port);
    m.add_attr(CTA_PROTO_NUM, &tcp, sizeof(tcp));
    m.add_attr(CTA_PROTO_SRC_PORT, &sport, sizeof(sport));
    m.add_attr(CTA_PROTO_DST_PORT, &dport, sizeof(dport));
    m.end_nested(proto);
    m.end_nested(tuple);
    return true;
}

bool nfct_probe(int fd) {
    // Read-only lookups of port-zero loopback tuples, never a write probe.
    for (uint8_t family : {uint8_t(4), uint8_t(6)}) {
        TransportView flow;
        flow.ip_version = family;
        flow.protocol = TransportProtocol::TCP;
        inet_pton(family == 4 ? AF_INET : AF_INET6, family == 4 ? "127.0.0.1" : "::1",
                  flow.src_addr.data());
        flow.dst_addr = flow.src_addr;
        MsgBuilder m;
        if (!ct_message(m, flow, IPCTNL_MSG_CT_GET)) return false;
        if (!send_and_wait_ack(fd, m) && errno != ENOENT) return false;
    }
    return true;
}

bool nfct_set_mark(int fd, const TransportView &flow, uint32_t mark, uint32_t mask) {
    if (mask == 0 || (mark & ~mask) != 0) { errno = EINVAL; return false; }
    MsgBuilder m;
    if (!ct_message(m, flow, IPCTNL_MSG_CT_NEW)) return false;
    const uint32_t mark_be = htonl(mark), mask_be = htonl(mask);
    m.add_attr(CTA_MARK, &mark_be, sizeof(mark_be));
    m.add_attr(CTA_MARK_MASK, &mask_be, sizeof(mask_be));
    return send_and_wait_ack(fd, m);
}

static bool send_verdict_message(int fd, uint16_t queue_num,
                                 uint32_t packet_id, uint32_t verdict,
                                 const uint32_t *connmark,
                                 const uint32_t *connmark_mask) {
    MsgBuilder m;
    m.begin(MSG_VERDICT, queue_num, NLM_F_REQUEST);
    NfqVerdictHeader header = nfq_make_verdict_header(packet_id, verdict);
    m.add_attr(NFQA_VERDICT_HDR, &header, sizeof(header));
    if (connmark && connmark_mask) {
        const size_t nested = m.begin_nested(NFQA_CT);
        const uint32_t mark_be = htonl(*connmark);
        const uint32_t mask_be = htonl(*connmark_mask);
        m.add_attr(CTA_MARK, &mark_be, sizeof(mark_be));
        m.add_attr(CTA_MARK_MASK, &mask_be, sizeof(mask_be));
        m.end_nested(nested);
    }
    struct sockaddr_nl snl;
    memset(&snl, 0, sizeof(snl));
    snl.nl_family = AF_NETLINK;
    ssize_t sent;
    do {
        sent = sendto(fd, m.buf, m.len, MSG_DONTWAIT,
                      reinterpret_cast<sockaddr *>(&snl), sizeof(snl));
    } while (sent < 0 && errno == EINTR);
    return sent == static_cast<ssize_t>(m.len);
}

bool nfq_set_verdict(int fd, uint16_t queue_num, uint32_t packet_id,
                     uint32_t verdict) {
    return send_verdict_message(fd, queue_num, packet_id, verdict, nullptr, nullptr);
}

bool nfq_set_verdict_with_connmark(int fd, uint16_t queue_num,
                                   uint32_t packet_id, uint32_t verdict,
                                   uint32_t mark, uint32_t mask) {
    if (mask == 0) return false;
    return send_verdict_message(fd, queue_num, packet_id, verdict, &mark, &mask);
}

// ---------------------------------------------------------------------------
int nfq_recv_packets(int fd, NfqPacketCb cb, void *userdata) {
    nfq_receive_report() = {};
    static thread_local std::vector<uint8_t> buf(65536 + 4096);
    struct sockaddr_nl from;
    memset(&from, 0, sizeof(from));
    struct iovec iov;
    iov.iov_base = buf.data();
    iov.iov_len = buf.size();
    struct msghdr mh;
    memset(&mh, 0, sizeof(mh));
    mh.msg_name = &from;
    mh.msg_namelen = sizeof(from);
    mh.msg_iov = &iov;
    mh.msg_iovlen = 1;

    ssize_t n = recvmsg(fd, &mh, MSG_DONTWAIT);
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return 0;
        nfq_receive_report().kind = NfqReceiveKind::SOCKET;
        nfq_receive_report().error = errno;
        return -1;
    }
    if (n == 0) {
        nfq_receive_report().kind = NfqReceiveKind::SOCKET;
        nfq_receive_report().error = ECONNRESET;
        return -1;
    }
    if ((mh.msg_flags & MSG_TRUNC) != 0) {
        nfq_receive_report().kind = NfqReceiveKind::TRUNCATED;
        nfq_receive_report().error = EMSGSIZE;
        return -1;
    }
    if (mh.msg_namelen < sizeof(sockaddr_nl) || from.nl_family != AF_NETLINK ||
        from.nl_pid != 0) {
        nfq_receive_report().kind = NfqReceiveKind::SENDER;
        nfq_receive_report().error = EPROTO;
        return -1;
    }
    return nfq_parse_packet_messages(buf.data(), static_cast<size_t>(n), cb, userdata);
}
