#!/bin/sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
TMP_BASE=${TMPDIR:-/tmp}
BUILD_DIR=$(mktemp -d "${TMP_BASE%/}/ad_killer_by_nfqueue_tests.XXXXXX")

cleanup() {
    status=$?
    trap - EXIT HUP INT TERM
    if [ "${KEEP_TEST_BUILD:-0}" = "1" ]; then
        echo "test build kept at: $BUILD_DIR"
    else
        rm -rf -- "$BUILD_DIR"
    fi
    exit "$status"
}
trap cleanup EXIT HUP INT TERM

command -v "${CXX:-c++}" >/dev/null 2>&1 || {
    echo "missing C++ compiler: ${CXX:-c++}" >&2
    exit 1
}
command -v python3 >/dev/null 2>&1 || {
    echo "missing python3" >&2
    exit 1
}

COMPILE_FLAGS="-std=c++17 -Wall -Wextra -Wpedantic"
if [ "${STRICT:-0}" = "1" ]; then
    COMPILE_FLAGS="$COMPILE_FLAGS -Werror"
fi
if [ "${SANITIZE:-0}" = "1" ]; then
    COMPILE_FLAGS="$COMPILE_FLAGS -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined"
fi

${CXX:-c++} $COMPILE_FLAGS \
    -I"$ROOT/src" \
    "$ROOT/tests/test_packet.cpp" \
    "$ROOT/src/packet.cpp" \
    -o "$BUILD_DIR/test_packet"

"$BUILD_DIR/test_packet"

${CXX:-c++} $COMPILE_FLAGS -I"$ROOT/src" \
    "$ROOT/tests/test_fast_response.cpp" "$ROOT/src/packet.cpp" -o "$BUILD_DIR/test_fast_response"
"$BUILD_DIR/test_fast_response"

${CXX:-c++} $COMPILE_FLAGS -I"$ROOT/src" \
    "$ROOT/tests/test_cgroup_isolation.cpp" "$ROOT/src/cgroup_isolation.cpp" \
    -o "$BUILD_DIR/test_cgroup_isolation"
"$BUILD_DIR/test_cgroup_isolation"

${CXX:-c++} $COMPILE_FLAGS -pthread \
    -I"$ROOT/src" \
    "$ROOT/tests/test_tcp_reassembly.cpp" \
    "$ROOT/src/tcp_reassembly.cpp" \
    "$ROOT/src/packet.cpp" \
    -o "$BUILD_DIR/test_tcp_reassembly"

"$BUILD_DIR/test_tcp_reassembly"

${CXX:-c++} $COMPILE_FLAGS -pthread \
    -I"$ROOT/src" \
    "$ROOT/tests/test_dns_tcp_reassembly.cpp" \
    "$ROOT/src/dns_tcp_reassembly.cpp" \
    "$ROOT/src/tcp_reassembly.cpp" \
    "$ROOT/src/packet.cpp" \
    -o "$BUILD_DIR/test_dns_tcp_reassembly"

"$BUILD_DIR/test_dns_tcp_reassembly"

${CXX:-c++} $COMPILE_FLAGS -pthread \
    -I"$ROOT/src" \
    "$ROOT/tests/test_metrics.cpp" \
    "$ROOT/src/metrics.cpp" \
    -o "$BUILD_DIR/test_metrics"

"$BUILD_DIR/test_metrics"

${CXX:-c++} $COMPILE_FLAGS -I"$ROOT/src" \
    "$ROOT/tests/test_metric_records.cpp" "$ROOT/src/metrics.cpp" -o "$BUILD_DIR/test_metric_records"
"$BUILD_DIR/test_metric_records"

${CXX:-c++} $COMPILE_FLAGS -pthread -I"$ROOT/src" \
    "$ROOT/tests/test_async_log.cpp" -o "$BUILD_DIR/test_async_log"
"$BUILD_DIR/test_async_log"

${CXX:-c++} $COMPILE_FLAGS -pthread \
    -I"$ROOT/src" \
    "$ROOT/tests/test_nfnetlink_parser.cpp" \
    "$ROOT/src/nfnetlink_parser.cpp" \
    -o "$BUILD_DIR/test_nfnetlink_parser"

"$BUILD_DIR/test_nfnetlink_parser"

${CXX:-c++} $COMPILE_FLAGS \
    -I"$ROOT/src" \
    "$ROOT/tests/test_net_policy.cpp" \
    "$ROOT/src/net_policy.cpp" \
    -o "$BUILD_DIR/test_net_policy"

