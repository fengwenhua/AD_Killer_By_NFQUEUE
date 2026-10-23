#!/bin/sh
# Test-only fault boundary around the actual iptables binary, never shipped.
set -eu
: "${ADK_FIREWALL_FAULT_FILE:?missing test fault file}"
mode=''
IFS= read -r mode < "$ADK_FIREWALL_FAULT_FILE" || :
query=false
delete=false
mutation=false
replace=false
for argument do
    case "$argument" in
        -S) query=true ;;
        -X) delete=true; mutation=true ;;
        -R) mutation=true; replace=true ;;
        -F|-N|-D|-I) mutation=true ;;
    esac
done
if [ "$mode" = rollback ] && [ "$replace" = true ]; then
    if [ "${0##*/}" = ip6tables ]; then exit 1; fi
    for argument do
        if [ "$argument" = ADK_NFQ_OUT_A ]; then exit 1; fi
    done
fi
if [ "$mode" = hold ] && [ "$mutation" = true ]; then
    printf '%s\n' "$$" > "$ADK_FIREWALL_FAULT_FILE.entered"
    i=0
    while [ ! -f "$ADK_FIREWALL_FAULT_FILE.release" ]; do
        sleep 0.01
        i=$((i + 1))
        [ "$i" -lt 3000 ] || exit 90
    done
fi
if [ "${0##*/}" = ip6tables ]; then
    if [ "$mode" = delete ] && [ "$delete" = true ]; then
        printf '%s\n' 'injected IPv6 delete failure' >&2
        exit 1
    fi
    if [ "$mode" = query ] && [ "$query" = true ]; then
        printf '%s\n' '-P OUTPUT ACCEPT'
        printf '%s\n' 'injected IPv6 query failure' >&2
        exit 1
    fi
    if [ "$mode" = empty ] && [ "$query" = true ]; then exit 0; fi
    if [ "$mode" = timeout ] && [ "$query" = true ]; then
        i=0
        while [ ! -f "$ADK_FIREWALL_FAULT_FILE.timeout.release" ]; do
            sleep 0.01
            i=$((i + 1))
            [ "$i" -lt 3000 ] || exit 90
        done
    fi
    exec "${ADK_FIREWALL_REAL_6:?missing real ip6tables}" "$@"
fi
exec "${ADK_FIREWALL_REAL_4:?missing real iptables}" "$@"
