#!/system/bin/sh
# Upgrades keep data. Uninstall deletes data only after native verification.
MODDIR="${0%/*}"
BIN="$MODDIR/AD_Killer_By_NFQUEUE"
if [ -x "$BIN" ]; then
    "$BIN" --uninstall
    result=$?
    if [ "$result" -eq 0 ]; then exit 0; fi
    echo "AD Killer 卸载未完成，数据可能保留。请检查上方诊断；不会绕过安装屏障或删除活动锁。" >&2
    # A busy/unknown result must never fall through to manual mutations.
    if [ "$result" -ne 126 ] && [ "$result" -ne 127 ]; then exit "$result"; fi
fi
echo "AD Killer 二进制缺失或无法执行，不能核实后台写入者。保留数据并返回失败，请重新安装匹配版本后重试卸载。" >&2
# Read-only bounded fallback. Never guess a PID or flush another module's rules.
if command -v timeout >/dev/null 2>&1; then
    for tb in iptables ip6tables; do
        echo "[$tb] 只读路由诊断（完整残留检查请参阅 ADB 手册）" >&2
        timeout 3 "$tb" -w 1 -t mangle -S 2>&1 || echo "[$tb] 查询失败，状态未知" >&2
    done
else
    echo "缺少 timeout，跳过路由查询；状态未知。" >&2
fi
exit 1