"$BUILD_DIR/test_net_policy"

${CXX:-c++} $COMPILE_FLAGS -I"$ROOT/src" \
    "$ROOT/tests/test_work_budget.cpp" -o "$BUILD_DIR/test_work_budget"
"$BUILD_DIR/test_work_budget"

${CXX:-c++} $COMPILE_FLAGS -I"$ROOT/src" \
    "$ROOT/tests/test_safe_tree.cpp" -o "$BUILD_DIR/test_safe_tree"
"$BUILD_DIR/test_safe_tree"

${CXX:-c++} $COMPILE_FLAGS -I"$ROOT/src" \
    "$ROOT/tests/test_source_preview.cpp" "$ROOT/src/source_preview.cpp" \
    "$ROOT/src/logging.cpp" "$ROOT/src/bounded_log.cpp" -lz -o "$BUILD_DIR/test_source_preview"
"$BUILD_DIR/test_source_preview"

${CXX:-c++} $COMPILE_FLAGS -I"$ROOT/src" \
    "$ROOT/tests/test_source_config.cpp" "$ROOT/src/net_policy.cpp" \
    -o "$BUILD_DIR/test_source_config"
"$BUILD_DIR/test_source_config"

${CXX:-c++} $COMPILE_FLAGS -I"$ROOT/src" \
    "$ROOT/tests/test_queue_ownership.cpp" -o "$BUILD_DIR/test_queue_ownership"
"$BUILD_DIR/test_queue_ownership"

${CXX:-c++} $COMPILE_FLAGS \
    -I"$ROOT/src" \
    "$ROOT/tests/test_source_validator.cpp" \
    "$ROOT/src/source_validator.cpp" \
    "$ROOT/src/rules.cpp" \
    "$ROOT/src/rule_stream.cpp" \
    "$ROOT/src/rule_bundle.cpp" \
    "$ROOT/src/logging.cpp" \
    "$ROOT/src/bounded_log.cpp" \
    -lz \
    -o "$BUILD_DIR/test_source_validator"

"$BUILD_DIR/test_source_validator"

${CXX:-c++} $COMPILE_FLAGS -I"$ROOT/src" \
    "$ROOT/tests/test_hagezi_source.cpp" "$ROOT/src/hagezi_source.cpp" \
    "$ROOT/src/source_validator.cpp" "$ROOT/src/rules.cpp" "$ROOT/src/rule_stream.cpp" \
    "$ROOT/src/rule_bundle.cpp" "$ROOT/src/logging.cpp" "$ROOT/src/bounded_log.cpp" \
    -lz -o "$BUILD_DIR/test_hagezi_source"
"$BUILD_DIR/test_hagezi_source"
PYTHONDONTWRITEBYTECODE=1 python3 "$ROOT/tests/test_hagezi_parity.py" "$BUILD_DIR/test_hagezi_source"

${CXX:-c++} $COMPILE_FLAGS -pthread \
    -I"$ROOT/src" \
    "$ROOT/tests/test_rules.cpp" \
    "$ROOT/src/rules.cpp" \
    "$ROOT/src/rule_stream.cpp" \
    "$ROOT/src/rule_bundle.cpp" \
    "$ROOT/src/logging.cpp" \
    "$ROOT/src/bounded_log.cpp" \
    -lz \
    -o "$BUILD_DIR/test_rules"

"$BUILD_DIR/test_rules" "$ROOT/tests/rule_cases.tsv"

${CXX:-c++} $COMPILE_FLAGS \
    -I"$ROOT/src" \
    "$ROOT/tests/test_rule_stream.cpp" \
    "$ROOT/src/rule_stream.cpp" \
    -lz \
    -o "$BUILD_DIR/test_rule_stream"

"$BUILD_DIR/test_rule_stream"

python3 "$ROOT/tests/test_merge_rules.py"

${CXX:-c++} $COMPILE_FLAGS -pthread -I"$ROOT/src" \
    "$ROOT/tests/snapshot_merge_cli.cpp" "$ROOT/src/snapshot_merge.cpp" \
    "$ROOT/src/source_validator.cpp" "$ROOT/src/rules.cpp" \
    "$ROOT/src/rule_stream.cpp" "$ROOT/src/rule_bundle.cpp" \
    "$ROOT/src/net_policy.cpp" "$ROOT/src/logging.cpp" \
    "$ROOT/src/bounded_log.cpp" -lz -o "$BUILD_DIR/snapshot_merge"
