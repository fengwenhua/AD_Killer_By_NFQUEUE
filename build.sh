#!/bin/bash
# ============================================================================
# AD_Killer_By_NFQUEUE 构建脚本:Vue WebUI + C++ 引擎(arm64-v8a)+ 组装模块 + 打包 zip
#
# 目录约定:
#   src/    引擎源码
#   frontend/ Vue 3 前端源码（Node.js 22.12+ / 24 LTS）
#   module/ 模块本体(module.prop 等 KSU 规范文件 + webroot)
#   dist/   构建产物(裸二进制 + 可刷入 zip)
#
# 用法:
#   NDK=/path/to/android-ndk-rXX ./build.sh
#   或先:  sdkmanager "ndk;26.3.11579264"
# ============================================================================
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
# module.prop 是版本号的唯一事实来源，同时注入二进制和产物文件名。
VERSION_RAW="$(sed -n 's/^version=//p' "$HERE/module/module.prop")"
VERSION="${VERSION_RAW#v}"
VERSION_CODE="$(sed -n 's/^versionCode=//p' "$HERE/module/module.prop")"
RELEASE_CHANNEL="${RELEASE_CHANNEL:-stable}"
if [[ ! "$VERSION" =~ ^[0-9]+\.[0-9]+(\.[0-9]+)?$ ]] ||
   [[ ! "$VERSION_CODE" =~ ^[0-9]+$ ]] || [ "$VERSION_CODE" -lt 2 ]; then
  echo "❌ module.prop 的 version/versionCode 非法"
  exit 1
fi
case "$RELEASE_CHANNEL" in
  stable) ;;
  preview) VERSION_CODE=$((VERSION_CODE - 1)) ;;
  *) echo "invalid RELEASE_CHANNEL (stable|preview)"; exit 1 ;;
esac
export RELEASE_CHANNEL

