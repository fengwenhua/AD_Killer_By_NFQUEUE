#!/bin/sh
# Local complete suite; CI invokes the same groups separately exactly once.
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
sh "$ROOT/tests/run_native_tests.sh"
sh "$ROOT/tests/run_frontend_tests.sh"
sh "$ROOT/tests/run_contract_tests.sh"
