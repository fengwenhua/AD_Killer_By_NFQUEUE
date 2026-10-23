#include "engine.h"
#include "common.h"
#include "logging.h"
#include "rules.h"
#include "nfnetlink.h"
#include "iptables.h"
#include "packet.h"
#include "tcp_reassembly.h"
#include "dns_tcp_reassembly.h"
#include "metrics.h"
#include "startup_status.h"
#include "queue_ownership.h"
#include "fast_response.h"
#include "policy_barrier.h"
#include "exclusive_file_lock.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <exception>
#include <thread>
#include <unistd.h>
#include <pthread.h>
#include <sys/prctl.h>

// ============================================================================
// 全局状态
static std::atomic<bool> g_stop{false};     // 停止标志(SIGTERM/SIGINT)
static std::atomic<bool> g_fault{false};
// POSIX 仅保证 sig_atomic_t 可在异步信号处理器中安全写入；主线程再把停止
// 请求桥接到供工作线程读取的 C++ atomic，避免在 handler 中调用非信号安全 API。
static volatile sig_atomic_t g_signal_stop = 0;
static volatile sig_atomic_t g_signal_reload = 0;
static HandshakeReassembler g_handshakes;
static DnsTcpReassembler g_dns_tcp;
static DataPlaneMetrics g_metrics;
static PolicyBarrier g_policy;
static bool g_workers_started = false; // owner thread only
static bool g_applied_block_quic = false; // protected by policy read/write leases
// Each worker owns its separate, bounded-timeout CTNETLINK request socket.
static int g_ct_fds[2] = {-1, -1};
static FastResponseSender g_response_senders[2];

static uint64_t monotonic_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

static bool stop_requested() {
    if (g_signal_stop != 0) g_stop.store(true, std::memory_order_relaxed);
    return g_stop.load(std::memory_order_relaxed);
}

static bool send_verdict(int fd, int queue, uint32_t id, uint32_t verdict) {
    bool ok = nfq_set_verdict(fd, queue, id, verdict);
    if (!ok) g_metrics.record_verdict_failure();
    return ok;
}

static bool send_marked_verdict(int fd, int queue, uint32_t id,
                                uint32_t verdict, uint32_t mark,
                                const TransportView &flow) {
    bool ok;
    if (g_ct_fds[queue] >= 0) {
        ok = nfct_set_mark(g_ct_fds[queue], flow, mark, BLOCK_CONNMARK_MASK);
        // A connection may have expired while its packet was queued. Do not
        // create a replacement entry or leave that packet without a verdict.
        if (!ok && errno != ENOENT) {
            record_engine_error("conntrack_update_failed", "连接标记更新失败，退出并清理路由：" +
                                std::string(strerror(errno)));
            g_metrics.record_verdict_failure();
            return false;
        }
        ok = nfq_set_verdict(fd, queue, id, verdict);
    } else {
        ok = nfq_set_verdict_with_connmark(fd, queue, id, verdict, mark, BLOCK_CONNMARK_MASK);
    }
    if (!ok) g_metrics.record_verdict_failure();
    return ok;
}

static bool send_fast_response(int queue, const std::vector<uint8_t> &packet, uint32_t outdev) {
    if (!packet.empty() && g_response_senders[queue].send(packet, outdev)) return true;
    const int error = packet.empty() ? EINVAL : errno;
    static uint64_t last_warning[2] = {};
    uint64_t now = monotonic_ms();
    if (now - last_warning[queue] >= 10000 || last_warning[queue] == 0) {
        log_line("警告", "队列 %d 快速响应失败（%s），该请求安全放行而非静默丢弃", queue, strerror(error));
        last_warning[queue] = now;
    }
    g_metrics.record_response_failure();
    return false;
}

static bool send_block_verdict(int fd, int queue, uint32_t id, const TransportView &flow, uint32_t outdev) {
    if (!(flow.tcp_flags & 4)) {
        if (!send_fast_response(queue, tcp_reset_packet(flow), outdev))
            return send_verdict(fd, queue, id, NF_ACCEPT_);
        g_metrics.record_tcp_reset();
    }
    return send_marked_verdict(fd, queue, id, NF_DROP_, BLOCK_CONNMARK, flow);
}

static bool send_classified_pass_verdict(int fd, uint32_t id, const TransportView &flow) {
    return send_marked_verdict(fd, QUEUE_TLS_HTTP, id, NF_ACCEPT_, PASS_CONNMARK, flow);
}

