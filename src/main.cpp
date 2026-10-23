// ============================================================================
// AD_Killer_By_NFQUEUE - CLI 入口与守护进程生命周期
//
// 13 个子命令:
//   --install --start --stop --daemon --reload --update --status
//   --version-status --mode N --scope N --dev on|off --quic on|off --autostart on|off --apps-apply
//
// 行为:
//   - 各命令的 JSON 输出({"status": ...} 形态,WebUI 依赖)
//   - --start 无 root 时经 su 重新拉起
//   - --stop 的 stopping/stopped 状态输出与 runtime.state 写入
//   - --dev 写 dev_mode_enabled 标志文件
//   - daemon/update/DNS 独立有界日志
// ============================================================================
#include "common.h"
#include "logging.h"
#include "rules.h"
#include "engine.h"
#include "iptables.h"
#include "net.h"
#include "cli_utils.h"
#include "exclusive_file_lock.h"
#include "rule_bundle.h"
#include "source_validator.h"
#include "update_job.h"
#include "rule_stream.h"
#include "metrics.h"
#include "source_config.h"
#include "snapshot_merge.h"
#include "queue_ownership.h"
#include "startup_status.h"
#include "cgroup_isolation.h"
#include "hagezi_source.h"
#include "source_preview.h"
#include "module_update.h"
#include "edit_config.h"
#include "lifecycle.h"
#include "process_runner.h"
#include "apply_state.h"
#include "config_input_lock.h"

#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <thread>
#include <chrono>
#include <memory>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>

// ---------------------------------------------------------------------------
// 目录初始化(幂等)
static bool ensure_dir(const char *path, mode_t mode) {
    if (mkdir(path, mode) != 0 && errno != EEXIST) return false;
    struct stat st {};
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) &&
           chmod(path, mode) == 0;
}

static bool ensure_dirs() {
    return ensure_dir(DATA_DIR, 0755) &&
           ensure_dir(DIR_LOG, 0755) &&
           ensure_dir(DIR_UPDATE_JOBS, 0700);
}

static bool file_exists(const char *p) { return access(p, F_OK) == 0; }

static bool write_initial_config(const char *path, const std::string &text) {
    const auto result = write_file_durable_atomic(path, text);
    return result.committed && result.durable;
}

static std::string current_metrics_json() {
    std::string content;
    return metrics_json(read_file(FILE_METRICS, content)
                            ? parse_metrics(content) : MetricsSnapshot{});
}

static std::string engine_diagnostics_json(const FreezerStatus &status, bool present) {
    const bool frozen = present && status.state == FreezerState::FROZEN;
    const bool unknown = present && status.state == FreezerState::UNKNOWN;
    return "\"frozen\":" + std::string(unknown ? "null" : frozen ? "true" : "false") +
           ",\"freezer_state\":" + source_json_string(!present ? "not_running" : unknown ? "unknown" : frozen ? "frozen" : "normal") +
           ",\"freezer_error\":" + source_json_string(unknown ? status.error : "") +
           ",\"runtime\":" + source_json_string(frozen ? "error" : read_runtime_state()) +
           ",\"last_error\":" + source_json_string(frozen
               ? "守护进程被后台冻结，网络队列无法处理。请暂停并升级模块后重新启动。"
               : read_conf_value(FILE_ENGINE_ERROR, "message")) +
           ",\"last_error_code\":" + source_json_string(frozen ? "daemon_frozen"
               : read_conf_value(FILE_ENGINE_ERROR, "code"));
}

static bool prepare_daemon_cgroups() {
    configure_process_log(FILE_ENGINE_LOG, ENGINE_LOG_MAX_BYTES, LOG_BACKUP_COUNT);
    auto isolated = isolate_daemon_cgroups();
    if (!isolated.ok) {
        fail_engine_start("cgroup_isolation_failed",
            "无法脱离调用方的后台冻结组，拒绝接管网络：" + isolated.error);
        return false;
    }
    if (isolated.moved)
        log_line("系统", "已将模块自身移出 %u 个继承的冻结层级；未修改管理器或系统冻结策略",
                 isolated.moved);
    return true;
}

// 是否具有 root 权限(iptables 需要真 root,uid!=0 时走 su 提升)
static bool am_root() { return geteuid() == 0; }

// 在候选路径中找一个可用的 su 二进制
static std::string find_su() {
    static const char *paths[] = {"/system/bin/su", "/system/xbin/su"};
    for (const char *p : paths) {
        if (access(p, X_OK) == 0) return p;
    }
    return "";
}

// ---------------------------------------------------------------------------
// --install:安装初始化(目录与默认配置)
static int cmd_install() {
    // Serialize upgrades, but never chmod or otherwise reconfigure /data/adb.
    ExclusiveFileLock install_lock("/data/adb/.AD_Killer_By_NFQUEUE.install.lock");
    if (!install_lock.ok()) {
        fprintf(stderr, "无法获取安装锁，请稍后重试\n");
        return 1;
    }
    if (!ensure_dirs()) {
        fprintf(stderr, "数据目录初始化失败: %s\n", DATA_DIR);
        return 1;
    }
    bool ok = true;
    // 默认 state.conf
    if (!file_exists(FILE_STATE_CONF)) {
        ok = write_initial_config(FILE_STATE_CONF,
                          "version=2\n"
                          "source_defaults_version=1\n"
                          "mode=2\n"
                          "scope=0\n"
                          "dev_mode=true\n"
                          "block_quic=true\n"
                          "autostart=false\n"
                          "enabled=false\n"
                          "manual_stop=true\n"
                          "updated_at=0\n") && ok;
    }
    if (read_conf_value(FILE_STATE_CONF, "dev_mode") == "true" && !file_exists(FILE_DEV_MODE_ENABLED)) {
        ok = write_file_atomic(FILE_DEV_MODE_ENABLED, "1") && ok;
    }
    if (!file_exists(FILE_ADBLOCK_MODE)) {
        ok = write_initial_config(FILE_ADBLOCK_MODE, "mode=2\n") && ok;
    }
    if (!file_exists(FILE_CUSTOM_RULES)) {
        ok = write_initial_config(FILE_CUSTOM_RULES, "") && ok;
    }
    if (!file_exists(FILE_APP_WHITELIST)) {
        ok = write_initial_config(FILE_APP_WHITELIST, "") && ok;
    }
    if (!file_exists(FILE_APP_INTERCEPT)) {
        ok = write_initial_config(FILE_APP_INTERCEPT, "") && ok;
    }
    // 默认规则源(本地可编辑)
    // Serialize the read/compare/write against both sync and source editing.
    bool sources_ok = [&]() {
        ExclusiveFileLock lock(UPDATE_LOCK_FILE);
        if (!lock.ok()) {
            fprintf(stderr, "来源更新正在运行，安装已中止，请等待任务结束\n");
            return false;
        }
        const bool migrate=read_conf_value(FILE_STATE_CONF,"source_defaults_version")!="1";
        if (!file_exists(FILE_UPDATE_SOURCES)) {
            if (!write_initial_config(FILE_UPDATE_SOURCES, default_rule_sources_text())) return false;
            return !migrate || write_conf_value(FILE_STATE_CONF,"source_defaults_version","1");
        }
        std::string configured;
        if (!read_file(FILE_UPDATE_SOURCES, configured)) return false;
        auto updated=should_refresh_default_rule_sources(configured) ? default_rule_sources_text() : migrate ? migrate_source_defaults(configured) : configured;
        if (updated!=configured && !write_initial_config(FILE_UPDATE_SOURCES,updated)) return false;
        return !migrate || write_conf_value(FILE_STATE_CONF,"source_defaults_version","1");
    }();
    ok = sources_ok && ok;
    if (!ok) {
        fprintf(stderr, "默认配置写入失败: %s\n", DATA_DIR);
        return 1;
    }
    log_line("安装", "模块安装完成,数据目录就绪: %s", DATA_DIR);
    return 0;
}

// ---------------------------------------------------------------------------
// 守护进程检测(pgrep + pid 文件)
// [M] 方括号技巧避免 pgrep 匹配到 pgrep 自身的命令行
// 返回 pid(>0)或 -1
static int find_daemon_pid() {
    // 先查 pid 文件(比 pgrep 快;同时校验 cmdline)
    int pid = read_daemon_pid();
    if (pid > 0) return pid;
    // 兜底:pgrep(兼容 pid 文件丢失/旧版本残留)
    const auto found = run_process({"pgrep", "-f", "[A]D_Killer_By_NFQUEUEd? --daemon"}, 1000, 8192);
    if (!found.ok()) return -1;
    std::istringstream rows(found.output);
    std::string line;
    while (std::getline(rows, line)) {
        if (!parse_integer_arg(line.c_str(), pid)) continue;
        if (is_daemon_pid(pid)) {
            return pid;
        }
    }
    return -1;
}

// Compare kernel file identities, not path strings: a replaced/deleted old image
// can keep the same argv and module path after a root-manager update.
static bool daemon_image_current(int pid) {
    if (pid <= 0 || !is_daemon_pid(pid)) return false;
    struct stat running {}, current {};
    const std::string path = "/proc/" + std::to_string(pid) + "/exe";
    return stat(path.c_str(), &running) == 0 && stat("/proc/self/exe", &current) == 0 &&
           running.st_dev == current.st_dev && running.st_ino == current.st_ino;
}

