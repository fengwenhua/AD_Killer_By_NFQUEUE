#!/system/bin/sh
# ============================================================================
# AD_Killer_By_NFQUEUE 模块安装脚本(KernelSU / Magisk / APatch 通用)
#
# KernelSU 规范要点:
#   - 本脚本会被模块安装器 source(非执行),$MODPATH 指向解压后的模块目录
#   - 使用 ui_print 输出(不用 echo),abort 终止(不用 exit)
#   - SKIPUNZIP 未设置:安装器已自动解压全部文件到 $MODPATH
# ============================================================================

ui_print " "
ui_print "======================================"
ui_print "       🛡️ AD_Killer_By_NFQUEUE 流量拦截引擎        "
ui_print "======================================"
ui_print "  引擎: C++ Native NFQUEUE(自建版)"
ui_print "  特性: 纯本地运行 / 规则源可自定义"
ui_print "======================================"
ui_print " "

# 安装器提供 ARCH/API。API 31 是当前 Android NDK 编译目标，包仅含 arm64。
[ "$ARCH" = "arm64" ] || abort "AD_Killer_By_NFQUEUE requires arm64-v8a"
case "$API" in
    ''|*[!0-9]*) abort "Cannot determine Android API level" ;;
esac
[ "$API" -ge 31 ] || abort "AD_Killer_By_NFQUEUE requires Android 12 (API 31) or newer"

# 初始化核心数据目录(实际初始化逻辑由二进制 --install 完成)
ui_print "- 正在初始化核心数据路径..."
# 数据目录创建与默认配置统一交给 --install。

# 赋予底层引擎执行权限($MODPATH:安装期模块目录,规范变量,勿硬编码)
ui_print "- 正在赋予底层引擎执行权限..."
chmod 755 "$MODPATH/AD_Killer_By_NFQUEUE"
chmod 755 "$MODPATH/service.sh"
chmod 755 "$MODPATH/uninstall.sh" 2>/dev/null

# 调用 C++ 核心的安装逻辑(目录初始化 + 默认配置)
"$MODPATH/AD_Killer_By_NFQUEUE" --install 2>&1
EXIT_CODE=$?

if [ "$EXIT_CODE" -eq 0 ]; then
    ui_print "✅ 模块安装完成!"
    ui_print "👉 首次安装默认不开机自启，请在 WebUI 手动启动测试。"
    ui_print "👉 开机自启可在 WebUI 单独开启；旧配置缺失此项也默认关闭。"
else
    ui_print "✗ 程序执行失败,退出码: $EXIT_CODE"
    ui_print "✗ 安装过程中出现错误"
    abort "install failed"
fi
