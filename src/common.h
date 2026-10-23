// ============================================================================
// AD_Killer_By_NFQUEUE - 全局去广告引擎(NFQUEUE 版)公共常量
//
// 常量与公共头文件。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

// ---------------------------------------------------------------------------
// 模块标识
// ---------------------------------------------------------------------------
#define MODULE_ID             "AD_Killer_By_NFQUEUE" // 模块唯一 id(KSU 规范:^[a-zA-Z][a-zA-Z0-9._-]+$)
#define MODULE_DISPLAY_NAME   "AD Killer"
#define DAEMON_ARGV0          "AD_Killer_By_NFQUEUEd"
#define DAEMON_COMM           "ADK_NFQd" // Linux TASK_COMM_LEN 最多 15 个可见字符
#ifndef MODULE_VERSION
#define MODULE_VERSION        "dev"           // 正式构建由 module.prop 注入
#endif
#ifndef MODULE_VERSION_CODE
#define MODULE_VERSION_CODE   0
#endif
#ifndef MODULE_RELEASE_CHANNEL
#define MODULE_RELEASE_CHANNEL "unknown"
#endif

// ---------------------------------------------------------------------------
// 路径布局
// ---------------------------------------------------------------------------
#define MODULE_DIR_BASE       "/data/adb/modules/" MODULE_ID   // 正式安装目录
#define MODULE_UPDATE_DIR     "/data/adb/modules_update/" MODULE_ID // 安装中目录
#define DATA_DIR              "/data/adb/AD_Killer_By_NFQUEUE" // 本模块独立数据目录

// DATA_DIR 下文件名拼路径(返回 std::string)
inline std::string DATA_FILE(const char *name) {
    return std::string(DATA_DIR) + "/" + name;
}

// ---------------------------------------------------------------------------
// 规则文件(两个 gzip 压缩文件)
//   hosts_rules.gz   —— Hosts/裸域名规则(gzip)
//   adguard_rules.gz —— AdGuard 语法规则(gzip)
//   custom_rules.txt —— 用户自定义规则(明文,WebUI 编辑)
// ---------------------------------------------------------------------------
#define FILE_HOSTS_RULES      (DATA_DIR "/hosts_rules.gz")
#define FILE_ADGUARD_RULES    (DATA_DIR "/adguard_rules.gz")
#define FILE_CUSTOM_RULES     (DATA_DIR "/custom_rules.txt")

// ---------------------------------------------------------------------------
// 状态/配置文件
// ---------------------------------------------------------------------------
#define FILE_STATE_CONF       (DATA_DIR "/state.conf")           // 引擎状态配置
#define FILE_ADBLOCK_MODE     (DATA_DIR "/adblock_mode.txt")     // 拦截模式(1/2)
#define FILE_DEV_MODE_ENABLED (DATA_DIR "/dev_mode_enabled")     // dev_mode 标志文件
#define FILE_ENGINE_ACTIVE    (DATA_DIR "/engine_active.txt")    // 引擎活跃标志
#define FILE_LAST_STOP_MANUAL (DATA_DIR "/last_stop_manual")     // 手动停止标记
#define FILE_RULE_STATS       (DATA_DIR "/rule_stats.conf")      // 规则统计(WebUI 读取)
#define FILE_AVAILABLE_RULE_STATS (DATA_DIR "/available_rule_stats.conf") // 已发布文件，不冒充运行快照
#define FILE_SOURCE_STATS     (DATA_DIR "/source_stats.conf")    // 最近一次来源校验统计
#define FILE_APP_WHITELIST    (DATA_DIR "/app_whitelist.txt")    // 应用放行名单(包名,scope=0)
#define FILE_APP_INTERCEPT    (DATA_DIR "/app_intercept.txt")    // 应用拦截名单(包名,scope=1)
#define FILE_UPDATE_SOURCES   (DATA_DIR "/update_sources.txt")   // 规则源列表
#define FILE_RUNTIME_STATE    (DATA_DIR "/runtime.state")        // 运行时状态(WebUI 轮询)
#define FILE_ENGINE_ERROR     (DATA_DIR "/engine_error.conf")     // 当前启动失败原因
#define FILE_METRICS          (DATA_DIR "/metrics.conf")         // 数据面指标快照
#define PROC_NFQUEUE_FILE     "/proc/net/netfilter/nfnetlink_queue"
#define STATE_LOCK_FILE       (DATA_DIR "/.state.lock")
#define DIR_LOG               (DATA_DIR "/log")
#define FILE_ENGINE_LOG       (DATA_DIR "/log/engine.log")       // daemon 日志
#define FILE_DNS_LOG          (DATA_DIR "/log/dns_query.log")    // DNS 查询日志
#define FILE_DOWNLOAD_PART    (DATA_DIR "/download.part")        // 下载临时文件
#define DIR_RULESETS          (DATA_DIR "/rulesets")             // 不可变规则 generation
#define FILE_RULESET_CURRENT  (DATA_DIR "/rules.current")        // 当前 generation 指针
#define UPDATE_LOCK_FILE      (DATA_DIR "/.update.lock")
#define DIR_UPDATE_JOBS       (DATA_DIR "/update_jobs")

