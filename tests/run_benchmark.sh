#!/bin/sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ad_killer_by_nfqueue_benchmark.XXXXXX")
trap 'rm -rf -- "$BUILD_DIR"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    -I"$ROOT/src" "$ROOT/tests/benchmark_dataplane.cpp" \
    "$ROOT/src/rules.cpp" "$ROOT/src/rule_stream.cpp" "$ROOT/src/rule_bundle.cpp" \
    "$ROOT/src/packet.cpp" "$ROOT/src/logging.cpp" "$ROOT/src/bounded_log.cpp" \
    -lz -o "$BUILD_DIR/benchmark"
"$BUILD_DIR/benchmark" "${RULE_COUNT:-420000}" "${BENCH_ITERATIONS:-100000}"
