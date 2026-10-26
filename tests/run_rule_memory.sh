#!/bin/sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$(uname -s)" != Linux ] || [ "$(id -u)" -ne 0 ]; then
    echo "rule memory profiling requires root in a disposable Linux environment" >&2
    exit 1
fi
: "${MEMORY_SNAPSHOT:?set MEMORY_SNAPSHOT to a frozen input directory}"
: "${MEMORY_OUTPUT:?set MEMORY_OUTPUT to a new persistent report directory}"
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/adk-rule-memory.XXXXXX")
trap 'rm -rf -- "$BUILD_DIR"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    '-DADK_NFQ_TEST_CURL_LIBRARY="/missing/adk-libcurl"' \
    '-DADK_NFQ_TEST_CURL_PROGRAM="/missing/adk-curl"' \
    '-DADK_NFQ_TEST_WGET_PROGRAM="/missing/adk-wget"' \
    -I"$ROOT/src" "$ROOT"/src/*.cpp -ldl -lz -o "$BUILD_DIR/AD_Killer_By_NFQUEUE"
set --
for source in "$ROOT"/src/*.cpp; do
    [ "$source" = "$ROOT/src/main.cpp" ] || set -- "$@" "$source"
done
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    -I"$ROOT/src" "$ROOT/tests/integration/prime_memory_cache.cpp" "$@" -ldl -lz -o "$BUILD_DIR/prime"
python3 "$ROOT/tests/integration/profile_rule_memory.py" \
    --binary "$BUILD_DIR/AD_Killer_By_NFQUEUE" --prime "$BUILD_DIR/prime" \
    --snapshot "$MEMORY_SNAPSHOT" --output "$MEMORY_OUTPUT"