static void record_match(Verdict verdict) {
    switch (verdict) {
        case Verdict::PASS_WHITELIST:
            g_metrics.record_match(MetricMatch::WHITELIST); break;
        case Verdict::BLOCK_SUFFIX:
            g_metrics.record_match(MetricMatch::BLOCK_SUFFIX); break;
        case Verdict::BLOCK_EXACT:
            g_metrics.record_match(MetricMatch::BLOCK_EXACT); break;
        case Verdict::PASS_DEFAULT:
            g_metrics.record_match(MetricMatch::PASS_DEFAULT); break;
    }
}

static void save_metrics() {
    static bool write_failed = false;
    MetricsSnapshot snapshot = g_metrics.snapshot();
    snapshot.updated_at = static_cast<uint64_t>(now_epoch());
    IpExtensionStats ip = ip_extension_stats();
    snapshot.ipv4_fragments = ip.ipv4_fragments;
    snapshot.ipv6_fragments = ip.ipv6_fragments;
    snapshot.ipv6_extension_errors = ip.ipv6_extension_errors;
    ReassemblyStats reassembly = g_handshakes.stats();
    snapshot.reassembly_flows = reassembly.flows;
    snapshot.reassembly_bytes = reassembly.bytes;
    snapshot.reassembly_completed = reassembly.completed;
    snapshot.reassembly_expired = reassembly.expired;
    snapshot.reassembly_evicted = reassembly.evicted;
    snapshot.reassembly_limit_rejected = reassembly.limit_rejected;
    bool ok = write_file_atomic(FILE_METRICS, serialize_metrics(snapshot));
    if (!ok && !write_failed)
        log_line("警告", "数据面指标写入失败: %s", FILE_METRICS);
    write_failed = !ok;
}

// dev_mode 读取(2 秒缓存,避免每个 DNS 包都读文件)
static bool dev_mode_now() {
    static time_t cached_at = 0;
    static bool cached = false;
    time_t now = time(nullptr);
    if (now != cached_at) {
        std::string v = read_conf_value(FILE_STATE_CONF, "dev_mode");
        cached = (v == "true" || v == "1");
        cached_at = now;
    }
    return cached;
}

// ============================================================================
// DNS 队列处理(queue 0)
// 提取查询域名 → 四态匹配 → 命中后快速响应再 DROP 原查询，其余 ACCEPT
static bool on_dns_packet(const NfqPacket &pkt, void *ud) {
    int fd = (int)(intptr_t)ud;
    g_metrics.record_packet(true);
    PolicyBarrier::Read policy(g_policy);
    if (!policy.active()) return send_verdict(fd, QUEUE_DNS, pkt.id, NF_ACCEPT_);
    bool counted_truncated = pkt.truncated;
    if (counted_truncated) g_metrics.record_truncated();
    TransportView v;
    if (!decode_ip_transport(pkt.payload, pkt.payload_len, pkt.hw_proto, v)) {
        g_metrics.record_transport_failure();
        if (v.status == TransportParseStatus::TRUNCATED && !counted_truncated)
            g_metrics.record_truncated();
        return send_verdict(fd, QUEUE_DNS, pkt.id, NF_ACCEPT_);
    }
    if (v.truncated && !counted_truncated) g_metrics.record_truncated();

    if (v.protocol == TransportProtocol::TCP) {
        TcpFlowKey flow;
        if (!tcp_flow_key_from_view(v, flow))
            return send_verdict(fd, QUEUE_DNS, pkt.id, NF_ACCEPT_);
        if (v.truncated) {
            g_dns_tcp.forget(flow);
            return send_verdict(fd, QUEUE_DNS, pkt.id, NF_ACCEPT_);
        }
        const uint64_t now = monotonic_ms();
        DnsTcpResult result = g_dns_tcp.inspect(
            flow, v.tcp_sequence, v.tcp_flags, v.payload, v.payload_len, now);
        if (result.decision == DnsTcpDecision::BLOCKED)
            return send_block_verdict(fd, QUEUE_DNS, pkt.id, v, pkt.outdev);
        if (result.decision == DnsTcpDecision::NEED_MORE)
            return send_verdict(fd, QUEUE_DNS, pkt.id, NF_ACCEPT_);
        if (result.decision == DnsTcpDecision::GIVE_UP || result.domains.empty()) {
            g_metrics.record_domain_failure();
            if (result.decision == DnsTcpDecision::MESSAGES_READY)
                g_dns_tcp.finish(flow, false, now);
            return send_verdict(fd, QUEUE_DNS, pkt.id, NF_ACCEPT_);
        }

        bool blocked = false;
        for (const auto &domain : result.domains) {
            Verdict verdict = match_domain(domain);
            record_match(verdict);
            if (dev_mode_now()) log_dns_query(domain, verdict_text(verdict));
            if (verdict == Verdict::BLOCK_EXACT ||
                verdict == Verdict::BLOCK_SUFFIX)
                blocked = true;
        }
        g_dns_tcp.finish(flow, blocked, now);
        return blocked ? send_block_verdict(fd, QUEUE_DNS, pkt.id, v, pkt.outdev)
                       : send_verdict(fd, QUEUE_DNS, pkt.id, NF_ACCEPT_);
    }

    std::vector<std::string> domains =
        dns_extract_qnames(v.payload, v.payload_len, false);
    if (domains.empty()) {
        if (v.status == TransportParseStatus::OK && v.payload_len > 0)
            g_metrics.record_domain_failure();
        return send_verdict(fd, QUEUE_DNS, pkt.id, NF_ACCEPT_);
    }
    bool blocked = false;
    for (const auto &domain : domains) {
        Verdict verdict = match_domain(domain);
        record_match(verdict);
        if (dev_mode_now()) log_dns_query(domain, verdict_text(verdict));
        if (verdict == Verdict::BLOCK_EXACT || verdict == Verdict::BLOCK_SUFFIX)
            blocked = true;
    }
    if (blocked) {
        auto response = dns_negative_packet(pkt.payload, pkt.payload_len, pkt.hw_proto);
        if (send_fast_response(QUEUE_DNS, response, pkt.outdev)) {
            g_metrics.record_dns_negative();
            return send_verdict(fd, QUEUE_DNS, pkt.id, NF_DROP_);
        }
        // Never turn an unsupported DNS packet into a silent resolver timeout.
        g_metrics.record_domain_failure();
    }
    return send_verdict(fd, QUEUE_DNS, pkt.id, NF_ACCEPT_);
}

