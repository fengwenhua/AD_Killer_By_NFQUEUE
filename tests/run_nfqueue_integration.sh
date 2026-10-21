#!/bin/sh
set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
TMP_BASE=${TMPDIR:-/tmp}
BUILD_DIR=$(mktemp -d "${TMP_BASE%/}/ad_killer_by_nfqueue_nfqueue.XXXXXX")
QUEUE_NUM=42420
TEST_PORT=49555
READY_FILE="$BUILD_DIR/ready"
MARKED_FILE="$BUILD_DIR/marked"
PROBE_PID=
IPV4_RULE_ADDED=0
IPV4_MARK_RULE_ADDED=0
IPV4_SEED_RULE_ADDED=0
IPV6_RULE_ADDED=0
IPV6_PASS_RULE_ADDED=0
IPV6_SEED_RULE_ADDED=0
BLOCK_SPEC=
PASS_SPEC=
if [ "$(id -u)" -eq 0 ]; then
    as_root() { "$@"; }
else
    as_root() { sudo "$@"; }
fi

cleanup() {
    status=$?
    trap - EXIT HUP INT TERM
    if [ "$IPV4_MARK_RULE_ADDED" = 1 ]; then
        as_root iptables -w 5 -t mangle -D OUTPUT -p udp -d 127.0.0.1 \
            --dport "$TEST_PORT" -m connmark \
            --mark "$BLOCK_SPEC" -j DROP >/dev/null 2>&1 || true
    fi
    if [ "$IPV4_SEED_RULE_ADDED" = 1 ]; then
        as_root iptables -w 5 -t mangle -D OUTPUT -p udp -d 127.0.0.1 \
            --dport "$TEST_PORT" -j CONNMARK --set-xmark \
            0x030abcde/0x03ffffff >/dev/null 2>&1 || true
    fi
    if [ "$IPV4_RULE_ADDED" = 1 ]; then
        as_root iptables -w 5 -t mangle -D OUTPUT -p udp -d 127.0.0.1 \
            --dport "$TEST_PORT" -j NFQUEUE --queue-num "$QUEUE_NUM" \
            --queue-bypass >/dev/null 2>&1 || true
    fi
    if [ "$IPV6_PASS_RULE_ADDED" = 1 ]; then
        as_root ip6tables -w 5 -t mangle -D OUTPUT -p udp -d ::1 \
            --dport "$TEST_PORT" -m connmark \
            --mark "$PASS_SPEC" -j RETURN >/dev/null 2>&1 || true
    fi
    if [ "$IPV6_SEED_RULE_ADDED" = 1 ]; then
        as_root ip6tables -w 5 -t mangle -D OUTPUT -p udp -d ::1 \
            --dport "$TEST_PORT" -j CONNMARK --set-xmark \
            0x030abcde/0x03ffffff >/dev/null 2>&1 || true
    fi
    if [ "$IPV6_RULE_ADDED" = 1 ]; then
        as_root ip6tables -w 5 -t mangle -D OUTPUT -p udp -d ::1 \
            --dport "$TEST_PORT" -j NFQUEUE --queue-num "$QUEUE_NUM" \
            --queue-bypass >/dev/null 2>&1 || true
    fi
    if [ -n "$PROBE_PID" ] && kill -0 "$PROBE_PID" 2>/dev/null; then
        as_root kill "$PROBE_PID" >/dev/null 2>&1 || true
        wait "$PROBE_PID" 2>/dev/null || true
    fi
    rm -rf -- "$BUILD_DIR"
    exit "$status"
}
trap cleanup EXIT HUP INT TERM

command -v "${CXX:-c++}" >/dev/null 2>&1 || {
    echo "missing C++ compiler: ${CXX:-c++}" >&2
    exit 1
}
if [ "$(id -u)" -ne 0 ]; then
    command -v sudo >/dev/null 2>&1 || {
        echo "missing integration dependency: sudo" >&2
        exit 1
    }
fi
for command in iptables ip6tables python3; do
    command -v "$command" >/dev/null 2>&1 || {
        echo "missing integration dependency: $command" >&2
        exit 1
    }
done

"${CXX:-c++}" -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -pthread \
    -I"$ROOT/src" \
    "$ROOT/tests/integration/nfqueue_probe.cpp" \
    "$ROOT/src/nfnetlink.cpp" "$ROOT/src/nfnetlink_parser.cpp" \
    -o "$BUILD_DIR/nfqueue_probe"