python3 "$ROOT/tests/test_recipe_parity.py" "$BUILD_DIR/snapshot_merge"

${CXX:-c++} $COMPILE_FLAGS \
    -I"$ROOT/src" \
    "$ROOT/tests/test_iptables.cpp" \
    "$ROOT/src/iptables.cpp" \
    "$ROOT/src/process_runner.cpp" \
    "$ROOT/src/logging.cpp" \
    "$ROOT/src/bounded_log.cpp" \
    -lz \
    -o "$BUILD_DIR/test_iptables"

"$BUILD_DIR/test_iptables"

${CXX:-c++} $COMPILE_FLAGS \
    -I"$ROOT/src" \
    "$ROOT/tests/test_cli_utils.cpp" \
    -o "$BUILD_DIR/test_cli_utils"

"$BUILD_DIR/test_cli_utils"

${CXX:-c++} $COMPILE_FLAGS \
    -I"$ROOT/src" \
    "$ROOT/tests/test_exclusive_file_lock.cpp" \
    -o "$BUILD_DIR/test_exclusive_file_lock"

"$BUILD_DIR/test_exclusive_file_lock"

${CXX:-c++} $COMPILE_FLAGS -pthread \
    -I"$ROOT/src" \
    "$ROOT/tests/test_config_store.cpp" \
    "$ROOT/src/logging.cpp" \
    "$ROOT/src/bounded_log.cpp" \
    -lz \
    -o "$BUILD_DIR/test_config_store"

"$BUILD_DIR/test_config_store"

${CXX:-c++} $COMPILE_FLAGS -pthread -I"$ROOT/src" "$ROOT/tests/test_apply_state.cpp" \
    "$ROOT/src/apply_state.cpp" "$ROOT/src/logging.cpp" "$ROOT/src/bounded_log.cpp" -lz \
    -o "$BUILD_DIR/test_apply_state"
"$BUILD_DIR/test_apply_state"

${CXX:-c++} $COMPILE_FLAGS "$ROOT/tests/process_fixture.cpp" -o "$BUILD_DIR/process_fixture"
${CXX:-c++} $COMPILE_FLAGS -I"$ROOT/src" "$ROOT/tests/test_process_runner.cpp" \
    "$ROOT/src/process_runner.cpp" -o "$BUILD_DIR/test_process_runner"
"$BUILD_DIR/test_process_runner" "$BUILD_DIR/process_fixture"

${CXX:-c++} $COMPILE_FLAGS -I"$ROOT/src" "$ROOT/tests/test_operation_gate.cpp" \
    "$ROOT/src/process_runner.cpp" -o "$BUILD_DIR/test_operation_gate"
"$BUILD_DIR/test_operation_gate" "$BUILD_DIR/process_fixture"

${CXX:-c++} $COMPILE_FLAGS -pthread -I"$ROOT/src" \
    "$ROOT/tests/test_lifecycle.cpp" "$ROOT/src/lifecycle.cpp" \
    "$ROOT/src/logging.cpp" "$ROOT/src/bounded_log.cpp" -lz -o "$BUILD_DIR/test_lifecycle"
"$BUILD_DIR/test_lifecycle"
case "$(uname -s)" in
    Darwin) LIFECYCLE_LINK_FLAGS=-Wl,-dead_strip ;;
    *) LIFECYCLE_LINK_FLAGS=-Wl,--gc-sections ;;
esac
${CXX:-c++} $COMPILE_FLAGS -pthread -Wno-unused-function -ffunction-sections -fdata-sections \
    -DADK_AUDIT_DATA_DIR=\""$BUILD_DIR/lifecycle-data"\" \
    -include "$ROOT/tests/audit/rebase_paths.h" -I"$ROOT/src" \
    "$ROOT/tests/lifecycle_cli.cpp" "$ROOT/tests/lifecycle_logging.cpp" \
    "$ROOT/src/lifecycle.cpp" "$ROOT/src/apply_state.cpp" "$ROOT/src/process_runner.cpp" "$ROOT/src/bounded_log.cpp" \
    $LIFECYCLE_LINK_FLAGS -lz -o "$BUILD_DIR/lifecycle_cli"
python3 "$ROOT/tests/test_lifecycle_cli.py" "$BUILD_DIR/lifecycle_cli"