// ============================================================================
// TLS/HTTP 队列处理(queue 1)
// 从 TCP 载荷提取 TLS ClientHello SNI 或 HTTP Host 头 → 匹配 → 命中 DROP
static bool on_tls_packet(const NfqPacket &pkt, void *ud) {
    int fd = (int)(intptr_t)ud;
    g_metrics.record_packet(false);
    PolicyBarrier::Read policy(g_policy);
    if (!policy.active()) return send_verdict(fd, QUEUE_TLS_HTTP, pkt.id, NF_ACCEPT_);
    bool counted_truncated = pkt.truncated;
    if (counted_truncated) g_metrics.record_truncated();
    TransportView v;
    const bool decoded = decode_ip_transport(pkt.payload, pkt.payload_len, pkt.hw_proto, v);
    if (decoded && v.protocol == TransportProtocol::UDP && v.dst_port == 443) {
        // A datagram admitted by the previous routes may arrive after QUIC was
        // disabled. It must not be rejected under the new effective policy.
        if (!g_applied_block_quic) return send_verdict(fd, QUEUE_TLS_HTTP, pkt.id, NF_ACCEPT_);
        if (send_fast_response(QUEUE_TLS_HTTP, udp_unreachable_packet(pkt.payload, pkt.payload_len, pkt.hw_proto), pkt.outdev)) {
            g_metrics.record_quic_reject();
            return send_verdict(fd, QUEUE_TLS_HTTP, pkt.id, NF_DROP_);
        }
        return send_verdict(fd, QUEUE_TLS_HTTP, pkt.id, NF_ACCEPT_);
    }
    if (!decoded || v.protocol != TransportProtocol::TCP) {
        g_metrics.record_transport_failure();
        if (v.status == TransportParseStatus::TRUNCATED && !counted_truncated)
            g_metrics.record_truncated();
        return send_verdict(fd, QUEUE_TLS_HTTP, pkt.id, NF_ACCEPT_);
    }
    if (v.truncated && !counted_truncated) g_metrics.record_truncated();
    TcpFlowKey flow;
    if (!tcp_flow_key_from_view(v, flow))
        return send_verdict(fd, QUEUE_TLS_HTTP, pkt.id, NF_ACCEPT_);
    uint64_t now = monotonic_ms();
    if (v.truncated) {
        // NFQUEUE copy_range 截断意味着 TCP 序列空间中存在未缓存的字节，不能
        // 假装连续重组；丢弃该流状态并安全放行。
        g_handshakes.forget(flow);
        return send_classified_pass_verdict(fd, pkt.id, v);
    }
    ReassemblyResult inspection = g_handshakes.inspect(
        flow, v.tcp_sequence, v.tcp_flags, v.payload, v.payload_len, now);
    if (inspection.decision == ReassemblyDecision::BLOCKED) {
        g_metrics.record_blocked_retransmit();
        return send_block_verdict(fd, QUEUE_TLS_HTTP, pkt.id, v, pkt.outdev);
    }
    if (inspection.decision != ReassemblyDecision::DOMAIN_FOUND) {
        if (inspection.decision == ReassemblyDecision::GIVE_UP) {
            g_metrics.record_domain_failure();
            return send_classified_pass_verdict(fd, pkt.id, v);
        }
        return send_verdict(fd, QUEUE_TLS_HTTP, pkt.id, NF_ACCEPT_);
    }
    Verdict verdict = match_domain(inspection.domain);
    record_match(verdict);
    bool blocked = verdict == Verdict::BLOCK_EXACT ||
                   verdict == Verdict::BLOCK_SUFFIX;
    if (blocked) g_handshakes.mark_blocked(flow, now);
    else g_handshakes.forget(flow);
    // TCP 命中后先 reset 本地发起者，再 DROP 原请求及保留连接级阻断。
    return blocked ? send_block_verdict(fd, QUEUE_TLS_HTTP, pkt.id, v, pkt.outdev)
                   : send_classified_pass_verdict(fd, pkt.id, v);
}

