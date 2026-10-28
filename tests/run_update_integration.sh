#!/bin/sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$(uname -s)" != Linux ] || [ "$(id -u)" -ne 0 ]; then
    echo "update integration requires root in a disposable Linux environment" >&2
    exit 1
fi
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ad_killer_by_nfqueue_update.XXXXXX")
trap 'rm -rf -- "$BUILD_DIR"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    '-DADK_NFQ_TEST_CURL_LIBRARY="libcurl.so.4"' \
    '-DADK_NFQ_TEST_CURL_PROGRAM="/usr/bin/curl"' \
    -I"$ROOT/src" "$ROOT"/src/*.cpp -ldl -lz -o "$BUILD_DIR/AD_Killer_By_NFQUEUE"
python3 "$ROOT/tests/integration/test_update.py" "$BUILD_DIR/AD_Killer_By_NFQUEUE"
python3 "$ROOT/tests/integration/test_sync_concurrency.py" "$BUILD_DIR/AD_Killer_By_NFQUEUE"
python3 "$ROOT/tests/integration/test_job_identity.py" "$BUILD_DIR/AD_Killer_By_NFQUEUE"
python3 "$ROOT/tests/integration/test_operation_gate.py" "$BUILD_DIR/AD_Killer_By_NFQUEUE"
PYTHONDONTWRITEBYTECODE=1 python3 "$ROOT/tests/integration/test_sync_budget.py"
PYTHONDONTWRITEBYTECODE=1 python3 "$ROOT/tests/integration/test_source_cache_live.py"
PYTHONDONTWRITEBYTECODE=1 python3 "$ROOT/tests/integration/test_hagezi_live.py"
PYTHONDONTWRITEBYTECODE=1 python3 "$ROOT/tests/integration/test_module_update.py"