// Lifecycle stop, not a user pause. The verified daemon owns queue/route cleanup
// and its lifetime lock. Never edit preferences, SIGKILL, unlink its PID, or
// remove routes over a still-live instance. Also safe before data exists.
static int cmd_prepare_reboot() {
    if (!am_root()) { fprintf(stderr, "生命周期清理需要 root 权限\n"); return 1; }
    if (!file_exists(DATA_DIR)) { printf("{\"status\":\"stopped\"}\n"); return 0; }
    lifecycle::Ticket request;
    std::string error;
    if (!lifecycle::begin(DATA_DIR, lifecycle::Action::REBOOT, request, error)) {
        fprintf(stderr, "%s\n", error.c_str()); return 1;
    }
    ExclusiveFileLock operation(DATA_DIR "/.control.lock", true, 12000);
    if (!operation.ok()) { fprintf(stderr, "生命周期操作繁忙，请稍后重试\n"); return 1; }
    if (!lifecycle::current(DATA_DIR, request)) { printf("{\"status\":\"cancelled\"}\n"); return 0; }
    int pid = find_daemon_pid();
    if (pid <= 0) { printf("{\"status\":\"stopped\"}\n"); return 0; }
    configure_process_log(FILE_ENGINE_LOG, ENGINE_LOG_MAX_BYTES, LOG_BACKUP_COUNT);
    log_line("系统", "重启生命周期：优雅停止本模块守护 pid %d，保留暂停与自启策略", pid);
    if (!is_daemon_pid(pid) || kill(pid, SIGTERM) != 0) {
        log_line("错误", "生命周期停止未能通知已核实的守护，未执行强杀或路由清理");
        fprintf(stderr, "无法通知已核实的守护退出，未执行强杀或路由清理\n");
        return 1;
    }
    for (int i = 0; i < STOP_WAIT_MS / 100 && is_daemon_pid(pid); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (is_daemon_pid(pid)) {
        log_line("错误", "生命周期停止超时，保留现场且不启动第二个实例");
        fprintf(stderr, "守护退出超时，保留现场且不启动第二个实例\n");
        return 1;
    }
    printf("{\"status\":\"stopped\"}\n");
    return 0;
}

// ---------------------------------------------------------------------------
// --start:拉起守护进程
//
// 进程模型:
//   1. mkdir -p 日志目录;runtime.state = "starting"
//   2. 检测守护进程(已在运行 → state="running" + {"status": "running"})
//   3. 未运行:
//      - 无 root → su -c 重入 --start({"status": "starting_su"})
//      - 有 root → shell 防重入拉起:
//          export ADK_NFQ_START_DISPATCHED=1; mkdir -p <dir>;
//          umask 022; cd /; nohup <自身> --start >/dev/null 2>&1 < /dev/null &
//        环境变量 DISPATCHED 标记"已派发",重入的 --start 直接 daemonize
//   4. daemonize(双 fork + setsid)后 exec 自身为 --daemon
static int cmd_daemon(void);
static int cmd_start(bool boot = false, bool foreground = false) {
    if (!ensure_dirs()) {
        printf("{\"status\": \"error_storage\"}\n");
        return 1;
    }

    const char *dispatched = getenv("ADK_NFQ_START_DISPATCHED");
    lifecycle::Ticket request;
    std::string intent_error;
    if (dispatched) {
        const char *revision = getenv("ADK_NFQ_START_REVISION");
        const char *boot_request = getenv("ADK_NFQ_START_BOOT");
        request = {revision ? revision : "", boot_request && strcmp(boot_request, "1") == 0};
    } else if (!lifecycle::begin(DATA_DIR, boot ? lifecycle::Action::BOOT : lifecycle::Action::START, request, intent_error)) {
        printf("{\"status\":\"%s\",\"message\":%s}\n", boot && intent_error == "boot policy disabled or paused" ? "cancelled" : "error_storage", source_json_string(intent_error).c_str());
        return boot && intent_error == "boot policy disabled or paused" ? 0 : 1;
    }
    auto operation = std::make_unique<ExclusiveFileLock>(DATA_DIR "/.control.lock", true, 12000);
    if (!operation->ok()) { printf("{\"status\":\"error_busy\",\"message\":\"启停操作繁忙，请稍后重试\"}\n"); return 1; }
    if (!lifecycle::current(DATA_DIR, request)) { printf("{\"status\":\"cancelled\"}\n"); return 0; }

    // 自身真实路径(readlink /proc/self/exe):无论从 modules 还是 modules_update
    // 目录运行都正确,避免硬编码 MODULE_DIR_BASE 在安装期路径不对
    char self[512];
    ssize_t nl = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (nl <= 0) {
        snprintf(self, sizeof(self), "%s/AD_Killer_By_NFQUEUE", MODULE_DIR_BASE);
    } else {
        self[nl] = 0;
    }

    int old = find_daemon_pid();
    if (!dispatched && old > 0) {
        const auto freezer = daemon_cgroup_status(old);
        if (freezer.state == FreezerState::UNKNOWN) {
            printf("{\"status\":\"error_health_unknown\",\"message\":\"未能核实守护冻结状态，请刷新状态与排查权限；未自动停止或解冻进程\"}\n");
            return 1;
        }
        if (freezer.state == FreezerState::FROZEN) {
            printf("{\"status\":\"error_daemon_frozen\",\"message\":\"守护已被冻结，请先暂停再重新启动\"}\n");
            return 1;
        }
        if (read_runtime_state() == "running") {
            EngineConfig cfg = load_engine_config();
            IptablesConfig routes;
            routes.block_quic = cfg.block_quic;
            routes.scope = cfg.scope;
            routes.bypass_uids = cfg.bypass_uids;
            routes.intercept_uids = cfg.intercept_uids;
            if (!daemon_image_current(old) || !iptables_chain_active(routes)) {
                configure_process_log(FILE_ENGINE_LOG, ENGINE_LOG_MAX_BYTES, LOG_BACKUP_COUNT);
                log_line("系统", "守护映像过期或路由校验失败；本次手动启动将优雅退出本模块旧实例，再使用当前二进制重建队列和双栈路由");
                // Do not attach traffic to an unverified/old listener. A module
                // upgrade can leave a deleted executable alive. Let that exact
                // daemon release its queues and lifecycle lock before restart.
                if (!is_daemon_pid(old) || kill(old, SIGTERM) != 0) {
                    printf("{\"status\":\"error_recovery_signal\",\"message\":\"无法通知本模块旧守护退出，请刷新后重试\"}\n");
                    return 1;
                }
                for (int i = 0; i < STOP_WAIT_MS / 100 && is_daemon_pid(old); i++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (is_daemon_pid(old) || find_daemon_pid() > 0) {
                    record_engine_error("recovery_stop_timeout", "旧守护尚未安全退出，未启动第二个实例；请先暂停后重试");
                    printf("{\"status\":\"error_recovery_stop_timeout\",\"message\":\"旧守护尚未安全退出，未启动第二个实例；请先暂停后重试\"}\n");
                    return 1;
                }
                old = -1;
            }
        }
    }
    if (!dispatched && old <= 0) {
        std::string conflict = find_queue_conflict();
        if (!conflict.empty()) {
            configure_process_log(FILE_ENGINE_LOG, ENGINE_LOG_MAX_BYTES, LOG_BACKUP_COUNT);
            fail_engine_start("queue_conflict", conflict);
            printf("{\"status\":\"error_queue_conflict\",\"message\":%s}\n",
                   source_json_string(conflict).c_str());
            return 1;
        }
        RuleDB candidate;
        load_all_rules(candidate);
        if (!candidate.load_valid || !candidate.ok) {
            const std::string message = candidate.load_valid
                ? "尚无可用规则，请先点击“同步开源规则”，同步成功后再启动守护。"
                : "规则读取失败，请先点击“同步开源规则”修复规则后再启动守护。";
            configure_process_log(FILE_ENGINE_LOG, ENGINE_LOG_MAX_BYTES, LOG_BACKUP_COUNT);
            fail_engine_start(candidate.load_valid ? "rules_empty" : "rule_load_failed", message);
            printf("{\"status\":\"%s\",\"message\":%s}\n",
                   candidate.load_valid ? "error_rules_empty" : "error_rule_load",
                   source_json_string(message).c_str());
            return 1;
        }
        if (!write_file_atomic(FILE_ENGINE_ERROR, "")) {
            printf("{\"status\":\"error_storage\"}\n");
            return 1;
        }
    }

    if (!dispatched && !lifecycle::enable(DATA_DIR, request, intent_error)) {
        printf("{\"status\":\"error_start_cancelled\",\"message\":%s}\n", source_json_string(intent_error).c_str());
        return 1;
    }

    // 检测守护进程是否已在运行
    if (old > 0 && !dispatched) {
        log_line("系统", "守护进程已在运行 (pid %d)", old);
        // A live PID can still be initializing its queues/routes.
        printf("{\"status\":\"%s\"}\n", read_runtime_state() == "running" ? "running" : "starting");
        return 0;
    }
    // Escape before spawning background children: double fork/setsid does not
    // detach an inherited Android app cgroup. Do not migrate the manager itself.
    if (am_root() && !prepare_daemon_cgroups()) return 1;
    if (!write_runtime_state("starting")) {
        printf("{\"status\":\"error_storage\"}\n");
        return 1;
    }

    if (foreground) {
        setenv("ADK_NFQ_START_REVISION", request.revision.c_str(), 1);
        setenv("ADK_NFQ_START_BOOT", request.boot ? "1" : "0", 1);
        operation.reset();
        return cmd_daemon();
    }

    if (!dispatched) {
        // ---- 首次调用:派发后台重入 ----
        // 无 root:经 su 重新拉起
        if (!am_root()) {
            std::string su = find_su();
            if (!su.empty()) {
                char cmd[1024];
                snprintf(cmd, sizeof(cmd),
                         "%s -c '%s --start' >/dev/null 2>&1 &",
                         su.c_str(), self);
                if (system(cmd) != 0) {
                    printf("{\"status\": \"error_start\"}\n");
                    return 1;
                }
                printf("{\"status\": \"starting_su\"}\n");
                return 0;
            }
            printf("{\"status\": \"error_unauthorized\"}\n");
            return 1;
        }
        // 有 root:shell 防重入拉起(nohup + 环境变量)
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
                 "export ADK_NFQ_START_DISPATCHED=1 ADK_NFQ_START_REVISION=%s ADK_NFQ_START_BOOT=%d; mkdir -p '%s'; "
                 "umask 022; cd /; nohup '%s' --start >/dev/null 2>&1 < /dev/null &",
                 request.revision.c_str(), request.boot ? 1 : 0, DATA_DIR, self);
        if (system(cmd) != 0) {
            printf("{\"status\": \"error_start\"}\n");
            return 1;
        }
        printf("{\"status\": \"starting\"}\n");
        return 0;
    }

    // ---- 重入路径(DISPATCHED 已设):daemonize 后进入引擎 ----
    pid_t pid = fork();
    if (pid < 0) {
        fail_engine_start("fork_failed", "守护进程 fork 失败");
        printf("{\"status\": \"fork_failed\"}\n");
        return 1;
    }
    if (pid > 0) {
        // 中间父进程退出;守护由孙进程接手
        return 0;
    }
    // 子进程:setsid + 二次 fork 彻底脱离会话(孙进程被 init 收养)
    if (setsid() < 0) { fail_engine_start("setsid_failed", "守护进程 setsid 失败"); _exit(127); }
    signal(SIGHUP, SIG_IGN);
    pid_t pid2 = fork();
    if (pid2 < 0) { fail_engine_start("fork_failed", "守护进程二次 fork 失败"); _exit(127); }
    if (pid2 > 0) _exit(0);

    // daemon 自己通过 bounded logger 写 engine.log；stdio 只负责彻底脱离调用会话。
    int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) close(devnull);
    }
    umask(022);
    chdir("/");
    // exec 自身为 --daemon:cmdline 含 "--daemon",便于 pgrep / pid 校验
    // 完整 argv[0] 用于身份校验，短 comm 名由 daemon 入口设置。
    execl(self, DAEMON_ARGV0, "--daemon", (char *)nullptr);
    fail_engine_start("exec_failed", "无法执行守护进程：" + std::string(strerror(errno)));
    _exit(127);   // exec 失败
}