// ============================================================================
// 队列 socket 建立(iptables 之前完成,消除 ACK 读取与包消息的竞态)
//   逐条:socket → PF_BIND(IPv4/IPv6) → 队列 BIND → copy 参数 → fail-open
static int setup_queue(int queue_num, int copy_range) {
    int fd = nfq_socket_open();
    if (fd < 0) {
        record_engine_error("queue_socket_failed", "队列 " + std::to_string(queue_num) +
                            " netlink socket 创建失败：" + strerror(errno));
        return -1;
    }
    nfq_pf_bind(fd, true);
    if (!nfq_queue_bind(fd, (uint16_t)queue_num)) {
        const int setup_errno = errno;
        std::string conflict = find_queue_conflict("/proc", queue_num);
        record_engine_error(conflict.empty() ? "queue_bind_failed" : "queue_conflict",
            conflict.empty() ? "队列 " + std::to_string(queue_num) + " 绑定失败：" +
                strerror(setup_errno) + " (" + std::to_string(setup_errno) +
                ")；请检查 NET_ADMIN/SELinux 权限及其他 NFQUEUE 模块。" : conflict);
        close(fd);
        return -1;
    }
    if (!nfq_set_params(fd, (uint16_t)queue_num, NfqCopyMode::PACKET,
                        (uint32_t)copy_range, 1024)) {
        const int setup_errno = errno;
        record_engine_error("queue_params_failed", "队列 " + std::to_string(queue_num) +
                            " copy/长度参数设置失败：" + strerror(setup_errno));
        nfq_queue_unbind(fd, (uint16_t)queue_num);
        close(fd);
        return -1;
    }
    // fail-open:队列满或内核无法把消息交给用户态时直接放行。--queue-bypass
    // 只覆盖“没有监听器”，不能替代此标志；失败时不得挂载 iptables。
    if (!nfq_set_fail_open(fd, (uint16_t)queue_num, true)) {
        const int setup_errno = errno;
        record_engine_error("fail_open_unsupported", "队列 " + std::to_string(queue_num) +
                            " 无法启用 fail-open，拒绝接管流量：" + strerror(setup_errno));
        nfq_queue_unbind(fd, (uint16_t)queue_num);
        close(fd);
        return -1;
    }
    // conntrack mark 用于 TCP DNS 阻断及 TLS/HTTP 分类。启动时开启再关闭元数据拷贝，借由
    // 配置 ACK 验证 vendor 内核的 NFQUEUE/conntrack glue；verdict 仍可携带
    // NFQA_CT，而正常包无需承担 CT 元数据的 netlink 开销。
    const bool glue = nfq_set_conntrack_metadata(fd, (uint16_t)queue_num, true);
    const int glue_errno = errno;
    if (!glue && glue_errno == EOPNOTSUPP) {
        int ct_fd = nfq_socket_open();
        if (ct_fd >= 0 && nfct_probe(ct_fd)) {
            g_ct_fds[queue_num] = ct_fd;
            log_line("系统", "队列 %d：内核未启用 NFQUEUE/CT glue，使用独立 CTNETLINK 掩码标记", queue_num);
        } else {
            const int ct_errno = errno;
            if (ct_fd >= 0) close(ct_fd);
            record_engine_error("conntrack_unsupported", "NFQUEUE/CT glue 不可用，独立 CTNETLINK 探测也失败，拒绝接管流量：" +
                                std::string(strerror(ct_errno)));
            nfq_queue_unbind(fd, (uint16_t)queue_num);
            close(fd);
            return -1;
        }
    } else if (!glue || !nfq_set_conntrack_metadata(fd, (uint16_t)queue_num, false)) {
        const int setup_errno = errno;
        record_engine_error("conntrack_unsupported", "队列 " + std::to_string(queue_num) +
                            " 不支持 conntrack mark，拒绝接管流量：" + strerror(setup_errno));
        nfq_queue_unbind(fd, (uint16_t)queue_num);
        close(fd);
        return -1;
    }
    log_line("系统", "队列 %d 已就绪 (copy %uB)", queue_num, (unsigned)copy_range);
    return fd;
}

