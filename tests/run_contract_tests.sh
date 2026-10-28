#!/bin/sh
set -eu
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
python3 "$ROOT/tests/test_engine_lifecycle_contract.py"
python3 "$ROOT/tests/test_module_lifecycle_contract.py"
python3 "$ROOT/tests/test_boot_service.py"
python3 "$ROOT/tests/test_package.py"
python3 "$ROOT/tests/test_manager_update.py"
python3 "$ROOT/tests/test_listing_and_release.py"
python3 "$ROOT/tests/test_release_transport.py"
python3 "$ROOT/tests/test_release_contract.py"
python3 "$ROOT/tests/test_adb_inspect.py"
python3 "$ROOT/tests/test_rule_memory_profile.py"
python3 "$ROOT/tests/test_uninstall_script.py"
python3 "$ROOT/tests/test_data_paths.py"
python3 "$ROOT/tests/test_test_orchestration.py"
python3 "$ROOT/tests/test_fuzz_runner.py"