// --daemon 入口(由 --start exec 而来):写 pid 后进入引擎
static int cmd_daemon() {
    if (!ensure_dirs()) return 1;
    std::string intent_error;
    auto operation = std::make_unique<ExclusiveFileLock>(DATA_DIR "/.control.lock", true, 12000);
    if (!operation->ok()) return 1;
    const char *revision = getenv("ADK_NFQ_START_REVISION");
    const char *boot_request = getenv("ADK_NFQ_START_BOOT");
    const lifecycle::Ticket request{revision ? revision : "", boot_request && strcmp(boot_request, "1") == 0};
    if (!lifecycle::current(DATA_DIR, request)) {
        printf("{\"status\":\"error_start_cancelled\",\"message\":\"启动已取消；前台调试请显式使用 --daemon --foreground\"}\n");
        return 1;
    }
    ExclusiveFileLock daemon_lock(DAEMON_LOCK_FILE);
    if (!daemon_lock.ok()) {
        return 0;
    }
    configure_process_log(FILE_ENGINE_LOG, ENGINE_LOG_MAX_BYTES, LOG_BACKUP_COUNT);
    if (!write_file_atomic(FILE_ENGINE_ERROR, "")) return 1;
    // Direct --daemon (including recovery after SIGKILL) must not expose an old
    // "running" state while its queues/routes are still being rebuilt.
    if (!write_runtime_state("starting")) {
        log_line("致命", "无法写入启动状态,拒绝接管网络");
        return 1;
    }
    // Also enforce this for direct --daemon and boot/recovery entry points,
    // before a PID is published, workers are created or queues are opened.
    if (!prepare_daemon_cgroups()) return 1;
    int self_pid = (int)getpid();
    if (!lifecycle::publish(DATA_DIR, request, [&] { return write_daemon_pid(self_pid); }, intent_error)) {
        // A newer STOP may be waiting for the operation lock. Do not overwrite
        // its preference or expose a PID/queue from the cancelled request.
        log_line("系统", "启动未接管网络: %s", intent_error.c_str());
        if (intent_error == "PID publication failed") fail_engine_start("pid_write_failed", intent_error);
        else write_runtime_state("stopped");
        return 1;
    }
    operation.reset(); // Stop can now find this PID; daemon retains lifetime lock.
    int rc = run_daemon();
    clear_daemon_pid_if(self_pid);
    return rc;
}

// ---------------------------------------------------------------------------
// --stop:安全停止
// 流程:runtime.state="stopping" → 找守护进程(pgrep)
// → SIGTERM 优雅退出 → 超时强杀 → 拔规则 → state="stopped" + {"status": "stopped"}
static int cmd_stop() {
    lifecycle::Ticket request;
    std::string error;
    if (!lifecycle::begin(DATA_DIR, lifecycle::Action::STOP, request, error)) {
        printf("{\"status\":\"error_storage\",\"message\":%s}\n", source_json_string(error).c_str()); return 1;
    }
    configure_process_log(FILE_ENGINE_LOG, ENGINE_LOG_MAX_BYTES, LOG_BACKUP_COUNT);
    ExclusiveFileLock operation(DATA_DIR "/.control.lock", true, 12000);
    if (!operation.ok()) { printf("{\"status\":\"error_busy\"}\n"); return 1; }
    if (!lifecycle::current(DATA_DIR, request)) { printf("{\"status\":\"cancelled\"}\n"); return 0; }
    write_runtime_state("stopping");
    int pid = find_daemon_pid();
    if (pid > 0 && is_daemon_pid(pid)) {
        log_line("系统", "已向守护进程发送停止信号,等待安全退出");
        kill(pid, SIGTERM);
        // 轮询等待优雅退出
        for (int i = 0; i < STOP_WAIT_MS / 100; i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!is_daemon_pid(pid)) { pid = -1; break; }
        }
        if (pid > 0 && is_daemon_pid(pid)) {
            // 超时:强杀兜底
            log_line("系统", "守护进程退出超时,执行强制停止兜底");
            kill(pid, SIGKILL);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }
    // 守护进程持有生命周期锁并负责清理自己的 PID。强杀留下的旧 PID 会在
    // read_daemon_pid() 中因 /proc 身份校验失效；这里不主动 unlink，避免
    // 并发新实例刚写入 PID 时被旧 stop 命令误删。
    // Nonblocking proof after the signal/wait phase, never wait on the lock of
    // the daemon we are trying to stop. It also covers a missing PID/pgrep.
    ExclusiveFileLock lifetime(DAEMON_LOCK_FILE);
    const auto cleanup = iptables_remove_all();
    const bool process_gone = lifetime.ok() && find_daemon_pid() <= 0;
    const auto queues = inspect_queue_release(is_daemon_pid);
    const bool complete = process_gone && cleanup.ok() && queues.ok();
    std::string message;
    if (!process_gone) message += "守护仍存在，或生命周期锁未释放/不可核实；";
    if (!cleanup.ok()) message += "双栈模块路由清理为 " + std::string(cleanup.name()) + "；";
    if (!queues.ok()) message += queues.message + "；";
    for (const auto &error : cleanup.errors) log_line("清理诊断", "%s", error.c_str());
    const char *code = cleanup.state == FirewallCleanupState::UNKNOWN || queues.state == QueueReleaseState::UNKNOWN
        ? "cleanup_unknown" : "cleanup_incomplete";
    bool saved = false;
    if (complete) {
        const auto previous = read_conf_value(FILE_ENGINE_ERROR, "code");
        const bool error_cleared = previous.compare(0, 8, "cleanup_") != 0 || write_file_atomic(FILE_ENGINE_ERROR, "");
        saved = write_runtime_state("stopped") && error_cleared;
        message = saved ? "引擎已停止，模块路由与队列已核实清理" : "清理已完成，但停止状态未能保存，请重新核实";
    } else {
        for (const auto &error : cleanup.errors) if (message.size() < 1024) message += error + "；";
        record_engine_error(code, message);
        write_runtime_state("error");
    }
    printf("{\"status\":%s,\"message\":%s,\"cleanup\":{\"routes\":%s,\"process_gone\":%s,\"queues\":%s,\"remaining\":[",
        source_json_string(complete ? (saved ? "stopped" : "error_storage") : (std::string("error_") + code)).c_str(),
        source_json_string(message).c_str(), source_json_string(cleanup.name()).c_str(), process_gone ? "true" : "false", source_json_string(queues.name()).c_str());
    for (size_t i = 0; i < cleanup.remaining.size(); ++i) printf("%s%s", i ? "," : "", source_json_string(cleanup.remaining[i]).c_str());
    printf("],\"errors\":[");
    for (size_t i = 0; i < cleanup.errors.size(); ++i) printf("%s%s", i ? "," : "", source_json_string(cleanup.errors[i]).c_str());
    printf("]}}\n");
    return complete && saved ? 0 : 1;
}

// ---------------------------------------------------------------------------
// --reload:热重载(运行中通知 SIGHUP;未运行报错)
// 输出:{"status": "reloaded"} / {"status": "error"}
static bool mark_policy_dirty() {
    std::string error;
    if (apply_state::dirty(DATA_DIR, error)) return true;
    fprintf(stderr, "%s\n", error.c_str()); return false;
}
static bool write_policy_value(const char *key, const char *value) {
    return write_conf_value_with_lock(FILE_STATE_CONF, key, value, STATE_LOCK_FILE, mark_policy_dirty);
}
static bool setting_matches(const char *key, const char *expected) {
    if (read_conf_value(FILE_STATE_CONF, key) == expected) return true;
    printf("{\"status\":\"superseded\",\"message\":\"配置已被其他窗口修改，请重新读取\"}\n");
    return false;
}
static int submit_apply_locked(const std::string &extra = "", bool emit = true) {
    std::string revision, error;
    if (!apply_state::queue(DATA_DIR, revision, error)) {
        if (emit) printf("{\"status\":\"error_apply_submission\",\"message\":%s%s}\n", source_json_string(error).c_str(), extra.c_str());
        else log_line("错误", "%s", error.c_str());
        return 1;
    }
    const int pid = find_daemon_pid();
    const bool signalled = pid > 0 && kill(pid, SIGHUP) == 0;
    if (emit) printf("{\"status\":%s,\"revision\":%s,\"signalled\":%s%s}\n",
        source_json_string(pid > 0 ? "queued" : "deferred").c_str(), source_json_string(revision).c_str(),
        signalled ? "true" : "false", extra.c_str());
    return 0; // queued is durable, never a claim that it has been applied
}
static int cmd_reload(const std::string &target = "", const std::string &expected = "") {
    ConfigInputLock inputs(DATA_DIR);
    if (!inputs.ok()) { printf("{\"status\":\"busy\"}\n"); return 2; }
    if (!target.empty()) {
        const auto value = edit_config::read(DATA_DIR, target);
        if (value.status != "ok" || value.revision != expected) {
            printf("{\"status\":\"superseded\",\"message\":\"配置已被其他修改替代，请重新读取\"}\n"); return 2;
        }
    }
    return submit_apply_locked();
}