static void teardown_queue(int fd, int queue_num) {
    if (fd < 0) return;
    if (g_ct_fds[queue_num] >= 0) { close(g_ct_fds[queue_num]); g_ct_fds[queue_num] = -1; }
    nfq_queue_unbind(fd, (uint16_t)queue_num);
    nfq_pf_bind(fd, false);
    close(fd);
}

// 工作线程:纯收包-判定-回复循环
static void recv_loop(int fd, int queue_num, NfqPacketCb cb) {
    log_line("系统", "队列 %d 线程已启动", queue_num);
    int idle = 0;
    while (!g_stop.load(std::memory_order_relaxed)) {
        int n = -1;
        const uint64_t policy_epoch = g_policy.epoch();
        try {
            n = nfq_recv_packets(fd, cb, (void *)(intptr_t)fd);
            g_metrics.record_stale_verdicts(nfq_receive_report().stale_verdicts);
            if (nfq_receive_report().idle) g_policy.observe_idle(queue_num, policy_epoch);
        } catch (const std::exception &error) {
            record_engine_error("packet_callback_exception", "队列 " + std::to_string(queue_num) +
                                " 处理异常：" + error.what());
        }
        if (n < 0) {
            if (stop_requested()) break;
            // socket 异常:进入保护处理(拔规则保网)
            g_metrics.record_queue_error();
            const auto report = nfq_receive_report();
            if (read_conf_value(FILE_ENGINE_ERROR, "code").empty()) {
                record_engine_error("queue_receive_failed", "队列 " + std::to_string(queue_num) +
                    " 接收失败 kind=" + std::to_string(static_cast<int>(report.kind)) +
                    " errno=" + std::to_string(report.error) + " (" + strerror(report.error) +
                    ") seq=" + std::to_string(report.sequence) + " request=" + std::to_string(report.request_type));
            }
            watchdog_emergency();
            break;
        }
        if (n == 0) {
            // 无包:自旋若干次后退避(降低空闲 CPU 占用)
            if (++idle > 64) {
                usleep(2000);
                idle = 64;
            }
        } else {
            idle = 0;
        }
    }
    log_line("系统", "队列 %d 线程退出", queue_num);
}

// ============================================================================
// 信号处理
static void sig_handler(int sig) {
    if (sig == SIGTERM || sig == SIGINT) g_signal_stop = 1;
    else if (sig == SIGHUP) g_signal_reload = 1;
}

static bool install_signal_handlers() {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = sig_handler;
    if (sigemptyset(&action.sa_mask) != 0 ||
        sigaction(SIGTERM, &action, nullptr) != 0 ||
        sigaction(SIGINT, &action, nullptr) != 0 ||
        sigaction(SIGHUP, &action, nullptr) != 0)
        return false;

    struct sigaction ignore;
    memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    return sigemptyset(&ignore.sa_mask) == 0 &&
           sigaction(SIGPIPE, &ignore, nullptr) == 0;
}

static bool cleanup_routes() {
    const auto result = iptables_remove_all();
    if (result.ok()) return true;
    g_fault.store(true);
    std::string message = "模块路由清理未能确认完成（" + std::string(result.name()) + "）";
    for (const auto &error : result.errors) {
        log_line("清理诊断", "%s", error.c_str());
        if (message.size() < 1024) message += "; " + error;
    }
    record_engine_error("cleanup_failed", message);
    write_runtime_state("error");
    return false;
}

// 看门狗:拔除规则保网
void watchdog_emergency() {
    g_fault.store(true);
    if (g_stop.exchange(true)) return;
    // Never block a receive worker on firewall tools while its queue remains
    // bound with no consumer. The owner loop joins workers, releases queues
    // (queue-bypass), then performs bounded route cleanup.
    log_line("保护", "NFQUEUE 队列无法安全恢复，正在释放监听器并有界清理路由");
}

bool daemon_running() { return read_daemon_pid() > 0; }

