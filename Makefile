.PHONY: test test-native test-native-sanitize test-frontend test-contracts test-rules test-sanitize test-nfqueue-integration test-engine-integration test-cgroup-integration test-download-integration test-update-integration fuzz-smoke benchmark profile-rule-memory

test:
	sh tests/run_host_tests.sh

test-native:
	sh tests/run_native_tests.sh

test-native-sanitize:
	STRICT=1 SANITIZE=1 sh tests/run_native_tests.sh

test-frontend:
	sh tests/run_frontend_tests.sh

test-contracts:
	sh tests/run_contract_tests.sh

test-rules:
	CHECK_RULE_ARTIFACTS=1 sh tests/run_host_tests.sh

test-sanitize:
	STRICT=1 SANITIZE=1 sh tests/run_host_tests.sh

test-nfqueue-integration:
	sh tests/run_nfqueue_integration.sh

test-engine-integration:
	sh tests/run_engine_integration.sh

test-cgroup-integration:
	sh tests/run_cgroup_integration.sh

test-download-integration:
	python3 tests/integration/test_download.py
	python3 tests/integration/test_download_budget.py
	python3 tests/integration/test_http_validators.py
	python3 tests/integration/test_preview.py
	python3 tests/integration/test_document.py

test-update-integration:
	sh tests/run_update_integration.sh

fuzz-smoke:
	sh tests/run_fuzz_smoke.sh

benchmark:
	sh tests/run_benchmark.sh

profile-rule-memory:
	sh tests/run_rule_memory.sh