NDK="${NDK:-$HOME/Library/Android/sdk/ndk/26.3.11579264}"
if [ ! -d "$NDK" ]; then
  for d in "$HOME/Android/Sdk/ndk"/* "$HOME/Library/Android/sdk/ndk"/*; do
    [ -d "$d" ] && NDK="$d" && break
  done
fi
if [ ! -d "$NDK" ]; then
  echo "❌ 未找到 Android NDK,请安装后重试:"
  echo "   sdkmanager \"ndk;26.3.11579264\""
  echo "   或 NDK=/path/to/ndk ./build.sh"
  exit 1
fi
echo "NDK: $NDK"

case "$(uname -s)" in
  Darwin) HOST_PREFIX=darwin ;;
  Linux)  HOST_PREFIX=linux ;;
  *) echo "❌ 不支持的 NDK 主机系统: $(uname -s)"; exit 1 ;;
esac
case "$(uname -m)" in
  x86_64) HOST_ARCH=x86_64 ;;
  arm64|aarch64) HOST_ARCH=arm64 ;;
  *) echo "❌ 不支持的 NDK 主机架构: $(uname -m)"; exit 1 ;;
esac
CXX=
for tag in "$HOST_PREFIX-$HOST_ARCH" "$HOST_PREFIX-x86_64" \
           "$HOST_PREFIX-arm64" "$HOST_PREFIX-aarch64"; do
  candidate="$NDK/toolchains/llvm/prebuilt/$tag/bin/aarch64-linux-android31-clang++"
  if [ -x "$candidate" ]; then CXX="$candidate"; break; fi
done
if [ -z "$CXX" ]; then
  echo "❌ NDK 中未找到 aarch64-linux-android31-clang++"
  exit 1
fi

DIST="$HERE/dist"
[ "$RELEASE_CHANNEL" = preview ] && DIST="$DIST/preview"
BIN="$DIST/AD_Killer_By_NFQUEUE"
mkdir -p "$DIST"

echo "[1/4] 构建 Vue WebUI..."
command -v npm >/dev/null 2>&1 || { echo "❌ 请先安装 Node.js 22.12+ 或 24 LTS (含 npm)"; exit 1; }
# 锁文件安装，缺依赖或构建失败立即停止，绝不打包旧 WebUI。
npm --prefix "$HERE/frontend" ci --include=dev --no-audit --no-fund
npm --prefix "$HERE/frontend" run build
python3 "$HERE/tests/test_webui_contract.py" "$HERE/module/webroot/index.html"

echo "[2/4] 编译 AD_Killer_By_NFQUEUE (arm64-v8a)..."
$CXX -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -fvisibility=hidden \
  -DMODULE_VERSION="\"$VERSION\"" -DMODULE_VERSION_CODE="$VERSION_CODE" \
  -DMODULE_RELEASE_CHANNEL="\"$RELEASE_CHANNEL\"" \
  -Wl,--gc-sections -ffunction-sections -fdata-sections \
  -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 \
  -static-libstdc++ \
  -o "$BIN" \
  "$HERE/src/main.cpp" "$HERE/src/engine.cpp" "$HERE/src/rules.cpp" \
  "$HERE/src/module_update.cpp" \
  "$HERE/src/edit_config.cpp" \
  "$HERE/src/lifecycle.cpp" \
  "$HERE/src/apply_state.cpp" \
  "$HERE/src/process_runner.cpp" \
  "$HERE/src/rule_stream.cpp" \
  "$HERE/src/snapshot_merge.cpp" \
  "$HERE/src/metrics.cpp" \
  "$HERE/src/cgroup_isolation.cpp" \
  "$HERE/src/rule_bundle.cpp" "$HERE/src/source_validator.cpp" \
  "$HERE/src/hagezi_source.cpp" \
  "$HERE/src/source_preview.cpp" \
  "$HERE/src/update_job.cpp" \
  "$HERE/src/bounded_log.cpp" \
  "$HERE/src/packet.cpp" "$HERE/src/tcp_reassembly.cpp" \
  "$HERE/src/fast_response.cpp" \
  "$HERE/src/dns_tcp_reassembly.cpp" \
  "$HERE/src/nfnetlink.cpp" "$HERE/src/nfnetlink_parser.cpp" \
  "$HERE/src/iptables.cpp" \
  "$HERE/src/net.cpp" "$HERE/src/net_policy.cpp" "$HERE/src/logging.cpp" \
  -llog -lz -ldl

python3 "$HERE/tests/validate_android_elf.py" "$BIN"

echo "[3/4] 组装模块(module/ + 编译产物)..."
PKG="$DIST/AD_Killer_By_NFQUEUE-module"
rm -rf "$PKG"
mkdir -p "$PKG/webroot"
cp "$BIN" "$PKG/AD_Killer_By_NFQUEUE"
cp "$HERE/module/module.prop" "$PKG/"
python3 "$HERE/scripts/package_channel.py" "$PKG/module.prop" "$RELEASE_CHANNEL" "$VERSION_CODE"
cp "$HERE/module/service.sh" "$HERE/module/emulated-soft-reboot.sh" "$HERE/module/customize.sh" "$HERE/module/uninstall.sh" "$PKG/"
cp "$HERE/module/webroot/index.html" "$PKG/webroot/"
cp "$HERE/LICENSE" "$PKG/"
chmod 755 "$PKG/AD_Killer_By_NFQUEUE" "$PKG/service.sh" "$PKG/emulated-soft-reboot.sh" "$PKG/customize.sh" "$PKG/uninstall.sh"

echo "[4/4] 打包 zip..."
ZIP="$DIST/AD_Killer_By_NFQUEUE-v${VERSION}.zip"
SOURCE_DATE_EPOCH="${SOURCE_DATE_EPOCH:-315532800}" \
  python3 "$HERE/scripts/package_module.py" "$PKG" "$ZIP"
python3 "$HERE/scripts/release_metadata.py" "$ZIP" "$HERE/CHANGELOG.md"
python3 "$HERE/tests/validate_manager_update.py" "$ZIP"

echo "✅ 完成: $ZIP"
echo "   校验: $ZIP.sha256"
echo "   adb push + 内核管理器安装,或直接在 KernelSU 中选择 zip 刷入"
