#!/bin/sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
if [ "$(uname -s)" != Linux ] || [ "$(id -u)" -ne 0 ]; then
    echo "cgroup integration requires root in a disposable Linux environment" >&2
    exit 1
fi
BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/ad_killer_by_nfqueue_cgroup.XXXXXX")
trap 'rm -rf -- "$BUILD_DIR"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    -I"$ROOT/src" "$ROOT"/src/*.cpp -ldl -lz -o "$BUILD_DIR/AD_Killer_By_NFQUEUE"
PYTHONDONTWRITEBYTECODE=1 python3 "$ROOT/tests/integration/test_cgroup_freezer.py" "$BUILD_DIR/AD_Killer_By_NFQUEUE"
