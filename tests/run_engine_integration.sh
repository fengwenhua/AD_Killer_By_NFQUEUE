#!/bin/sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$(uname -s)" != Linux ] || [ "$(id -u)" -ne 0 ]; then
    echo "engine integration requires root in a disposable Linux environment" >&2
    exit 1
fi
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ad_killer_by_nfqueue_engine.XXXXXX")
trap 'rm -rf -- "$BUILD_DIR"' EXIT HUP INT TERM
case "${IPTABLES_BACKEND:-auto}" in
    auto) ;;
    legacy|nft)
        mkdir "$BUILD_DIR/bin"
        for tool in iptables ip6tables iptables-save ip6tables-save; do
            # save binaries are named iptables-legacy-save / iptables-nft-save.
            case "$tool" in
                *-save) selected="${tool%-save}-${IPTABLES_BACKEND}-save" ;;
                *) selected="${tool}-${IPTABLES_BACKEND}" ;;
            esac
            path=$(command -v "$selected") || {
                echo "missing integration backend: $selected" >&2
                exit 1
            }
            ln -s "$path" "$BUILD_DIR/bin/$tool"
        done
        PATH="$BUILD_DIR/bin:$PATH"
        export PATH
        ;;
    *) echo "IPTABLES_BACKEND must be auto, legacy or nft" >&2; exit 1 ;;
esac
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    -I"$ROOT/src" "$ROOT"/src/*.cpp -ldl -lz -o "$BUILD_DIR/AD_Killer_By_NFQUEUE"
python3 "$ROOT/tests/integration/test_autostart.py" "$BUILD_DIR/AD_Killer_By_NFQUEUE"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    -I"$ROOT/src" "$ROOT/tests/integration/hold_nfqueue.cpp" \
    "$ROOT/src/nfnetlink.cpp" "$ROOT/src/nfnetlink_parser.cpp" \
    -o "$BUILD_DIR/hold_nfqueue"
python3 "$ROOT/tests/integration/test_startup_conflict.py" "$BUILD_DIR/AD_Killer_By_NFQUEUE" "$BUILD_DIR/hold_nfqueue"
python3 "$ROOT/tests/integration/test_lifecycle_races.py" "$BUILD_DIR/AD_Killer_By_NFQUEUE"
python3 "$ROOT/tests/integration/test_observability.py" "$BUILD_DIR/AD_Killer_By_NFQUEUE"
python3 "$ROOT/tests/integration/test_cleanup_failures.py" "$BUILD_DIR/AD_Killer_By_NFQUEUE"
python3 "$ROOT/tests/integration/test_reload_consistency.py" "$BUILD_DIR/AD_Killer_By_NFQUEUE"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    -I"$ROOT/src" "$ROOT/tests/integration/queue_apply_without_signal.cpp" \
    "$ROOT/src/apply_state.cpp" "$ROOT/src/logging.cpp" "$ROOT/src/bounded_log.cpp" \
    -lz -o "$BUILD_DIR/queue_apply_without_signal"
python3 "$ROOT/tests/integration/test_apply_protocol.py" "$BUILD_DIR/AD_Killer_By_NFQUEUE" "$BUILD_DIR/queue_apply_without_signal"
python3 "$ROOT/tests/integration/test_engine.py" "$BUILD_DIR/AD_Killer_By_NFQUEUE"
python3 "$ROOT/tests/integration/test_policy.py" "$BUILD_DIR/AD_Killer_By_NFQUEUE"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    -I"$ROOT/src" "$ROOT/tests/integration/conntrack_probe.cpp" \
    "$ROOT/src/nfnetlink.cpp" "$ROOT/src/nfnetlink_parser.cpp" \
    -o "$BUILD_DIR/conntrack_probe"
python3 "$ROOT/tests/integration/test_conntrack.py" "$BUILD_DIR/conntrack_probe"
# Compile with the real loopback downloader for uninstall cancellation coverage.
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    '-DADK_NFQ_TEST_CURL_LIBRARY="libcurl.so.4"' \
    '-DADK_NFQ_TEST_CURL_PROGRAM="/usr/bin/curl"' \
    -I"$ROOT/src" "$ROOT"/src/*.cpp -ldl -lz -o "$BUILD_DIR/uninstall-engine"
python3 "$ROOT/tests/integration/test_uninstall.py" "$BUILD_DIR/uninstall-engine"
# Exercise the production CTNETLINK fallback even on a host with GLUE_CT enabled.
mkdir "$BUILD_DIR/direct"
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    -DADK_NFQ_TEST_NO_CT_GLUE -I"$ROOT/src" "$ROOT"/src/*.cpp \
    -ldl -lz -o "$BUILD_DIR/direct/AD_Killer_By_NFQUEUE"
python3 "$ROOT/tests/integration/test_engine.py" "$BUILD_DIR/direct/AD_Killer_By_NFQUEUE"
python3 "$ROOT/tests/integration/test_policy.py" "$BUILD_DIR/direct/AD_Killer_By_NFQUEUE"
