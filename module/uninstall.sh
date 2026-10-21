#!/system/bin/sh
# ============================================================================
# AD_Killer_By_NFQUEUE 卸载钩子:确保拦截规则被彻底拔除,防止卸载后断网/失联
#
# 安全要点:
#   1. 先删 OUTPUT 上所有链的跳转(-j 链),再 -F/-X 删链(顺序不可反)
#   2. 覆盖全部链:稳定入口 + A/B 代际链 + 旧版双缓冲链
#   3. 只删除可由本模块链名确认归属的规则
#   4. 全程幂等(重复执行无副作用)
# ============================================================================

MODDIR="${0%/*}"

delete_all() {
    count=0
    while [ "$count" -lt 32 ]; do
        "$@" 2>/dev/null || break
        count=$((count + 1))
    done
}

# 1. 优先让二进制自己干净卸载(含 .route.lock 互斥)
if [ -x "$MODDIR/AD_Killer_By_NFQUEUE" ]; then
    "$MODDIR/AD_Killer_By_NFQUEUE" --stop >/dev/null 2>&1
fi

# 2. 手动兜底清理(二进制已删/--stop 失败时仍能拔净)
CH="ADK_NFQ_ENTRY ADK_NFQ_APPS_A ADK_NFQ_APPS_B ADK_NFQ_OUT_A ADK_NFQ_OUT_B ADK_NFQ_APPS_NEXT ADK_NFQ_APPS_OLD ADK_NFQ_APPS ADK_NFQ_OUT"
for tb in iptables ip6tables; do
    # 2.1 先删 OUTPUT 跳转(否则 -X 删链会失败,规则残留)
    for ch in $CH; do
        delete_all "$tb" -w 5 -t mangle -D OUTPUT -j "$ch"
    done
    # 2.2 可确认归属的旧 uid jump。匿名 NFQUEUE/DROP 可能属于别的模块，
    #     不按端口或 queue 编号擅自删除；旧匿名布局须由原版本显式迁移。
    delete_all "$tb" -w 5 -t mangle -D OUTPUT -m owner --uid-owner 0 -j ADK_NFQ_OUT
    # 2.3 清链 + 删链
    for ch in $CH; do
        "$tb" -w 5 -t mangle -F "$ch" 2>/dev/null
        "$tb" -w 5 -t mangle -X "$ch" 2>/dev/null
    done
done

# 3. 清理守护进程 pid 文件(防残留误判)
rm -f /data/adb/AD_Killer_By_NFQUEUE/daemon.pid 2>/dev/null

# 数据目录(/data/adb/AD_Killer_By_NFQUEUE)保留,便于重新安装后复用规则。