${CXX:-c++} $COMPILE_FLAGS -pthread -Wno-unused-function -ffunction-sections -fdata-sections \
    -DADK_AUDIT_DATA_DIR=\""$BUILD_DIR/idle-data"\" \
    -include "$ROOT/tests/audit/rebase_paths.h" -I"$ROOT/tests/audit/include" -I"$ROOT/src" \
    "$ROOT/tests/test_idle_maintenance.cpp" "$ROOT/src/tcp_reassembly.cpp" \
    "$ROOT/src/dns_tcp_reassembly.cpp" "$ROOT/src/packet.cpp" \
    $LIFECYCLE_LINK_FLAGS -o "$BUILD_DIR/test_idle_maintenance"
"$BUILD_DIR/test_idle_maintenance"

${CXX:-c++} $COMPILE_FLAGS -pthread -Wno-unused-function -ffunction-sections -fdata-sections \
    -I"$ROOT/tests/audit/include" -I"$ROOT/src" \
    "$ROOT/tests/test_packet_metrics.cpp" "$ROOT/src/metrics.cpp" "$ROOT/src/tcp_reassembly.cpp" \
    "$ROOT/src/dns_tcp_reassembly.cpp" "$ROOT/src/packet.cpp" \
    $LIFECYCLE_LINK_FLAGS -o "$BUILD_DIR/test_packet_metrics"
"$BUILD_DIR/test_packet_metrics"
python3 "$ROOT/tests/test_uninstall_script.py"

${CXX:-c++} $COMPILE_FLAGS -pthread -DADK_AUDIT_DATA_DIR=\""$BUILD_DIR/cleanup-data"\" \
    -include "$ROOT/tests/audit/rebase_paths.h" -I"$ROOT/src" \
    "$ROOT/tests/cleanup_cli.cpp" "$ROOT/src/iptables.cpp" "$ROOT/src/process_runner.cpp" \
    "$ROOT/src/logging.cpp" "$ROOT/src/bounded_log.cpp" -lz -o "$BUILD_DIR/cleanup_cli"
python3 "$ROOT/tests/test_iptables_cleanup.py" "$BUILD_DIR/cleanup_cli"

mkdir "$BUILD_DIR/engine-failure-data"
${CXX:-c++} $COMPILE_FLAGS -pthread -I"$ROOT/src" "$ROOT/tests/test_policy_barrier.cpp" \
    -o "$BUILD_DIR/test_policy_barrier"
"$BUILD_DIR/test_policy_barrier"
${CXX:-c++} $COMPILE_FLAGS -pthread -Wno-unused-function -ffunction-sections -fdata-sections \
    -DADK_AUDIT_DATA_DIR=\""$BUILD_DIR/engine-failure-data"\" -include "$ROOT/tests/audit/rebase_paths.h" \
    -I"$ROOT/tests/audit/include" -I"$ROOT/src" "$ROOT/tests/test_engine_failure.cpp" "$ROOT/src/edit_config.cpp" \
    "$ROOT/src/rules.cpp" "$ROOT/src/rule_stream.cpp" "$ROOT/src/rule_bundle.cpp" "$ROOT/src/apply_state.cpp" \
    "$ROOT/src/logging.cpp" "$ROOT/src/bounded_log.cpp" "$ROOT/src/tcp_reassembly.cpp" \
    "$ROOT/src/dns_tcp_reassembly.cpp" "$ROOT/src/packet.cpp" \
    $LIFECYCLE_LINK_FLAGS -lz -o "$BUILD_DIR/test_engine_failure"
"$BUILD_DIR/test_engine_failure"

for probe in test_edit_config edit_config_cli; do
    ${CXX:-c++} $COMPILE_FLAGS -pthread -I"$ROOT/src" \
        "$ROOT/tests/$probe.cpp" "$ROOT/src/edit_config.cpp" \
        "$ROOT/src/rules.cpp" "$ROOT/src/rule_stream.cpp" "$ROOT/src/rule_bundle.cpp" \
        "$ROOT/src/logging.cpp" "$ROOT/src/bounded_log.cpp" -lz -o "$BUILD_DIR/$probe"
done
"$BUILD_DIR/test_edit_config"
python3 "$ROOT/tests/test_edit_config.py" "$BUILD_DIR/edit_config_cli"

python3 "$ROOT/tests/test_data_paths.py"