#define ENGINE_LOG_MAX_BYTES  (2u * 1024u * 1024u)
#define DNS_LOG_MAX_BYTES     (2u * 1024u * 1024u)
#define UPDATE_LOG_MAX_BYTES  (1u * 1024u * 1024u)
#define LOG_BACKUP_COUNT      2
#define UPDATE_JOB_KEEP_COUNT 8

#define MODULE_PROP_PATH      (MODULE_DIR_BASE "/module.prop")
#define SERVICE_SH_PATH       (MODULE_DIR_BASE "/service.sh")

// ---------------------------------------------------------------------------
// NFQUEUE 队列编号(两个队列)
//   queue 0: DNS(TCP/UDP 53)               copy 4096B
//   queue 1: HTTP/TLS 未分类连接(TCP 80/443) copy 4096B
//   QUIC(UDP 443)可选进入 queue 1，以 ICMP 不可达快速触发回退 TCP
// ---------------------------------------------------------------------------
#define QUEUE_DNS             0
#define QUEUE_TLS_HTTP        1

#define DNS_COPY_RANGE        4096u
#define TLS_COPY_RANGE        4096u

// AOSP netd 使用低 20 位保存 partial fwmark，bit 24/25 保存 StrictController
// 状态。仅修改 bit 26..31，并在接管前检查其它 iptables 规则是否使用这些位。
// 这些位不是 Android 为第三方保留的 ABI；vendor/BPF 仍需目标设备验收。
#define BLOCK_CONNMARK        0xa4000000u
#define BLOCK_CONNMARK_MASK   0xfc000000u
#define BLOCK_CONNMARK_SPEC   "0xa4000000/0xfc000000"
#define PASS_CONNMARK         0xa8000000u
#define PASS_CONNMARK_SPEC    "0xa8000000/0xfc000000"
static_assert((BLOCK_CONNMARK_MASK & 0x030fffffu) == 0,
              "connmark mask overlaps AOSP netd state");
static_assert((BLOCK_CONNMARK & ~BLOCK_CONNMARK_MASK) == 0 &&
              (PASS_CONNMARK & ~BLOCK_CONNMARK_MASK) == 0 &&
              BLOCK_CONNMARK != PASS_CONNMARK,
              "invalid connection classification marks");

// 命中后先向本地发起者返回快速失败，再 DROP 原始请求。

// 拦截模式(adblock_mode.txt / state.conf 的 mode 键)
#define MODE_HOSTS_ONLY       1   // 仅 Hosts 规则
#define MODE_HOSTS_ADGUARD    2   // Hosts + AdGuard

// 作用域(state.conf 的 scope 键)
#define SCOPE_GLOBAL          0   // 全局拦截(app_whitelist.txt 内应用放行)
#define SCOPE_PER_APP         1   // 仅拦截 app_intercept.txt 内应用

// 守护进程管理
#define DAEMON_PID_FILE       (DATA_DIR "/daemon.pid")
#define DAEMON_LOCK_FILE      (DATA_DIR "/.daemon.lock")
#define STOP_WAIT_MS          8000   // --stop 等待退出超时
