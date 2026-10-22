#!/system/bin/sh
# ============================================================================
# AD_Killer_By_NFQUEUE 开机自启脚本(KernelSU late_start / Magisk service 阶段执行)
#
# 安全要点:
#   - late_start 阶段脚本为后台执行,不阻塞 boot(区别于 post-fs-data 的阻塞)
#   - 独立 autostart 开关，缺失或无效时默认关闭
#   - 等待 boot_completed；超时退出，不在异常启动阶段接管网络
#   - 尊重 WebUI 持久暂停；正常启动由 daemon 自己清理崩溃残留规则
# ============================================================================

# 模块自身目录(KSU 规范:必须用 ${0%/*} 获取,禁止硬编码)
MODDIR=${0%/*}

# KernelSU soft reboot keeps independent root daemons alive while netd rebuilds
# OUTPUT. Reconcile an old instance before applying the normal boot policy,
# including upgrades from packages that had no soft-reboot hook. No preferences
# are changed; no daemon means this command performs no writes or route cleanup.
"$MODDIR/AD_Killer_By_NFQUEUE" --prepare-reboot >/dev/null 2>&1 || exit 1

STATE_CONF=/data/adb/AD_Killer_By_NFQUEUE/state.conf
should_autostart() {
    # Missing, unreadable, malformed or duplicate values all fail closed.
    boot_autostart=$(sed -n 's/^autostart=//p' "$STATE_CONF" 2>/dev/null) || return 1
    case "$boot_autostart" in
        true|1) ;;
        *) return 1 ;;
    esac
    boot_enabled=$(sed -n 's/^enabled=//p' "$STATE_CONF" 2>/dev/null) || return 1
    case "$boot_enabled" in
        true|1) ;;
        *) return 1 ;;
    esac
    boot_manual_stop=$(sed -n 's/^manual_stop=//p' "$STATE_CONF" 2>/dev/null) || return 1
    case "$boot_manual_stop" in
        ''|false|0) return 0 ;; # Missing legacy key still follows enabled.
        *) return 1 ;;
    esac
}

# 1. 默认不自启，手动暂停也跨重启保留。
should_autostart || exit 0

# 2. 仅在 Android 框架启动完成后接管；超时则跳过本次自启。
i=0
until [ "$(getprop sys.boot_completed)" = "1" ]; do
    sleep 1
    i=$((i + 1))
    [ "$i" -ge 180 ] && exit 0
done

# 等待期间可能从 WebUI 关闭自启或手动暂停，启动前再次检查。
should_autostart || exit 0

# 3. 确保二进制核心拥有可执行权限
chmod 755 "$MODDIR/AD_Killer_By_NFQUEUE"

# 4. 由核心自己双 fork + setsid 拉起独立守护进程；启动路径会先清理残留链。
#    不修改 autostart 配置；手动启停与是否开机自启是两个独立选择。
"$MODDIR/AD_Killer_By_NFQUEUE" --start >/dev/null 2>&1
