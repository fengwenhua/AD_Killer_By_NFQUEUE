// ============================================================================
// AD_Killer_By_NFQUEUE - NFQUEUE 引擎:队列线程、包处理、守护与热重载
//
// 行为:
//   - 2 个队列线程:queue0=DNS(4096B) queue1=TLS/HTTP(4096B)
//   - DNS 查询日志四态文案:PASS(whitelist)/BLOCK(suffix)/BLOCK(exact)/PASS(default)
//   - 接管前脱离调用方冻结组；保留独立 CPU/cpuset 调度策略
//   - 看门狗:队列线程异常 → 立即拔除规则保网 → 引擎退出
// ============================================================================
#pragma once

#include "common.h"

#include <atomic>
#include <string>
#include <vector>

// 引擎配置(从 state.conf / adblock_mode.txt / 两个应用名单装配)
struct EngineConfig {
    int  mode = MODE_HOSTS_ADGUARD;    // 1=仅Hosts 2=Hosts+AdGuard
    int  scope = SCOPE_GLOBAL;         // 0=全局拦截 1=仅拦截名单内应用
    bool dev_mode = false;             // DNS 查询日志开关
    bool block_quic = false;           // 阻止 QUIC(UDP 443 快速不可达)
    bool enabled = true;               // 引擎启用
    std::vector<uid_t> bypass_uids;     // scope=0:放行名单 uid
    std::vector<uid_t> intercept_uids;  // scope=1:拦截名单 uid
};

// 读取全部配置文件(含包名→uid 解析缓存)
EngineConfig load_engine_config();

// 守护进程主体(阻塞运行直到收到停止信号):
//   1. 加载规则  2. 队列先于 iptables 建立(消除 ACK 竞态)
//   3. iptables 落地(失败即拔除并拒绝启动,防断网)
//   4. 2 个工作线程 + 主循环(热重载/停止)
int run_daemon();

// 看门狗:队列线程异常时拔除规则保网
void watchdog_emergency();

// 守护进程是否在运行
bool daemon_running();