${CXX:-c++} $COMPILE_FLAGS -pthread -I"$ROOT/src" \
    "$ROOT/tests/test_module_update.cpp" "$ROOT/src/module_update.cpp" \
    "$ROOT/src/net.cpp" "$ROOT/src/net_policy.cpp" \
    "$ROOT/src/logging.cpp" "$ROOT/src/bounded_log.cpp" -ldl -lz \
    -o "$BUILD_DIR/test_module_update"
"$BUILD_DIR/test_module_update"

${CXX:-c++} $COMPILE_FLAGS -I"$ROOT/src" \
    "$ROOT/tests/test_module_package.cpp" -lz -o "$BUILD_DIR/test_module_package"
"$BUILD_DIR/test_module_package"

${CXX:-c++} $COMPILE_FLAGS \
    -I"$ROOT/src" \
    "$ROOT/tests/test_rule_bundle.cpp" \
    "$ROOT/src/rule_bundle.cpp" \
    -o "$BUILD_DIR/test_rule_bundle"

"$BUILD_DIR/test_rule_bundle"

${CXX:-c++} $COMPILE_FLAGS -DADK_RULE_BUNDLE_TESTING -I"$ROOT/src" \
    "$ROOT/tests/test_rule_bundle_gc.cpp" "$ROOT/src/rule_bundle.cpp" \
    -o "$BUILD_DIR/test_rule_bundle_gc"
"$BUILD_DIR/test_rule_bundle_gc"

${CXX:-c++} $COMPILE_FLAGS \
    -I"$ROOT/src" \
    "$ROOT/tests/test_update_job.cpp" \
    "$ROOT/src/update_job.cpp" \
    -o "$BUILD_DIR/test_update_job"

"$BUILD_DIR/test_update_job"

${CXX:-c++} $COMPILE_FLAGS -I"$ROOT/src" \
    "$ROOT/tests/test_job_identity.cpp" "$ROOT/src/update_job.cpp" -o "$BUILD_DIR/test_job_identity"
"$BUILD_DIR/test_job_identity"

${CXX:-c++} $COMPILE_FLAGS -I"$ROOT/src" "$ROOT/tests/test_task_lease.cpp" -o "$BUILD_DIR/test_task_lease"
"$BUILD_DIR/test_task_lease"

${CXX:-c++} $COMPILE_FLAGS -I"$ROOT/src" \
    "$ROOT/tests/test_job_log.cpp" "$ROOT/src/update_job.cpp" "$ROOT/src/bounded_log.cpp" -o "$BUILD_DIR/test_job_log"
"$BUILD_DIR/test_job_log"

${CXX:-c++} $COMPILE_FLAGS \
    -I"$ROOT/src" \
    "$ROOT/tests/test_bounded_log.cpp" \
    "$ROOT/src/bounded_log.cpp" \
    -o "$BUILD_DIR/test_bounded_log"

"$BUILD_DIR/test_bounded_log"

npm --prefix "$ROOT/frontend" test
npm --prefix "$ROOT/frontend" run build
python3 "$ROOT/tests/test_webui_contract.py" "$ROOT/module/webroot/index.html"
python3 "$ROOT/tests/test_engine_lifecycle_contract.py"
python3 "$ROOT/tests/test_module_lifecycle_contract.py"
python3 "$ROOT/tests/test_boot_service.py"
python3 "$ROOT/tests/test_package.py"
python3 "$ROOT/tests/test_manager_update.py"
python3 "$ROOT/tests/test_listing_and_release.py"
python3 "$ROOT/tests/test_release_contract.py"

if [ "${CHECK_RULE_ARTIFACTS:-0}" = "1" ]; then
    ${CXX:-c++} $COMPILE_FLAGS \
        -I"$ROOT/src" \
        "$ROOT/tests/check_rule_artifacts.cpp" \
        "$ROOT/src/rules.cpp" \
        "$ROOT/src/rule_stream.cpp" \
        "$ROOT/src/rule_bundle.cpp" \
        "$ROOT/src/logging.cpp" \
        "$ROOT/src/bounded_log.cpp" \
        -lz \
        -o "$BUILD_DIR/check_rule_artifacts"
    "$BUILD_DIR/check_rule_artifacts" \
        "$ROOT/data/out_rules/hosts_rules.gz" \
        "$ROOT/data/out_rules/adguard_rules.gz" \
        "$ROOT/data/out_rules/rule_stats.conf"
fi