// ---------------------------------------------------------------------------
// --mode N:切换规则强度(写配置;运行中热重载)
static int cmd_mode(int mode) {
    configure_process_log(FILE_ENGINE_LOG, ENGINE_LOG_MAX_BYTES, LOG_BACKUP_COUNT);
    if (!valid_mode(mode)) {
        fprintf(stderr, "参数错误\n用法: AD_Killer_By_NFQUEUE --mode 1|2 (1=仅Hosts 2=Hosts+AdGuard)\n");
        return 1;
    }
    char v[8];
    snprintf(v, sizeof(v), "%d", mode);
    if (!write_policy_value("mode", v)) {
        fprintf(stderr, "配置写入失败: mode\n");
        return 1;
    }
    ConfigInputLock inputs(DATA_DIR);
    if (!inputs.ok()) { printf("{\"status\":\"busy\"}\n"); return 2; }
    if (!setting_matches("mode", v)) return 2;
    return submit_apply_locked(",\"mode\":" + std::to_string(mode));
}

// ---------------------------------------------------------------------------
// --scope 0|1:切换作用域并让运行中的守护进程重建路由。
static int cmd_apps_apply(const std::string &target = "", const std::string &expected = "", int expected_scope = -1);
static int cmd_scope(int scope) {
    configure_process_log(FILE_ENGINE_LOG, ENGINE_LOG_MAX_BYTES, LOG_BACKUP_COUNT);
    if (!valid_scope(scope)) {
        fprintf(stderr, "参数错误\n用法: AD_Killer_By_NFQUEUE --scope 0|1 (0=全局 1=仅名单应用)\n");
        return 1;
    }
    char value[8];
    snprintf(value, sizeof(value), "%d", scope);
    if (!write_policy_value("scope", value)) {
        fprintf(stderr, "配置写入失败: scope\n");
        return 1;
    }
    return cmd_apps_apply("", "", scope); // one coherent scope + UID-cache submission
}

// ---------------------------------------------------------------------------
// --dev / --quic / --autostart on|off（自启只修改下次开机策略）
static int cmd_toggle(const char *key, bool on) {
    configure_process_log(FILE_ENGINE_LOG, ENGINE_LOG_MAX_BYTES, LOG_BACKUP_COUNT);
    std::string error;
    const bool saved = strcmp(key, "autostart") == 0 ? lifecycle::set_autostart(DATA_DIR, on, error)
        : strcmp(key, "block_quic") == 0 ? write_policy_value(key, on ? "true" : "false")
        : write_conf_value(FILE_STATE_CONF, key, on ? "true" : "false");
    if (!saved) {
        fprintf(stderr, "配置写入失败: %s\n", key);
        return 1;
    }
    if (strcmp(key, "autostart") == 0) {
        // Only persist boot policy: do not start/stop/reload the running engine.
        printf("{\"autostart\": %s}\n", on ? "true" : "false");
    } else if (strcmp(key, "dev_mode") == 0) {
        // 写/删 dev_mode_enabled 标志文件
        if (on) {
            FILE *f = fopen(FILE_DEV_MODE_ENABLED, "w");
            if (f) { fputs("1", f); fclose(f); }
        } else {
            unlink(FILE_DEV_MODE_ENABLED);
        }
        log_line("系统", "%s DNS 流量日志", on ? "已开启" : "已关闭");
        printf("{\"dev_mode\": \"done\"}\n");
    } else {
        // QUIC 切换需要重建 iptables → 通知守护进程热重载
        ConfigInputLock inputs(DATA_DIR);
        if (!inputs.ok()) { printf("{\"status\":\"busy\"}\n"); return 2; }
        if (!setting_matches("block_quic", on ? "true" : "false")) return 2;
        return submit_apply_locked(std::string(",\"block_quic\":") + (on ? "true" : "false"));
    }
    return 0;
}

// ---------------------------------------------------------------------------
// --apps-apply:应用名单落地(WebUI 保存名单文件后调用)
// 流程:读名单(scope 决定读哪个文件)→ 包名解析 uid(优先 packages.list,
// 兜底 cmd package list packages -U)→ 写 uid 缓存 → 通知守护进程热重载
// 输出:{"status": "applied"} / {"status": "not_running"}
static int cmd_apps_apply(const std::string &target, const std::string &expected, int expected_scope) {
    configure_process_log(FILE_ENGINE_LOG, ENGINE_LOG_MAX_BYTES, LOG_BACKUP_COUNT);
    if (!ensure_dirs()) {
        printf("{\"status\": \"error_storage\"}\n");
        return 1;
    }
    // Same order as the daemon's candidate capture. Keep selected packages,
    // scope and the generated UID cache from different saves from interleaving.
    ConfigInputLock inputs(DATA_DIR);
    if (!inputs.ok()) {
        printf("{\"status\":\"busy\",\"message\":\"配置正在保存，请稍后重试\"}\n");
        return 2;
    }
    int scope = 0;
    {
        std::string s = read_conf_value(FILE_STATE_CONF, "scope");
        scope = (s == "1") ? SCOPE_PER_APP : SCOPE_GLOBAL;
    }
    const auto selected_list = edit_config::read(DATA_DIR, scope == SCOPE_PER_APP ? "intercept" : "allow");
    if (selected_list.status != "ok") {
        printf("{\"status\":\"error_config_read\",\"message\":%s}\n", source_json_string(selected_list.message).c_str()); return 1;
    }
    const auto invalid = edit_config::validate_content(scope == SCOPE_PER_APP ? "intercept" : "allow", selected_list.content);
    if (!invalid.empty()) {
        printf("{\"status\":\"error_config_content\",\"message\":%s}\n", source_json_string(invalid).c_str()); return 1;
    }
    if (expected_scope >= 0 && expected_scope != scope) {
        printf("{\"status\":\"superseded\",\"message\":\"拦截范围已被其他窗口修改\"}\n"); return 2;
    }
    if (!target.empty()) {
        const auto value = edit_config::read(DATA_DIR, target);
        if (target != (scope == SCOPE_PER_APP ? "intercept" : "allow") || value.status != "ok" || value.revision != expected) {
            printf("{\"status\":\"superseded\",\"message\":\"范围或名单已变更，请重新读取\"}\n"); return 2;
        }
    }

    // 1. 读取包名名单
    std::vector<std::string> pkgs;
    std::unordered_set<std::string> unique_pkgs;
    {
        const std::string &content = selected_list.content;
        {
            std::istringstream lines(content);
            std::string line;
            while (std::getline(lines, line)) {
                size_t b = line.find_first_not_of(" \t\r");
                if (b != std::string::npos) {
                    size_t e = line.find_last_not_of(" \t\r");
                    line = line.substr(b, e - b + 1);
                    if (!line.empty() && line[0] != '#' &&
                        valid_package_name(line) && unique_pkgs.insert(line).second)
                        pkgs.push_back(line);
                }
            }
        }
    }

    // 2. 构建 包名→uid 映射
    //    优先:/data/system/packages.list(每行 "pkg uid debugFlag dataDir seinfo",免子进程)
    //    兜底:cmd package list packages -U(输出 "package:<pkg> uid:<n>")
    std::string uid_map = "# packages_revision=" + selected_list.revision + "\n";
    {
        std::unordered_map<std::string, long> pkg2uid;
        std::string pl;
        if (!pkgs.empty() && read_regular_bounded("/data/system/packages.list", pl, 8u * 1024u * 1024u) == BoundedRead::OK) {
            // 快速路径:直接解析 packages.list
            std::istringstream ss(pl);
            std::string line;
            while (std::getline(ss, line)) {
                std::istringstream row(line);
                std::string pkg;
                long uid = -1;
                if (row >> pkg >> uid && valid_package_name(pkg) && uid > 0 && static_cast<uint64_t>(uid) <= UINT32_MAX)
                    pkg2uid[pkg] = uid;
            }
        }
        if (!pkgs.empty() && pkg2uid.empty()) {
            // 兜底链:cmd package → pm list
            const std::vector<std::vector<std::string>> queries = {
                {"cmd", "package", "list", "packages", "-U"}, {"pm", "list", "packages", "-U"}};
            for (const auto &query : queries) {
                const auto result = run_process(query, 2000, 8u * 1024u * 1024u);
                if (!result.ok()) continue;
                std::istringstream output(result.output); std::string row;
                while (std::getline(output, row)) {
                    if (row.compare(0, 8, "package:") != 0) continue;
                    const size_t marker = row.find(" uid:", 8);
                    if (marker == std::string::npos) continue;
                    std::string pkg = row.substr(8, marker - 8);
                    char *end = nullptr;
                    long uid = strtol(row.c_str() + marker + 5, &end, 10);
                    if (valid_package_name(pkg) && end != row.c_str() + marker + 5 &&
                        uid > 0 && static_cast<uint64_t>(uid) <= UINT32_MAX && std::string(end).find_first_not_of(" \t\r") == std::string::npos)
                        pkg2uid[pkg] = uid;
                }
                if (!pkg2uid.empty()) break;
            }
        }
        if (!pkgs.empty() && pkg2uid.empty()) {
            printf("{\"status\":\"error_uid_resolution\",\"message\":\"无法核实已安装应用的 UID，原缓存与旧策略未改变\"}\n");
            return 1;
        }
        // 解析名单中每个包的 uid
        for (auto &pkg : pkgs) {
            long uid = -1;
            auto found = pkg2uid.find(pkg);
            if (found != pkg2uid.end()) uid = found->second;
            if (uid > 0) {
                char row[512];
                snprintf(row, sizeof(row), "%ld %s\n", uid, pkg.c_str());
                uid_map += row;
                log_line("应用", "%s 已解析 UID: %ld", pkg.c_str(), uid);
            } else {
                log_line("应用", "未安装或无法解析,跳过: %s", pkg.c_str());
            }
        }
    }
    // 3. 写 uid 缓存(scope=1 拦截名单用独立文件)
    std::string uid_cache_path = (scope == SCOPE_PER_APP)
        ? DATA_FILE("app_intercept_uids.conf")
        : DATA_FILE("app_uids.conf");
    if (uid_map.size() > edit_config::MAX_BYTES) {
        printf("{\"status\":\"error_capacity\",\"message\":\"UID 缓存超过容量上限，原缓存未替换\"}\n"); return 1;
    }
    const auto cache_saved = mark_policy_dirty() ? write_file_durable_atomic(uid_cache_path.c_str(), uid_map) : AtomicWriteResult{};
    if (!cache_saved.committed || !cache_saved.durable) {
        log_line("应用", "UID 缓存写入失败: %s", uid_cache_path.c_str());
        printf("{\"status\": \"error_storage\"}\n");
        return 1;
    }
    if (pkgs.empty()) {
        log_line("应用", "按应用拦截列表为空,当前没有应用被%s",
                 scope == SCOPE_PER_APP ? "拦截" : "放行");
    }

    // 4. 通知守护进程热重载(重读 uid 缓存并同步 selector 链)
    // 运行中 applied+scope;未运行 deferred+scope(下次启动生效)
    return submit_apply_locked(",\"scope\":" + std::to_string(scope));
}

