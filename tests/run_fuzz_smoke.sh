#!/bin/sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
TMP_BASE=${TMPDIR:-/tmp}
BUILD_DIR=$(mktemp -d "${TMP_BASE%/}/ad_killer_by_nfqueue_fuzz.XXXXXX")

cleanup() {
    status=$?
    trap - EXIT HUP INT TERM
    if [ "${KEEP_TEST_BUILD:-0}" = "1" ]; then
        echo "fuzz build kept at: $BUILD_DIR"
    else
        rm -rf -- "$BUILD_DIR"
    fi
    exit "$status"
}
trap cleanup EXIT HUP INT TERM

CXX=${FUZZ_CXX:-clang++}
command -v "$CXX" >/dev/null 2>&1 || {
    echo "missing libFuzzer-capable compiler: $CXX" >&2
    exit 1
}

BASE="-std=c++17 -O1 -g -Wall -Wextra -Wpedantic -fno-omit-frame-pointer"
FUZZ_FLAGS="$BASE -fsanitize=fuzzer,address,undefined"
SAN_FLAGS="$BASE -fsanitize=address,undefined"

mode=libfuzzer
COMMON_FLAGS=$FUZZ_FLAGS
DRIVER=
if ! $CXX $FUZZ_FLAGS -I"$ROOT/src" \
    "$ROOT/tests/fuzz/fuzz_packet.cpp" "$ROOT/src/packet.cpp" \
    -o "$BUILD_DIR/fuzz_packet" 2>"$BUILD_DIR/libfuzzer-probe.log"; then
    mode=standalone
    COMMON_FLAGS=$SAN_FLAGS
    DRIVER="$ROOT/tests/fuzz/fuzz_standalone_main.cpp"
    echo "libFuzzer runtime unavailable; using deterministic ASan/UBSan driver"
    $CXX $SAN_FLAGS -I"$ROOT/src" \
        "$ROOT/tests/fuzz/fuzz_packet.cpp" \
        "$ROOT/tests/fuzz/fuzz_standalone_main.cpp" "$ROOT/src/packet.cpp" \
        -o "$BUILD_DIR/fuzz_packet"
fi

$CXX $COMMON_FLAGS -I"$ROOT/src" \
    "$ROOT/tests/fuzz/fuzz_rules.cpp" \
    $DRIVER \
    "$ROOT/src/rules.cpp" "$ROOT/src/rule_stream.cpp" \
    "$ROOT/src/rule_bundle.cpp" "$ROOT/src/logging.cpp" \
    "$ROOT/src/bounded_log.cpp" -lz \
    -o "$BUILD_DIR/fuzz_rules"

$CXX $COMMON_FLAGS -I"$ROOT/src" \
    "$ROOT/tests/fuzz/fuzz_nfnetlink.cpp" "$ROOT/src/nfnetlink_parser.cpp" \
    $DRIVER \
    -o "$BUILD_DIR/fuzz_nfnetlink"

RUNS=${FUZZ_RUNS:-500}
for target in fuzz_packet fuzz_rules fuzz_nfnetlink; do
    if [ "$mode" = libfuzzer ]; then
        "$BUILD_DIR/$target" -runs="$RUNS" -max_len=4096 -print_final_stats=1
    else
        "$BUILD_DIR/$target" "$RUNS"
    fi
done