// ============================================================================
// 配置装配:state.conf + adblock_mode.txt + 应用名单 → uid
//   包名→uid:优先读 app_uids.conf 缓存(--apps-apply 生成);
//   兜底直接解析 /data/system/packages.list(免 popen)
EngineConfig load_engine_config() {
    EngineConfig cfg;
    // 拦截模式
    std::string mode = read_conf_value(FILE_STATE_CONF, "mode");
    if (mode.empty()) mode = read_conf_value(FILE_ADBLOCK_MODE, "mode");
    if (mode == "1") cfg.mode = MODE_HOSTS_ONLY;
    else cfg.mode = MODE_HOSTS_ADGUARD;
    // 作用域 / 日志 / QUIC / 启用
    std::string scope = read_conf_value(FILE_STATE_CONF, "scope");
    cfg.scope = (scope == "1") ? SCOPE_PER_APP : SCOPE_GLOBAL;
    std::string dev = read_conf_value(FILE_STATE_CONF, "dev_mode");
    cfg.dev_mode = (dev == "true" || dev == "1");
    std::string quic = read_conf_value(FILE_STATE_CONF, "block_quic");
    cfg.block_quic = (quic == "true" || quic == "1");
    std::string en = read_conf_value(FILE_STATE_CONF, "enabled");
    cfg.enabled = (en != "false" && en != "0");

    // 读 uid 缓存(每行 "<uid> <包名>")
    auto load_uids = [](const char *path, std::vector<uid_t> &out) {
        std::string content;
        if (!read_file(path, content)) return;
        const char *p = content.c_str();
        while (*p) {
            char *end = nullptr;
            long uid = strtol(p, &end, 10);
            if (end > p && uid > 0) out.push_back((uid_t)uid);
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
        }
    };
    load_uids(DATA_FILE("app_uids.conf").c_str(), cfg.bypass_uids);
    if (cfg.scope == SCOPE_PER_APP) {
        // 拦截名单的 uid 缓存
        std::string content;
        if (read_file(DATA_FILE("app_intercept_uids.conf").c_str(), content)) {
            const char *p = content.c_str();
            while (*p) {
                char *end = nullptr;
                long uid = strtol(p, &end, 10);
                if (end > p && uid > 0) cfg.intercept_uids.push_back((uid_t)uid);
                while (*p && *p != '\n') p++;
                if (*p == '\n') p++;
            }
        }
    }
    return cfg;
}

// Snapshot writers participate in the same lock order: apps -> editable ->
// state. In particular, do not read the rule mode, then read routing settings
// again after a slow gzip load. rules.current selects immutable bundle files
// once; generation collection must respect readers (see O01).
static bool load_candidate(EngineConfig &cfg, RuleDB &rules, std::string &error) {
    ExclusiveFileLock apps(DATA_FILE(".apps.lock").c_str(), true, 1000);
    ExclusiveFileLock editable(DATA_FILE(".editable.lock").c_str(), true, 1000);
    ExclusiveFileLock state(STATE_LOCK_FILE, true, 1000);
    if (!apps.ok() || !editable.ok() || !state.ok()) {
        error = "配置正在保存，暂未应用；请稍后重试";
        return false;
    }
    cfg = load_engine_config();
    load_all_rules(rules);
    if (!rules.load_valid) { error = rules.load_error; return false; }
    if (cfg.mode != rules.mode) { error = "候选规则与路由的模式不一致"; return false; }
    rules.finalize(); // No allocation/sorting should be deferred until cutover.
    return true;
}

// After switching/rolling back routes, keep bypassing until both consumers
// have observed a genuinely empty socket after this epoch. An ACK-only frame
// or an interrupted syscall is not evidence of a drained receive queue.
static bool drain_policy_transition(PolicyBarrier::Change &change) {
    if (!g_workers_started) return true;
    const uint64_t epoch = change.seal();
    const uint64_t deadline = monotonic_ms() + 3000;
    while (!g_policy.drained(epoch) && !stop_requested() && monotonic_ms() < deadline)
        usleep(2000);
    if (g_policy.drained(epoch) && !stop_requested()) return true;
    change.fail_open();
    if (stop_requested()) return false;
    record_engine_error("reload_drain_failed", "热重载收包边界未能确认，停止接管并保留故障诊断");
    watchdog_emergency();
    return false;
}

