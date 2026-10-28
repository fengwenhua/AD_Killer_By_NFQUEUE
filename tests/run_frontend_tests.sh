#!/bin/sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
npm --prefix "$ROOT/frontend" test
npm --prefix "$ROOT/frontend" run build
python3 "$ROOT/tests/test_webui_contract.py" "$ROOT/module/webroot/index.html"
