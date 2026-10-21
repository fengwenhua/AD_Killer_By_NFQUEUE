// AD_Killer_By_NFQUEUE - 原生 NFQUEUE netlink 实现(手写,无 libnetfilter_queue 依赖)
//
// 协议要点(生产实现直接使用 Linux/Android NDK UAPI):
//   socket(AF_NETLINK, SOCK_RAW, NETLINK_NETFILTER=12)
//   SO_RCVBUF=4MB, bind(sockaddr_nl)
//   nlmsg_type = (NFNL_SUBSYS_QUEUE=3 << 8) | NFQNL_MSG_CONFIG=2 = 0x302
//   nlmsg_flags = NLM_F_REQUEST|NLM_F_ACK = 5
//   nfgenmsg: family=0 version=0 res_id=htons(queue_num)
//   attrs(独立编号空间):
//     NFQA_CFG_CMD=1        {u8 command; u8 pad; be16 pf}
//       command: BIND=1 UNBIND=2 PF_BIND=3 PF_UNBIND=4
//     NFQA_CFG_PARAMS=2     {be32 copy_range; u8 copy_mode}
//     NFQA_CFG_QUEUE_MAXLEN=3 {be32}
//     NFQA_CFG_MASK=4       {be32}
//     NFQA_CFG_FLAGS=5      {be32}  FAIL_OPEN=0x1
//   VERDICT: type=(3<<8)|1=0x301, res_id=queue, attr NFQA_VERDICT_HDR=2
//     {be32 verdict; be32 packet_id}  NF_DROP=0 NF_ACCEPT=1
//   内核->用户: type=(3<<8)|0=0x300 NFQNL_MSG_PACKET
//     attr NFQA_PACKET_HDR=1 {be32 id; be16 hw_proto; u8 hook}
//          NFQA_PAYLOAD=10  NFQA_CAP_LEN=13
//   响应 ACK: NLMSG_ERROR=2, error==0 表示成功
#pragma once

#include <cstdint>
#include <cstddef>

enum class NfqCopyMode : uint8_t {
    NONE = 0,
    META = 1,
    PACKET = 2,
};

#define NF_DROP_ 0
#define NF_ACCEPT_ 1

// 打开 netfilter netlink socket(设置接收缓冲)
int nfq_socket_open();

// PF_BIND/PF_UNBIND(both IPv4+IPv6,幂等)
bool nfq_pf_bind(int fd, bool bind_);
// 队列 BIND(创建 queue 实例)
bool nfq_queue_bind(int fd, uint16_t queue_num);
// 队列 UNBIND(销毁)
bool nfq_queue_unbind(int fd, uint16_t queue_num);
// copy 模式与拷贝范围 + 队列长度
bool nfq_set_params(int fd, uint16_t queue_num, NfqCopyMode copy_mode,
                    uint32_t copy_range, uint32_t queue_maxlen);
// 设置 fail-open；必须检查配置 ACK，失败时不接管流量。
bool nfq_set_fail_open(int fd, uint16_t queue_num, bool on);
// 探测/切换 NFQA_CFG_F_CONNTRACK。TLS/HTTP 队列启动时先开再关，以确认
// 内核具备 verdict 中写 conntrack 属性所需的 glue，但不让每包上送 CT 元数据。
bool nfq_set_conntrack_metadata(int fd, uint16_t queue_num, bool on);

struct TransportView;
// Independent CTNETLINK path for kernels built without NETLINK_GLUE_CT.
// Uses an exclusive socket per queue worker; never creates/deletes connections.
bool nfct_probe(int fd);
bool nfct_set_mark(int fd, const TransportView &flow, uint32_t mark, uint32_t mask);

// NFQA_VERDICT_HDR 的线格式；两个字段均为网络字节序。
struct NfqVerdictHeader {
    uint32_t verdict_be;
    uint32_t packet_id_be;
};
NfqVerdictHeader nfq_make_verdict_header(uint32_t packet_id, uint32_t verdict);

enum class NfqAckStatus {
    NO_MATCH,
    SUCCESS,
    KERNEL_ERROR,
    MALFORMED,
};

// 从一个 netlink datagram 中查找指定 sequence 的 NLMSG_ERROR ACK。
// kernel_error 在 KERNEL_ERROR 时保存内核返回的负 errno。
NfqAckStatus nfq_parse_ack_messages(const uint8_t *data, size_t len,
                                    uint32_t expected_sequence,
                                    int32_t &kernel_error);

// 发送 verdict(不修改包内容)
bool nfq_set_verdict(int fd, uint16_t queue_num, uint32_t packet_id, uint32_t verdict);
// verdict 同时以掩码更新 conntrack mark；用于让 iptables 永久阻断该连接后续重传。
bool nfq_set_verdict_with_connmark(int fd, uint16_t queue_num,
                                   uint32_t packet_id, uint32_t verdict,
                                   uint32_t mark, uint32_t mask);

// 收到的一条 PACKET 消息(解析后的视图)
struct NfqPacket {
    uint32_t id = 0;
    uint16_t queue_num = 0;   // nfgenmsg.res_id，主机字节序
    uint16_t hw_proto = 0;   // 主机字节序:ETH_P_IP=0x0800 / ETH_P_IPV6=0x86DD
    uint8_t  hook = 0;
    uint32_t outdev = 0;
    const uint8_t *payload = nullptr;
    size_t   payload_len = 0;   // 实际拷贝到的字节数
    size_t   cap_len = 0;       // 原始包长(NFQA_CAP_LEN)
    bool     truncated = false;
};

// 解析一批消息(单帧可能含多条 nlmsg)。调用前必须验证实际 socket 发送者为内核。
// 回调:每解析出一条 packet 调用一次;返回 false 停止处理本帧
using NfqPacketCb = bool (*)(const NfqPacket &pkt, void *userdata);
enum class NfqReceiveKind { NONE, SOCKET, TRUNCATED, SENDER, MALFORMED, KERNEL, CALLBACK };
struct NfqReceiveReport {
    NfqReceiveKind kind = NfqReceiveKind::NONE;
    int error = 0;
    uint32_t sequence = 0;
    uint16_t request_type = 0;
    uint64_t stale_verdicts = 0;
};
// Thread-local receive diagnostics, including non-fatal expired packet IDs.
NfqReceiveReport &nfq_receive_report();
// callback=false 表示 verdict/处理失败；解析器返回 -1,由调用方触发保护。
// 无 payload 但含 PACKET_HDR 的消息仍会回调,以便发送 ACCEPT verdict。
int nfq_parse_packet_messages(const uint8_t *data, size_t len,
                              NfqPacketCb cb, void *userdata);

// 线程安全的 netlink sequence 生成器(配置消息和双队列 verdict 共用)。
uint32_t nfq_next_sequence();

// 返回: >0 已回复/处理的包数, 0 空转, -1 socket/解析/verdict 错误。
int nfq_recv_packets(int fd, NfqPacketCb cb, void *userdata);