// 热重载的协调事务：慢速候选构建不阻塞旧策略；入口切换期间收包线程
// 仅发送无 connmark 的 ACCEPT，不会在新/旧双栈路由上执行混合规则判定。
static bool hot_reload() {
    log_line("系统", "收到热重载信号，构建私有规则与路由候选...");
    auto candidate = std::make_shared<RuleDB>();
    EngineConfig cfg;
    std::string candidate_error;
    if (!load_candidate(cfg, *candidate, candidate_error)) {
        log_line("错误", "热重载失败，保留旧规则与路由: %s", candidate_error.c_str());
        return false;
    }
    IptablesConfig icfg;
    icfg.block_quic = cfg.block_quic;
    icfg.scope = cfg.scope;
    icfg.bypass_uids = cfg.bypass_uids;
    icfg.intercept_uids = cfg.intercept_uids;
    PolicyBarrier::Change change(g_policy);
    if (!change.ok()) {
        log_line("错误", "热重载未能及时取得数据面写租约，保留旧策略");
        return false;
    }
    std::string err = iptables_install(icfg);
    if (!err.empty()) {
        if (err.find("rollback failed") != std::string::npos) {
            change.fail_open();
            log_line("致命", "热重载 iptables 失败且回滚失败: %s,拔除规则保网",
                     err.c_str());
            record_engine_error("reload_rollback_failed", "热重载路由回滚失败: " + err);
            watchdog_emergency();
        } else {
            log_line("错误", "热重载 iptables 失败: %s,已保留旧规则和路由",
                     err.c_str());
            if (drain_policy_transition(change)) change.resume();
        }
        return false;
    }
    publish_rules_snapshot(candidate);
    g_applied_block_quic = cfg.block_quic;
    // Do not clear old reassembly decisions on a failed candidate or rollback.
    // Existing kernel PASS/BLOCK connmarks intentionally retain flow lifetime.
    g_handshakes.clear();
    g_dns_tcp.clear();
    if (!drain_policy_transition(change)) return false;
    save_rule_stats(*candidate);
    change.resume();
    log_line("规则", "热重载已完成:黑 %zu / 白 %zu (mode %d)，新连接使用新策略",
             candidate->black.size(), candidate->white.size(), candidate->mode);
    return true;
}