// ---------------------------------------------------------------------------
// --update:拉取规则源(支持 gzip)→ 逐行归类 → 原子落盘 → 热重载
// 失败输出:{"status": "error_download"}
static int cmd_update(const std::string &test_url = "") {
    if (!ensure_dirs()) {
        printf("{\"status\": \"error_storage\"}\n");
        return 1;
    }
    ExclusiveFileLock update_lock(UPDATE_LOCK_FILE);
    if (!update_lock.ok()) {
        log_line("更新", "已有规则更新正在运行");
        printf("{\"status\": \"busy\"}\n");
        return 2;
    }
    std::string sources;
    if (test_url.empty() && !read_file(FILE_UPDATE_SOURCES, sources)) {
        log_line("更新", "未找到规则源配置 %s", FILE_UPDATE_SOURCES);
        printf("{\"status\": \"error_download\"}\n");
        return 1;
    }
    // Test jobs share the download/validation path and resource bounds but
    // return before staging, publishing, statistics writes or daemon signals.
    std::vector<std::string> urls;
    if (!test_url.empty()) urls.push_back(test_url);
    else {
        std::vector<RuleSource> entries;
        std::string error;
        if (!parse_rule_sources(sources, entries, error)) {
            log_line("更新", "来源配置错误: %s", error.c_str());
            return 1;
        }
        retain_builtin_source(entries);
        for (const auto &entry : entries) if (entry.enabled) urls.push_back(entry.url);
    }
    if (urls.empty()) {
        write_file_atomic(FILE_SOURCE_STATS, serialize_source_reports(
            {}, "no_enabled_sources", now_epoch()));
        log_line("更新", "规则源为空");
        printf("{\"status\": \"error_download\"}\n");
        return 1;
    }

    std::string hosts_rules;      // 归类后的 Hosts 行
    std::string adguard_rules;    // 归类后的 AdGuard 行
    size_t aggregate_content_bytes = 0;
    size_t aggregate_source_lines = 0;
    std::vector<SourceUpdateReport> source_reports;
    PublishedSourceStats published;
    const char *update_job = getenv("ADK_NFQ_UPDATE_JOB");
    auto record_source_report = [&](SourceUpdateReport &report) {
        report.checked_at = now_epoch();
        source_reports.push_back(report);
    };

    auto save_source_reports = [&](const char *publish_status) {
        std::string report = serialize_source_reports(
            source_reports, publish_status, now_epoch(), published,
            update_job ? update_job : "");
        if (!write_file_atomic(FILE_SOURCE_STATS, report))
            log_line("更新", "来源统计写入失败: %s", FILE_SOURCE_STATS);
    };

    for (auto &url : urls) {
        SourceUpdateReport report;
        report.url = url;
        SourceFormat format = recipe_source_format(url);
        report.format = is_builtin_source(url) ? "hagezi-pro-minus-fake" : source_format_name(format);
        RuleStreamLimits source_limits;
        source_limits.output_bytes = MAX_RULE_DECOMPRESSED_BYTES - aggregate_content_bytes;
        source_limits.lines = MAX_RULE_LINES - aggregate_source_lines;
        SourceValidationResult validation;
        if (is_builtin_source(url)) {
            validation = fetch_hagezi_source(DATA_DIR, source_limits, download_file, report.downloaded_bytes);
        } else {
            log_line("更新", "拉取: %s", url.c_str());
            std::string part = std::string(DATA_DIR) + "/download.part";
            std::string err = download_file(url, part);
            if (!err.empty()) {
                log_line("更新", "下载失败: %s (%s)", url.c_str(), err.c_str());
                report.status = "download_error";
                report.reason = err;
                record_source_report(report);
                continue;
            }
            struct stat downloaded {};
            if (stat(part.c_str(), &downloaded) == 0 && downloaded.st_size > 0)
                report.downloaded_bytes = static_cast<size_t>(downloaded.st_size);
            // Same bounded plain/gzip stream path for all ordinary sources.
            validation = validate_rule_source_file(part.c_str(), source_limits, format);
            unlink(part.c_str());
        }
        report.stats = validation.stats;
        report.reason = validation.reason;
        if (!validation.accepted) {
            report.status = "content_rejected";
            log_line("更新", "来源内容拒绝: %s (%s,有效率 %.2f%%)",
                     url.c_str(), validation.reason.c_str(),
                     validation.stats.valid_ratio_ppm / 10000.0);
            record_source_report(report);
            continue;
        }
        size_t aggregate_bytes = hosts_rules.size() + adguard_rules.size();
        size_t source_bytes = validation.hosts_rules.size() +
                              validation.adguard_rules.size();
        if (source_bytes > MAX_RULE_DECOMPRESSED_BYTES - aggregate_bytes ||
            validation.stats.total_lines > MAX_RULE_LINES - aggregate_source_lines) {
            report.status = "content_rejected";
            report.reason = "aggregate_resource_limit";
            log_line("更新", "来源累计资源上限,跳过: %s", url.c_str());
            record_source_report(report);
            continue;
        }
        report.status = "accepted";
        hosts_rules += validation.hosts_rules;
        adguard_rules += validation.adguard_rules;
        aggregate_content_bytes += validation.stats.bytes;
        aggregate_source_lines += validation.stats.total_lines;
        log_line("更新", "来源接受:Hosts %zu / AdGuard %zu / 拒绝 %zu / 有效率 %.2f%%",
                 validation.stats.accepted_hosts,
                 validation.stats.accepted_adguard,
                 validation.stats.rejected_unsupported +
                     validation.stats.rejected_too_long,
                 validation.stats.valid_ratio_ppm / 10000.0);
        record_source_report(report);
    }

    if (!test_url.empty()) {
        bool accepted = !source_reports.empty() && source_reports[0].status == "accepted";
        log_line("测试", "%s；仅测试下载和解析，未替换规则或启动引擎",
                 accepted ? "来源测试通过" : "来源测试失败");
        return accepted ? 0 : 1;
    }

    // 全部源失败 → error_download
    if (hosts_rules.empty() && adguard_rules.empty()) {
        save_source_reports("no_acceptable_sources");
        log_line("更新", "规则同步失败:所有源均不可用");
        printf("{\"status\": \"error_download\"}\n");
        return 1;
    }

    // 两个 gzip 先进入不可见 staging,用真实规则加载器校验后只切换一次指针。
    deduplicate_rule_text(hosts_rules);
    deduplicate_rule_text(adguard_rules);
    std::string gz_hosts, gz_adguard;
    // A source set may legitimately contain only one category (the default
    // sources are AdGuard-only). Keep both generated files nonempty text with
    // a comment header; do not relax empty/corrupt upstream stream validation.
    if (!gz_compress_string("# AD_Killer_By_NFQUEUE generated Hosts rules\n" + hosts_rules, gz_hosts) ||
        !gz_compress_string("# AD_Killer_By_NFQUEUE generated AdGuard rules\n" + adguard_rules, gz_adguard)) {
        save_source_reports("publish_failed");
        log_line("更新", "规则 staging 压缩失败,保留旧规则");
        printf("{\"status\": \"error_download\"}\n");
        return 1;
    }
    long long generation_clock = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string generation = std::to_string(generation_clock) + "-" +
                             std::to_string((long long)getpid());
    StagedRuleBundle staged;
    std::string stage_error;
    if (!stage_rule_bundle(DATA_DIR, generation, gz_hosts, gz_adguard,
                           staged, &stage_error)) {
        save_source_reports("publish_failed");
        log_line("更新", "规则 staging 失败: %s,保留旧规则", stage_error.c_str());
        printf("{\"status\": \"error_download\"}\n");
        return 1;
    }

    RuleDB candidate;
    int staged_hosts = 0, staged_adguard = 0;
    bool hosts_valid = load_rule_file_checked(
        staged.hosts_path.c_str(), false, candidate, &staged_hosts, true);
    bool adguard_valid = load_rule_file_checked(
        staged.adguard_path.c_str(), true, candidate, &staged_adguard, true);
    if (!hosts_valid || !adguard_valid || staged_hosts + staged_adguard <= 0 ||
        candidate.black.size() + candidate.white.size() == 0) {
        discard_staged_rule_bundle(staged);
        save_source_reports("publish_failed");
        log_line("更新", "staging 规则无法被引擎加载(Hosts 校验=%d 行=%d / AdGuard 校验=%d 行=%d),保留旧规则",
                 hosts_valid, staged_hosts, adguard_valid, staged_adguard);
        printf("{\"status\": \"error_download\"}\n");
        return 1;
    }
    bool committed = [&] {
        ConfigInputLock inputs(DATA_DIR);
        if (!inputs.ok() || !mark_policy_dirty()) { stage_error = "配置保存或生效回执不可用，暂未发布"; return false; }
        if (!commit_rule_bundle(FILE_RULESET_CURRENT, staged, &stage_error)) return false;
        // Publication success is distinct from applying it to a live daemon.
        if (submit_apply_locked("", false)) log_line("更新", "规则已发布，应用请求尚未确认；请重新提交热重载");
        return true;
    }();
    if (!committed) {
        discard_staged_rule_bundle(staged);
        save_source_reports("publish_failed");
        log_line("更新", "规则指针提交失败: %s,保留旧规则", stage_error.c_str());
        printf("{\"status\": \"error_download\"}\n");
        return 1;
    }
    log_line("更新", "规则 generation %s 已原子发布(引擎可加载 %d/%d 行)",
             generation.c_str(), staged_hosts, staged_adguard);
    published = {generation, candidate.black.size(), candidate.white.size()};
    size_t accepted_sources = 0, accepted_lines = 0, filtered_lines = 0;
    for (const auto &report : source_reports) {
        if (report.status == "accepted") {
            accepted_sources++;
            accepted_lines += report.stats.accepted_hosts + report.stats.accepted_adguard;
        }
        filtered_lines += report.stats.rejected_unsupported + report.stats.rejected_too_long;
    }
    size_t unique = published.unique_black + published.unique_white;
    log_line("更新", "来源 %zu/%zu 成功；过滤 %zu 行；接受 %zu 行，去重 %zu 行，来源唯一规则 %zu 条(含白名单 %zu)",
             accepted_sources, urls.size(), filtered_lines, accepted_lines,
             accepted_lines >= unique ? accepted_lines - unique : 0, unique, published.unique_white);
    if (accepted_sources != urls.size())
        log_line("更新", "注意：部分来源失败，本次仅发布成功来源；请检查上方失败原因");
    save_source_reports("published");
    // Synchronization is allowed while the engine is paused. Refresh the
    // current-mode/custom-rule counters without starting an NFQUEUE listener.
    candidate = RuleDB{};
    load_all_rules(candidate);
    if (candidate.load_valid) save_rule_stats(candidate, FILE_AVAILABLE_RULE_STATS);
    else log_line("更新", "规则已发布，但当前模式计数刷新失败: %s", candidate.load_error.c_str());
    // 运行中热重载
    return 0;
}