MARK_SPECS=$("$BUILD_DIR/nfqueue_probe" --mark-specs)
BLOCK_SPEC=${MARK_SPECS%% *}
PASS_SPEC=${MARK_SPECS#* }

# 模块可能内建于 runner 内核；modprobe 失败不能单独判定不支持，真实 bind 才是门禁。
as_root modprobe nfnetlink_queue >/dev/null 2>&1 || true
as_root "$BUILD_DIR/nfqueue_probe" "$QUEUE_NUM" "$READY_FILE" "$MARKED_FILE" &
PROBE_PID=$!

wait_ready() {
    i=0
    while [ ! -s "$READY_FILE" ]; do
        if ! kill -0 "$PROBE_PID" 2>/dev/null; then
            wait "$PROBE_PID"
            exit 1
        fi
        i=$((i + 1))
        [ "$i" -ge 100 ] && {
            echo "NFQUEUE probe readiness timeout" >&2
            exit 1
        }
        sleep 0.05
    done
}
wait_ready

IPV4_RULE_ADDED=1
as_root iptables -w 5 -t mangle -I OUTPUT 1 -p udp -d 127.0.0.1 \
    --dport "$TEST_PORT" -j NFQUEUE --queue-num "$QUEUE_NUM" --queue-bypass
IPV4_MARK_RULE_ADDED=1
as_root iptables -w 5 -t mangle -I OUTPUT 1 -p udp -d 127.0.0.1 \
    --dport "$TEST_PORT" -m connmark \
    --mark "$BLOCK_SPEC" -j DROP
IPV4_SEED_RULE_ADDED=1
as_root iptables -w 5 -t mangle -I OUTPUT 1 -p udp -d 127.0.0.1 \
    --dport "$TEST_PORT" -j CONNMARK --set-xmark 0x030abcde/0x03ffffff
IPV6_RULE_ADDED=1
as_root ip6tables -w 5 -t mangle -I OUTPUT 1 -p udp -d ::1 \
    --dport "$TEST_PORT" -j NFQUEUE --queue-num "$QUEUE_NUM" --queue-bypass
IPV6_PASS_RULE_ADDED=1
as_root ip6tables -w 5 -t mangle -I OUTPUT 1 -p udp -d ::1 \
    --dport "$TEST_PORT" -m connmark \
    --mark "$PASS_SPEC" -j RETURN
IPV6_SEED_RULE_ADDED=1
as_root ip6tables -w 5 -t mangle -I OUTPUT 1 -p udp -d ::1 \
    --dport "$TEST_PORT" -j CONNMARK --set-xmark 0x030abcde/0x03ffffff

python3 - "$TEST_PORT" "$MARKED_FILE" <<'PY'
import errno
import os
import socket
import sys
import time

port = int(sys.argv[1])
marked_file = sys.argv[2]
payload = b"ad-killer-by-nfqueue-nfqueue-probe"

with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
    receiver.bind(("127.0.0.1", port))
    receiver.settimeout(2)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
        sender.connect(("127.0.0.1", port))
        sender.send(payload)
        if receiver.recv(4096) != payload:
            raise RuntimeError("IPv4 probe payload changed")
        deadline = time.monotonic() + 2
        while not os.path.exists(marked_file) and time.monotonic() < deadline:
            time.sleep(0.01)
        if not os.path.exists(marked_file):
            raise RuntimeError("connmark verdict acknowledgement timeout")
        try:
            sender.send(payload)
        except OSError as exc:
            # A locally generated packet rejected by netfilter may report the
            # DROP synchronously instead of making recv() time out.
            if exc.errno not in (errno.EPERM, errno.EACCES):
                raise
        else:
            try:
                receiver.recv(4096)
            except socket.timeout:
                pass
            else:
                raise RuntimeError("connmark DROP did not block the second datagram")

with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as receiver:
    receiver.bind(("::1", port))
    receiver.settimeout(2)
    with socket.socket(socket.AF_INET6, socket.SOCK_DGRAM) as sender:
        sender.connect(("::1", port))
        sender.send(payload)
        if receiver.recv(4096) != payload:
            raise RuntimeError("IPv6 probe payload changed")
        sender.send(payload)
        if receiver.recv(4096) != payload:
            raise RuntimeError("PASS connmark did not bypass the queue")
PY

wait "$PROBE_PID"
PROBE_PID=

as_root "$BUILD_DIR/nfqueue_probe" "$QUEUE_NUM" "$READY_FILE" "$MARKED_FILE" --saturate &
PROBE_PID=$!
wait_ready
python3 - "$TEST_PORT" "$MARKED_FILE" <<'PY'
import socket
import sys
from pathlib import Path

port = int(sys.argv[1])
first = b"ad-killer-by-nfqueue-nfqueue-probe-held"
second = b"ad-killer-by-nfqueue-nfqueue-probe-overflow"
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
    receiver.bind(("127.0.0.1", port))
    receiver.settimeout(3)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender, \
            socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as overflow_sender:
        sender.sendto(first, ("127.0.0.1", port))
        overflow_sender.sendto(second, ("127.0.0.1", port))
        if receiver.recv(4096) != second:
            raise RuntimeError("full queue did not fail open while the first packet was held")
        Path(sys.argv[2]).touch()
        if receiver.recv(4096) != first:
            raise RuntimeError("held packet did not receive its later ACCEPT verdict")
PY
wait "$PROBE_PID"
PROBE_PID=

# Listener is now gone, but the queue-bypass rule remains until the EXIT trap.
python3 - "$TEST_PORT" <<'PY'
import socket
import sys

port = int(sys.argv[1])
payload = b"ad-killer-by-nfqueue-no-listener-bypass"
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as receiver:
    receiver.bind(("127.0.0.1", port))
    receiver.settimeout(2)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sender:
        sender.sendto(payload, ("127.0.0.1", port))
        if receiver.recv(4096) != payload:
            raise RuntimeError("queue-bypass did not work without a listener")
print("real NFQUEUE no-listener queue-bypass test passed")
PY