// ============================================================================
// 守护进程主体
int run_daemon() {
    // /proc/<pid>/comm 有 15 字节上限；完整身份由 cmdline/exe 严格校验。
    prctl(PR_SET_NAME, DAEMON_COMM, 0UL, 0UL, 0UL);
    reset_ip_extension_stats();
    g_stop.store(false, std::memory_order_relaxed);
    g_fault.store(false, std::memory_order_relaxed);
    g_signal_stop = 0;
    g_signal_reload = 0;

    // 在规则、队列或运行状态发生任何变化前接管退出信号，消除启动阶段被
    // 默认 SIGTERM 直接终止后遗留 iptables 链的窗口。
    if (!install_signal_handlers()) {
        fail_engine_start("signal_setup_failed", "信号处理器注册失败，拒绝启动");
        return 1;
    }

    // Read-only preflight before rule loading, firewall cleanup or binding.
    // In particular, never unbind or kill another module to claim its queue.
    std::string conflict = find_queue_conflict();
    if (!conflict.empty()) {
        fail_engine_start("queue_conflict", conflict);
        return 1;
    }

    // CLI 已在创建线程和接管网络前验证脱离调用方的冻结层级。
    // 不改动独立的 CPU/cpuset/stune 策略，也不关闭系统/管理器的冻结机制。

    // 1. 规则加载。启动阶段尚无数据面线程，但仍经同一个不可变快照入口发布。
    auto initial_rules = std::make_shared<RuleDB>();
    EngineConfig cfg;
    std::string candidate_error;
    if (!load_candidate(cfg, *initial_rules, candidate_error)) {
        fail_engine_start("rule_load_failed", candidate_error);
        return 1;
    }
    if (!initial_rules->ok) {
        fail_engine_start("rules_empty", "尚无可用规则，请先点击“同步开源规则”，同步成功后再启动守护。");
        return 1;
    }
    log_line("规则", "已加载:黑 %zu / 白 %zu (mode %d)",
                 initial_rules->black.size(), initial_rules->white.size(),
                 initial_rules->mode);
    if (stop_requested()) {
        return write_runtime_state("stopped") ? 0 : 1;
    }

    for (auto &sender : g_response_senders) {
        if (!sender.open()) {
            fail_engine_start("response_socket_failed", "无法创建双栈快速拒绝响应 socket，拒绝接管：" + std::string(strerror(errno)));
            return 1;
        }
    }

    // 2. 生命周期锁已证明没有其它实例。先清除崩溃遗留链，再建立队列，确保
    // 配置 ACK 阶段不可能收到旧链产生的 PACKET 消息并把包悬挂在内核队列中。
    if (!cleanup_routes()) return 1;
    if (stop_requested()) {
        return write_runtime_state("stopped") ? 0 : 1;
    }

    // 3. 队列先于新 iptables 建立
    int fd0 = setup_queue(QUEUE_DNS, DNS_COPY_RANGE);
    if (fd0 < 0) { write_runtime_state("error"); return 1; }
    if (stop_requested()) {
        teardown_queue(fd0, QUEUE_DNS);
        return write_runtime_state("stopped") ? 0 : 1;
    }
    int fd1 = setup_queue(QUEUE_TLS_HTTP, TLS_COPY_RANGE);
    if (fd1 < 0) {
        teardown_queue(fd0, QUEUE_DNS);
        write_runtime_state("error");
        return 1;
    }
    if (stop_requested()) {
        teardown_queue(fd0, QUEUE_DNS);
        teardown_queue(fd1, QUEUE_TLS_HTTP);
        return write_runtime_state("stopped") ? 0 : 1;
    }

    // 4. iptables 落地(失败即拔除并拒绝启动,防断网)
    IptablesConfig icfg;
    icfg.block_quic = cfg.block_quic;
    icfg.scope = cfg.scope;
    icfg.bypass_uids = cfg.bypass_uids;
    icfg.intercept_uids = cfg.intercept_uids;
    std::string err = iptables_install(icfg);
    if (!err.empty()) {
        record_engine_error("iptables_failed", "iptables 规则植入失败：" + err);
        teardown_queue(fd0, QUEUE_DNS);
        teardown_queue(fd1, QUEUE_TLS_HTTP);
        cleanup_routes();
        write_runtime_state("error");
        return 1;
    }
    publish_rules_snapshot(initial_rules);
    g_applied_block_quic = cfg.block_quic;
    save_rule_stats(*initial_rules);
    if (stop_requested()) {
        teardown_queue(fd0, QUEUE_DNS);
        teardown_queue(fd1, QUEUE_TLS_HTTP);
        cleanup_routes();
        const bool saved = write_runtime_state(g_fault.load() ? "error" : "stopped");
        return g_fault.load() || !saved ? 1 : 0;
    }
    write_runtime_state("running");
    log_line("系统", "引擎已启动,2 线程并行处理 DNS/HTTP-TLS 握手阶段流量");
    if (cfg.block_quic)
        log_line("系统", "已启用连接分类拦截,QUIC/UDP 443 快速返回不可达以回退 TCP");
    else
        log_line("系统", "已启用连接分类拦截:DNS 53 / TCP 80,443;QUIC/UDP 443 不接管");
    log_line("系统", "命中后快速拒绝：DNS NXDOMAIN / TCP reset / QUIC ICMP 不可达，不等待静默丢包超时");

    // 5. 双线程(DNS 与 TLS/HTTP 并行)
    std::thread t1, t2;
    try {
        t1 = std::thread(recv_loop, fd0, QUEUE_DNS, on_dns_packet);
        t2 = std::thread(recv_loop, fd1, QUEUE_TLS_HTTP, on_tls_packet);
        g_workers_started = true;
    } catch (const std::exception &error) {
        record_engine_error("thread_failed", "队列线程创建失败：" + std::string(error.what()));
        g_stop.store(true);
        if (t1.joinable()) t1.join();
        if (t2.joinable()) t2.join();
        teardown_queue(fd0, QUEUE_DNS);
        teardown_queue(fd1, QUEUE_TLS_HTTP);
        cleanup_routes();
        write_runtime_state("error");
        return 1;
    }

    // 6. 主循环:热重载 + 停止等待
    uint64_t next_metrics_ms = 0;
    while (!stop_requested()) {
        try {
            if (g_signal_reload != 0) {
                g_signal_reload = 0;
                hot_reload();
            }
            uint64_t now = monotonic_ms();
            if (now >= next_metrics_ms) {
                save_metrics();
                next_metrics_ms = now + 1000;
            }
        } catch (const std::exception &error) {
            log_line("错误", "主循环异常: %s,退出并清理路由", error.what());
            watchdog_emergency();
        }
        usleep(200 * 1000);
    }

    t1.join();
    t2.join();
    g_workers_started = false;
    try {
        save_metrics();
    } catch (const std::exception &error) {
        log_line("警告", "最终指标保存失败: %s", error.what());
    }

    // 7. No workers remain: release listeners before possibly slow cleanup.
    // New unclassified packets then use queue-bypass, not an unconsumed queue.
    teardown_queue(fd0, QUEUE_DNS);
    teardown_queue(fd1, QUEUE_TLS_HTTP);
    for (auto &sender : g_response_senders) sender.close();
    cleanup_routes();
    const bool final_state_saved = write_runtime_state(g_fault.load() ? "error" : "stopped");
    if (!final_state_saved) g_fault.store(true);
    log_line("系统", "%s", g_fault.load() ? "引擎已退出，故障状态已保留，请查看清理诊断" : "引擎已停止,拦截规则已核实清理");
    return g_fault.load() ? 1 : 0;
}