static std::string update_job_path(const std::string &job_id, const char *suffix) {
    return std::string(DIR_UPDATE_JOBS) + "/update-" + job_id + suffix;
}

// 由后端创建唯一任务并脱离 WebUI exec 会话。状态和日志均按 job_id 隔离。
static int cmd_update_start(const std::string &test_url = "", const std::string &module_version = "", bool install_module = false, bool module_preview = false) {
    std::string error;
    if (!test_url.empty() && !is_builtin_source(test_url) && !validate_download_url(test_url, &error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (!ensure_dirs()) {
        printf("{\"status\": \"error_storage\"}\n");
        return 1;
    }
    prune_update_jobs(DIR_UPDATE_JOBS, UPDATE_JOB_KEEP_COUNT);
    long long clock = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string job_id = std::to_string(clock) + "-" +
                         std::to_string((long long)getpid());
    std::string state_path = update_job_path(job_id, ".state");
    std::string log_path = update_job_path(job_id, ".log");

    int gate[2];
    if (pipe(gate) != 0) {
        printf("{\"status\": \"error\"}\n");
        return 1;
    }
    pid_t child = fork();
    if (child < 0) {
        close(gate[0]);
        close(gate[1]);
        printf("{\"status\": \"error\"}\n");
        return 1;
    }
    if (child == 0) {
        close(gate[1]);
        char start = 0;
        ssize_t received;
        do {
            received = read(gate[0], &start, 1);
        } while (received < 0 && errno == EINTR);
        close(gate[0]);
        if (received != 1 || start != '1') _exit(1);

        setsid();
        int null_fd = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null_fd >= 0) {
            dup2(null_fd, STDIN_FILENO);
            dup2(null_fd, STDOUT_FILENO);
            dup2(null_fd, STDERR_FILENO);
            if (null_fd > STDERR_FILENO) close(null_fd);
        }
        configure_process_log(log_path.c_str(), UPDATE_LOG_MAX_BYTES, 0);
        if (setenv("ADK_NFQ_UPDATE_JOB", job_id.c_str(), 1) != 0) _exit(1);
        auto isolated = isolate_daemon_cgroups();
        if (!isolated.ok) {
            log_line("错误", "后台任务无法脱离调用方冻结组：%s", isolated.error.c_str());
            write_file_atomic(state_path.c_str(), finished_update_job_state(1));
            _exit(1);
        }
        int result = module_version.empty() ? cmd_update(test_url) : module_update::worker(module_version, install_module, module_preview);
        write_file_atomic(state_path.c_str(), finished_update_job_state(result));
        _exit(result);
    }

    close(gate[0]);
    bool state_ready = write_file_atomic(
        state_path.c_str(), running_update_job_state(static_cast<int>(child)));
    char start = state_ready ? '1' : '0';
    ssize_t ignored = write(gate[1], &start, 1);
    (void)ignored;
    close(gate[1]);
    if (!state_ready) {
        kill(child, SIGKILL);
        waitpid(child, nullptr, 0);
        printf("{\"status\": \"error\"}\n");
        return 1;
    }
    printf("{\"status\": \"started\", \"job_id\": \"%s\"}\n",
           job_id.c_str());
    return 0;
}

static int cmd_update_status(const std::string &job_id) {
    if (!valid_update_job_id(job_id)) {
        printf("{\"status\": \"invalid_job\"}\n");
        return 1;
    }
    std::string raw;
    if (!read_file(update_job_path(job_id, ".state").c_str(), raw)) {
        printf("{\"status\": \"not_found\", \"job_id\": \"%s\"}\n",
               job_id.c_str());
        return 1;
    }
    UpdateJobState state;
    if (!parse_update_job_state(raw, state)) {
        printf("{\"status\": \"invalid_state\", \"job_id\": \"%s\"}\n",
               job_id.c_str());
        return 1;
    }
    if (state.status == "running") {
        if (kill(state.pid, 0) != 0 && errno == ESRCH) {
            printf("{\"status\": \"interrupted\", \"job_id\": \"%s\", "
                   "\"exit_code\": 255}\n", job_id.c_str());
        } else {
            printf("{\"status\": \"running\", \"job_id\": \"%s\", "
                   "\"exit_code\": null}\n", job_id.c_str());
        }
        return 0;
    }
    printf("{\"status\": \"%s\", \"job_id\": \"%s\", \"exit_code\": %d}\n",
           state.status.c_str(), job_id.c_str(), state.exit_code);
    return 0;
}

// ---------------------------------------------------------------------------
static apply_state::View current_apply(int pid, const std::string &expected = "") {
    const auto live = pid > 0 ? process_identity(pid) : ProcessIdentity{};
    auto view = apply_state::view(apply_state::read(DATA_DIR), live, pid > 0, expected);
    if (pid > 0 && read_conf_value(FILE_ENGINE_ERROR, "code") == "apply_ack_failed") {
        // A rename may be visible despite a failed directory sync. Never
        // present that receipt as confirmed while its owner is fault-closing.
        view.phase = "unverified"; view.actual_verified = false;
    }
    return view;
}
static std::string apply_json(const apply_state::View &view) {
    const auto &record = view.record;
    std::ostringstream out;
    out << "{\"status\":" << source_json_string(view.phase) << ",\"verification\":\"owner_and_commit\""
        << ",\"desired_revision\":" << source_json_string(record.desired)
        << ",\"applied_revision\":" << source_json_string(view.actual_verified ? record.applied : "")
        << ",\"verified\":" << (view.actual_verified ? "true" : "false")
        << ",\"error\":" << source_json_string(record.error)
        << ",\"mode\":" << (view.actual_verified ? std::to_string(record.actual.config.mode) : "null")
        << ",\"scope\":" << (view.actual_verified ? std::to_string(record.actual.config.scope) : "null")
        << ",\"block_quic\":" << (view.actual_verified ? record.actual.config.block_quic ? "true" : "false" : "null")
        << ",\"generation\":" << source_json_string(view.actual_verified ? record.actual.generation : "")
        << ",\"rules_black\":" << (view.actual_verified ? std::to_string(record.actual.black) : "null")
        << ",\"rules_white\":" << (view.actual_verified ? std::to_string(record.actual.white) : "null") << "}";
    return out.str();
}
static int cmd_apply_status(const std::string &expected = "") {
    const auto view = current_apply(find_daemon_pid(), expected);
    puts(apply_json(view).c_str());
    return view.phase == "unverified" ? 1 : 0;
}
static void rule_counters(const apply_state::View &apply, int pid, long &black, long &white, long &updated) {
    if (apply.actual_verified) {
        black = static_cast<long>(apply.record.actual.black);
        white = static_cast<long>(apply.record.actual.white);
        updated = static_cast<long>(apply.record.actual.updated_at); return;
    }
    // Never label unrelated/stale file counters as the live daemon's rules.
    if (pid > 0) { black = white = updated = -1; return; }
    const char *path = access(FILE_AVAILABLE_RULE_STATS, F_OK) == 0 ? FILE_AVAILABLE_RULE_STATS : FILE_RULE_STATS;
    black = atol(read_conf_value(path, "black").c_str());
    white = atol(read_conf_value(path, "white").c_str());
    updated = atol(read_conf_value(path, "updated_at").c_str());
}

// --status:WebUI 状态 JSON
static int cmd_status() {
    int pid = read_daemon_pid();
    const auto freezer = pid > 0 ? daemon_cgroup_status(pid) : FreezerStatus{};
    bool running = pid > 0 && freezer.state != FreezerState::FROZEN;

    // 配置读取
    EngineConfig engine_cfg = load_engine_config();
    const auto apply = current_apply(pid);
    const EngineConfig &actual = apply.record.actual.config;
    IptablesConfig iptables_cfg;
    iptables_cfg.block_quic = actual.block_quic;
    iptables_cfg.scope = actual.scope;
    iptables_cfg.bypass_uids = actual.bypass_uids;
    iptables_cfg.intercept_uids = actual.intercept_uids;
    bool ipt_active = apply.actual_verified && iptables_chain_active(iptables_cfg);

    std::string enabled = read_conf_value(FILE_STATE_CONF, "enabled");
    std::string manual = read_conf_value(FILE_STATE_CONF, "manual_stop");
    std::string autostart = read_conf_value(FILE_STATE_CONF, "autostart");
    std::string metrics = current_metrics_json();
    long black = 0, white = 0, updated = 0;
    rule_counters(apply, pid, black, white, updated);
    printf("{\"running\": %s, \"iptables_active\": %s, \"mode\": %d, \"scope\": %d, "
           "\"dev_mode\": %s, \"block_quic\": %s, \"enabled\": %s, \"manual_stop\": %s, \"autostart\": %s, "
           "\"rules_black\": %ld, \"rules_white\": %ld, \"rules_updated_at\": %ld, "
           "\"ports\": \"53,80,443\", \"quic\": %s, \"table\": \"mangle\", "
           "\"metrics\": %s, \"state_file\": \"state.conf\", \"daemon_pid\":%d, \"daemon_image_current\":%s, \"route_check\":\"full\", \"apply\":%s, %s}\n",
           running ? "true" : "false",
           ipt_active ? "true" : "false",
           engine_cfg.mode, engine_cfg.scope,
           engine_cfg.dev_mode ? "true" : "false",
           engine_cfg.block_quic ? "true" : "false",
           (enabled == "false" || enabled == "0") ? "false" : "true",
           (manual == "true" || manual == "1") ? "true" : "false",
           (autostart == "true" || autostart == "1") ? "true" : "false",
           black, white, updated,
           engine_cfg.block_quic ? "true" : "false", metrics.c_str(), pid > 0 ? pid : 0,
           daemon_image_current(pid) ? "true" : "false", apply_json(apply).c_str(), engine_diagnostics_json(freezer, pid > 0).c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// --status-fast:低频概览轮询，只读核对双栈 OUTPUT 的唯一入口挂载。
// route_check=entry 不代表完整代际布局校验；异常时 WebUI 再调用 --status。
static int cmd_status_fast() {
    int pid = find_daemon_pid();
    const auto apply = current_apply(pid);
    const auto freezer = pid > 0 ? daemon_cgroup_status(pid) : FreezerStatus{};
    bool running = pid > 0 && freezer.state != FreezerState::FROZEN;
    int mode = MODE_HOSTS_ADGUARD;
    {
        std::string m = read_conf_value(FILE_STATE_CONF, "mode");
        if (m.empty()) m = read_conf_value(FILE_ADBLOCK_MODE, "mode");
        int parsed = 0;
        if (parse_integer_arg(m.c_str(), parsed) && valid_mode(parsed)) mode = parsed;
    }
    int scope = read_conf_value(FILE_STATE_CONF, "scope") == "1"
        ? SCOPE_PER_APP : SCOPE_GLOBAL;
    std::string dev = read_conf_value(FILE_STATE_CONF, "dev_mode");
    std::string quic = read_conf_value(FILE_STATE_CONF, "block_quic");
    std::string enabled = read_conf_value(FILE_STATE_CONF, "enabled");
    std::string manual = read_conf_value(FILE_STATE_CONF, "manual_stop");
    std::string autostart = read_conf_value(FILE_STATE_CONF, "autostart");
    std::string metrics = current_metrics_json();
    long black = 0, white = 0, updated = 0;
    rule_counters(apply, pid, black, white, updated);
    bool dev_on = (dev == "true" || dev == "1");
    bool quic_on = (quic == "true" || quic == "1");
    // A frozen worker is unhealthy, but its routes can still be installed.
    // Keep route presence independent from worker health, as --status does.
    const bool attached = pid > 0 && iptables_entry_attached();
    printf("{\"running\": %s, \"iptables_active\": %s, \"mode\": %d, \"scope\": %d, "
           "\"dev_mode\": %s, \"block_quic\": %s, \"enabled\": %s, \"manual_stop\": %s, \"autostart\": %s, "
           "\"rules_black\": %ld, \"rules_white\": %ld, \"rules_updated_at\": %ld, "
           "\"ports\": \"53,80,443\", \"quic\": %s, \"table\": \"mangle\", "
           "\"metrics\": %s, \"state_file\": \"state.conf\", \"daemon_pid\":%d, \"daemon_image_current\":%s, \"route_check\":\"entry\", \"apply\":%s, %s}\n",
           running ? "true" : "false",
           attached ? "true" : "false",
           mode, scope,
           dev_on ? "true" : "false",
           quic_on ? "true" : "false",
           (enabled == "false" || enabled == "0") ? "false" : "true",
           (manual == "true" || manual == "1") ? "true" : "false",
           (autostart == "true" || autostart == "1") ? "true" : "false",
           black, white, updated,
           quic_on ? "true" : "false", metrics.c_str(), pid > 0 ? pid : 0,
           daemon_image_current(pid) ? "true" : "false", apply_json(apply).c_str(), engine_diagnostics_json(freezer, pid > 0).c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// --version-status:版本 JSON
static int cmd_version_status() {
    printf("{\"module\": \"%s\", \"version\": \"%s\", \"versionCode\": %d, \"releaseChannel\": \"%s\"}\n",
           MODULE_ID, MODULE_VERSION, MODULE_VERSION_CODE, MODULE_RELEASE_CHANNEL);
    return 0;
}

// ---------------------------------------------------------------------------
// Source management only changes subscriptions, never active rules or boot state.
static int cmd_sources(bool save) {
    if (!ensure_dirs()) return 1;
    std::string text, error;
    if (save) {
        char buffer[4096];
        size_t n = 0;
        while ((n = fread(buffer, 1, sizeof(buffer), stdin)) != 0) {
            text.append(buffer, n);
            if (text.size() > MAX_SOURCE_CONFIG_BYTES) {
                fprintf(stderr, "来源配置过大\n");
                return 1;
            }
        }
        if (ferror(stdin)) return 1;
    } else if (!read_file(FILE_UPDATE_SOURCES, text)) {
        fprintf(stderr, "无法读取来源列表\n");
        return 1;
    }
    std::vector<RuleSource> entries;
    if (!parse_rule_sources(text, entries, error, save)) {
        fprintf(stderr, "%s\n", error.c_str());
        return 1;
    }
    if (save) {
        ExclusiveFileLock lock(UPDATE_LOCK_FILE);
        if (!lock.ok()) {
            fprintf(stderr, "规则同步或来源测试正在运行，请结束后保存\n");
            return 2;
        }
        std::string previous_text;
        std::vector<RuleSource> previous;
        if (read_file(FILE_UPDATE_SOURCES, previous_text) &&
            !parse_rule_sources(previous_text, previous, error)) {
            fprintf(stderr, "现有来源配置无法读取：%s\n", error.c_str());
            return 1;
        }
        retain_builtin_source(entries, previous);
        if (!write_initial_config(FILE_UPDATE_SOURCES, serialize_rule_sources(entries))) return 1;
    } else retain_builtin_source(entries);
    printf("{\"status\":\"ok\",\"sources\":[");
    for (size_t i = 0; i < entries.size(); i++) {
        if (i) printf(",");
        const bool builtin = is_builtin_source(entries[i].url);
        printf("{\"url\":%s,\"enabled\":%s,\"locked\":%s,\"name\":%s",
               source_json_string(entries[i].url).c_str(), entries[i].enabled ? "true" : "false",
               builtin ? "true" : "false", source_json_string(builtin ? HAGEZI_SOURCE_NAME : "").c_str());
        if (builtin)
            printf(",\"kind\":\"hagezi-pro-minus-fake\",\"dependencies\":[%s,%s]",
                   source_json_string(HAGEZI_PRO_URL).c_str(), source_json_string(HAGEZI_FAKE_URL).c_str());
        printf("}");
    }
    printf("]}\n");
    return 0;
}

// A read-only, bounded preview. Builtin recipes show both raw dependencies,
// explicitly not the post-difference rule set. No config/history/job writes.
static int cmd_source_preview(const std::string &url, size_t characters) {
    std::vector<std::string> urls;
    if (is_builtin_source(url)) urls = {HAGEZI_PRO_URL, HAGEZI_FAKE_URL};
    else {
        std::string error;
        if (!validate_download_url(url, &error)) {
            printf("{\"status\":\"error\",\"message\":%s}\n", source_json_string(error).c_str());
            return 1;
        }
        urls.push_back(url);
    }
    std::string entries;
    bool success = true;
    for (const auto &source : urls) {
        SourcePreview preview = preview_source(source, characters);
        if (!preview.error.empty()) success = false;
        if (!entries.empty()) entries += ',';
        entries += "{\"url\":" + source_json_string(source) + ",\"text\":" + source_json_string(preview.text) +
            ",\"error\":" + source_json_string(preview.error) + ",\"characters\":" + std::to_string(preview.characters) +
            ",\"truncated\":" + (preview.truncated ? "true" : "false") + "}";
    }
    net_curl_shutdown();
    printf("{\"status\":\"%s\",\"previews\":[%s]}\n", success ? "ok" : "error", entries.c_str());
    return success ? 0 : 1;
}

// 用法说明
static void usage() {
    fprintf(stderr,
            "AD_Killer_By_NFQUEUE %s - NFQUEUE 全局去广告引擎\n"
            "用法:\n"
            "  AD_Killer_By_NFQUEUE --install          安装初始化\n"
            "  AD_Killer_By_NFQUEUE --config-read custom|allow|intercept 安全读取配置及修订号\n"
            "  AD_Killer_By_NFQUEUE --config-save TARGET REVISION 从 stdin 校验并原子保存配置\n"
            "  AD_Killer_By_NFQUEUE --start            启动引擎(守护进程)\n"
            "  AD_Killer_By_NFQUEUE --start-boot       按最新自启/暂停策略启动，不恢复手动暂停\n"
            "  AD_Killer_By_NFQUEUE --daemon --foreground 显式前台启动(调试)，不双 fork\n"
            "  AD_Killer_By_NFQUEUE --stop             停止引擎并清理规则\n"
            "  AD_Killer_By_NFQUEUE --prepare-reboot  生命周期停止，不修改暂停或自启策略\n"
            "  AD_Killer_By_NFQUEUE --reload           热重载规则\n"
            "  AD_Killer_By_NFQUEUE --update           拉取规则源更新\n"
            "  AD_Killer_By_NFQUEUE --update-start     创建后台更新 job\n"
            "  AD_Killer_By_NFQUEUE --update-status ID 查询更新 job 状态\n"
            "  AD_Killer_By_NFQUEUE --module-update-check stable|preview 检查模块 Release\n"
            "  AD_Killer_By_NFQUEUE --module-release-notes 读取 GitHub 最新发布日志\n"
            "  AD_Killer_By_NFQUEUE --module-update-download VERSION [stable|preview] 后台下载并校验模块\n"
            "  AD_Killer_By_NFQUEUE --module-update-install VERSION [stable|preview] 后台安装已校验模块\n"
            "  AD_Killer_By_NFQUEUE --module-update-status 查询安装包与重启状态\n"
            "  AD_Killer_By_NFQUEUE --sources-list     读取启用/停用来源 JSON\n"
            "  AD_Killer_By_NFQUEUE --sources-save     从 stdin 校验并原子保存来源\n"
            "  AD_Killer_By_NFQUEUE --source-test URL  后台测试单源，不发布规则\n"
            "  AD_Killer_By_NFQUEUE --source-preview URL [字符数]  只读预览（默认500，可选100–5000）\n"
            "  AD_Killer_By_NFQUEUE --merge-snapshot INPUT OUTPUT  隔离合并固定快照，不访问运行数据\n"
            "  AD_Killer_By_NFQUEUE --mode 1|2         规则强度(1=仅Hosts 2=Hosts+AdGuard)\n"
            "  AD_Killer_By_NFQUEUE --scope 0|1        作用域(0=全局 1=仅名单内应用)\n"
            "  AD_Killer_By_NFQUEUE --dev on|off       DNS 查询日志\n"
            "  AD_Killer_By_NFQUEUE --quic on|off      阻止 QUIC\n"
            "  AD_Killer_By_NFQUEUE --autostart on|off 开机自启(默认关闭，不改变当前运行状态)\n"
            "  AD_Killer_By_NFQUEUE --apps-apply       应用名单生效\n"
            "  AD_Killer_By_NFQUEUE --status           状态 JSON\n"
            "  AD_Killer_By_NFQUEUE --apply-status [REVISION]  查询配置生效回执（只读）\n"
            "  AD_Killer_By_NFQUEUE --version-status   版本 JSON\n",
            MODULE_VERSION);
}

// ---------------------------------------------------------------------------
// 入口:子命令分派
int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    std::string arg = argv[1];

    if (arg == "--config-read" || arg == "--config-save") {
        const bool saving = arg == "--config-save";
        if (argc != (saving ? 4 : 3)) { usage(); return 1; }
        return edit_config::command(DATA_DIR, saving, argv[2], saving ? argv[3] : "", [](const char *path, const std::string &text) {
            return mark_policy_dirty() ? write_file_durable_atomic(path, text) : AtomicWriteResult{};
        });
    }

    if (arg == "--merge-snapshot") {
        if (argc != 4) { usage(); return 1; }
        return merge_rule_snapshot(argv[2], argv[3]);
    }

    if (arg == "--install")        return cmd_install();
    if (arg == "--module-update-check") {
        if (argc != 3 || (strcmp(argv[2],"stable") != 0 && strcmp(argv[2],"preview") != 0)) { usage(); return 1; }
        return module_update::release_info(strcmp(argv[2],"preview") == 0);
    }
    if (arg == "--module-update-status") return module_update::status();
    if (arg == "--module-release-notes") {
        if (argc != 2) { usage(); return 1; }
        return module_update::release_notes();
    }
    if (arg == "--module-update-download" || arg == "--module-update-install") {
        if (geteuid() != 0 || (argc != 3 && argc != 4) ||
            (argc == 4 && strcmp(argv[3],"stable") != 0 && strcmp(argv[3],"preview") != 0) ||
            !module_update::upgrade_allowed(argv[2], MODULE_VERSION, MODULE_RELEASE_CHANNEL, argc == 4 ? argv[3] : "stable")) {
            fprintf(stderr,"模块更新需要 root 权限及更新版本，或同版本的预览版升级正式版\n"); return 1;
        }
        return cmd_update_start("",argv[2],arg == "--module-update-install",argc == 4 && strcmp(argv[3],"preview") == 0);
    }
    if (arg == "--start" || arg == "--start-boot") {
        if (argc != 2) { usage(); return 1; }
        return cmd_start(arg == "--start-boot");
    }
    if (arg == "--stop")           return cmd_stop();
    if (arg == "--prepare-reboot") {
        if (argc != 2) { usage(); return 1; }
        return cmd_prepare_reboot();
    }
    if (arg == "--daemon") {
        if (argc == 3 && strcmp(argv[2], "--foreground") == 0) return cmd_start(false, true);
        if (argc != 2) { usage(); return 1; }
        return cmd_daemon();
    }
    if (arg == "--reload") {
        if (argc == 2) return cmd_reload();
        if (argc == 4 && strcmp(argv[2], "custom") == 0 && apply_state::valid_revision(argv[3])) return cmd_reload(argv[2], argv[3]);
        usage(); return 1;
    }
    if (arg == "--apply-status") {
        if (argc == 2) return cmd_apply_status();
        if (argc == 3 && apply_state::valid_revision(argv[2])) return cmd_apply_status(argv[2]);
        usage(); return 1;
    }
    if (arg == "--update")         return cmd_update();
    if (arg == "--update-start")   return cmd_update_start();
    if (arg == "--source-preview") {
        int characters = SOURCE_PREVIEW_DEFAULT;
        if ((argc != 3 && argc != 4) || (argc == 4 && (!parse_integer_arg(argv[3], characters) ||
            characters < static_cast<int>(SOURCE_PREVIEW_MIN) || characters > static_cast<int>(SOURCE_PREVIEW_MAX)))) {
            fprintf(stderr, "用法: AD_Killer_By_NFQUEUE --source-preview URL [100–5000]\n"); return 1;
        }
        return cmd_source_preview(argv[2], static_cast<size_t>(characters));
    }
    if (arg == "--sources-list")   return cmd_sources(false);
    if (arg == "--sources-save")   return cmd_sources(true);
    if (arg == "--source-test") {
        if (argc != 3 || !argv[2][0]) { usage(); return 1; }
        return cmd_update_start(argv[2]);
    }
    if (arg == "--update-status") {
        if (argc < 3) {
            fprintf(stderr, "参数错误\n用法: AD_Killer_By_NFQUEUE --update-status JOB_ID\n");
            return 1;
        }
        return cmd_update_status(argv[2]);
    }
    if (arg == "--status")         return cmd_status();
    if (arg == "--status-fast")    return cmd_status_fast();
    if (arg == "--version-status") return cmd_version_status();
    if (arg == "--mode") {
        if (argc < 3) { usage(); return 1; }
        int mode = 0;
        if (!parse_integer_arg(argv[2], mode)) {
            fprintf(stderr, "参数错误\n用法: AD_Killer_By_NFQUEUE --mode 1|2\n");
            return 1;
        }
        return cmd_mode(mode);
    }
    if (arg == "--scope") {
        if (argc < 3) { usage(); return 1; }
        int scope = 0;
        if (!parse_integer_arg(argv[2], scope)) {
            fprintf(stderr, "参数错误\n用法: AD_Killer_By_NFQUEUE --scope 0|1\n");
            return 1;
        }
        return cmd_scope(scope);
    }
    if (arg == "--dev" || arg == "--quic" || arg == "--autostart") {
        if (argc < 3) { usage(); return 1; }
        const bool valid_on = strcmp(argv[2], "on") == 0 ||
                              strcmp(argv[2], "1") == 0;
        const bool valid_off = strcmp(argv[2], "off") == 0 ||
                               strcmp(argv[2], "0") == 0;
        if (!valid_on && !valid_off) {
            fprintf(stderr, "参数错误\n用法: AD_Killer_By_NFQUEUE %s on|off\n", argv[1]);
            return 1;
        }
        bool on = valid_on;
        return cmd_toggle(arg == "--autostart" ? "autostart" :
                          arg == "--dev" ? "dev_mode" : "block_quic", on);
    }
    if (arg == "--apps-apply") {
        if (argc == 2) return cmd_apps_apply();
        if (argc == 4 && (strcmp(argv[2], "allow") == 0 || strcmp(argv[2], "intercept") == 0) && apply_state::valid_revision(argv[3])) return cmd_apps_apply(argv[2], argv[3]);
        usage(); return 1;
    }
    usage();
    return 1;
}
