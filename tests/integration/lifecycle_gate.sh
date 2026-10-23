#!/bin/sh
# Disposable integration fixture, never shipped in the module.
set -eu
: "${ADK_LIFECYCLE_GATE:?missing exclusive test gate directory}"
printf '%s\n' "$$" > "$ADK_LIFECYCLE_GATE/entered"
i=0
while [ ! -f "$ADK_LIFECYCLE_GATE/release" ]; do
    sleep 0.01
    i=$((i + 1))
    [ "$i" -lt 2000 ] || exit 90
done
case "${0##*/}" in
    nohup) exec "$@" ;;
    iptables) exec "${ADK_LIFECYCLE_REAL_IPTABLES:?missing actual iptables}" "$@" ;;
    *) exit 91 ;;
esac
