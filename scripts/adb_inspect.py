#!/usr/bin/env python3
"""Read-only, explicitly targeted AD Killer inspection and passive baseline.

Never invokes the installed module CLI, sends DNS probes, or reads DNS history.
Device commands are fixed reads. Raw configuration, routing rules, cgroup paths,
serials and command stderr are not written to the report.
"""
import argparse
import base64
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path, PurePosixPath
import re
import selectors
import shlex
import shutil
import subprocess
import time

MODULE = 'AD_Killer_By_NFQUEUE'
DATA = '/data/adb/' + MODULE
MODULE_DIR = '/data/adb/modules/' + MODULE
MAX_OUTPUT = 2 * 1024 * 1024
METRIC_KEYS = frozenset('dns_packets tls_packets block_exact_hits block_suffix_hits whitelist_hits default_passes blocked_retransmits verdict_failures queue_errors stale_verdicts dns_negative_replies tcp_resets quic_rejects response_failures transport_parse_failures domain_parse_failures truncated_packets reassembly_flows reassembly_bytes reassembly_expired reassembly_evicted dns_log_enqueued dns_log_written dns_log_dropped dns_log_write_failures dns_log_pending dns_log_pending_bytes dns_drop_verdicts inspected_drop_verdicts'.split())


def run_bounded(args, timeout=15, limit=MAX_OUTPUT):
    """Bound stdout/stderr and the lifetime of this owned adb client."""
    child = subprocess.Popen(args, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    selector = selectors.DefaultSelector()
    output, errors = bytearray(), bytearray()
    for stream, destination in ((child.stdout, output), (child.stderr, errors)):
        os.set_blocking(stream.fileno(), False)
        selector.register(stream, selectors.EVENT_READ, destination)
    deadline, reason = time.monotonic() + timeout, ''
    try:
        while selector.get_map():
            left = deadline - time.monotonic()
            if left <= 0:
                reason = 'timeout'
                break
            for key, _ in selector.select(min(left, .2)):
                chunk = os.read(key.fileobj.fileno(), 65536)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                destination = key.data
                if len(destination) + len(chunk) > (limit if destination is output else 16384):
                    reason = 'output_limit'
                    break
                destination.extend(chunk)
            if reason:
                break
        if not reason:
            try:
                child.wait(timeout=max(.001, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                reason = 'timeout'
    finally:
        selector.close()
        # Popen.kill checks/reaps its own child, never a PID from a device file.
        if child.returncode is None:
            child.kill()
        try:
            child.wait(timeout=1)
        except subprocess.TimeoutExpired as error:
            raise RuntimeError('adb_client_exit_unconfirmed') from error
        child.stdout.close(); child.stderr.close()
    return {'code': child.returncode, 'data': bytes(output), 'error': reason}


PREAMBLE = r'''
adk_file() {
  adk_key="$1"; adk_limit="$2"; adk_path="$3"
  if [ -L "$adk_path" ]; then adk_state=symlink
  elif [ ! -e "$adk_path" ]; then adk_state=missing
  elif [ ! -f "$adk_path" ]; then adk_state=invalid_type
  elif [ ! -r "$adk_path" ]; then adk_state=unreadable
  else adk_state=ok; fi
  printf '@ADK:%s:%s\n' "$adk_key" "$adk_state"
  if [ "$adk_state" = ok ]; then
    { head -c "$((adk_limit+1))" "$adk_path"; adk_code=$?; printf '\n@EXIT:%s\n' "$adk_code"; } | base64
  fi
  printf '@END\n'
}
adk_cmd() {
  adk_key="$1"; adk_limit="$2"; shift 2
  printf '@ADK:%s:ok\n' "$adk_key"
  { "$@"; adk_code=$?; printf '\n@EXIT:%s\n' "$adk_code"; } | head -c "$((adk_limit+65))" | base64
  printf '@END\n'
}
'''


class Probe:
    def __init__(self, serial, adb=None, runner=run_bounded):
        if not serial or len(serial) > 256 or re.search(r'[\s\x00-\x20\x7f]', serial):
            raise ValueError('invalid_serial')
        self.serial, self.adb, self.runner = serial, adb or shutil.which('adb'), runner
        if not self.adb:
            raise ValueError('adb_not_found')

    def execute(self, script, caps):
        # adb joins shell arguments: quote the complete su script explicitly.
        remote = 'su -c ' + shlex.quote(PREAMBLE + script)
        result = self.runner([self.adb, '-s', self.serial, 'shell', remote])
        if result['error'] or result['code']:
            return {}, result['error'] or 'adb_or_root_failed'
        try:
            return parse_frames(result['data'], caps), ''
        except ValueError:
            return {}, 'invalid_or_incomplete_response'


def parse_frames(data, caps):
    if len(data) > MAX_OUTPUT:
        raise ValueError('response_limit')
    lines, result, index = data.decode('ascii').splitlines(), {}, 0
    while index < len(lines):
        header = re.fullmatch(r'@ADK:([a-z0-9_]+):(ok|missing|symlink|unreadable|invalid_type|unverified)', lines[index])
        if not header:
            raise ValueError('frame')
        key, state = header.groups()
        if key not in caps or key in result:
            raise ValueError('unexpected_or_duplicate_frame')
        index += 1
        encoded = []
        while index < len(lines) and lines[index] != '@END':
            encoded.append(lines[index]); index += 1
        if index >= len(lines):
            raise ValueError('unfinished_frame')
        index += 1
        raw = base64.b64decode(''.join(encoded), validate=True)
        if state != 'ok':
            if raw:
                raise ValueError('failure_payload')
            result[key] = {'status': state}
            continue
        split = raw.rfind(b'\n@EXIT:')
        if split < 0 or not re.fullmatch(rb'\n@EXIT:[0-9]{1,3}\n', raw[split:]):
            result[key] = {'status': 'truncated_or_failed'}
            continue
        code, body = int(raw[split + 7:].strip()), raw[:split]
        if code or len(body) > caps[key]:
            result[key] = {'status': 'command_failed' if code else 'oversize'}
        else:
            result[key] = {'status': 'ok', 'text': body.decode('utf-8', errors='strict')}
    if set(result) != set(caps):
        raise ValueError('missing_frame')
    return result


def text(sections, key):
    value = sections.get(key, {})
    return value.get('text', '') if value.get('status') == 'ok' else ''


def unsigned(value):
    return int(value) if re.fullmatch(r'[0-9]{1,20}', str(value)) and int(value) <= (1 << 64) - 1 else None


def values(body, separator='='):
    result = {}
    for line in body.splitlines():
        if separator not in line:
            continue
        key, value = line.split(separator, 1)
        if key.strip() in result:
            return {}
        result[key.strip()] = value.strip()
    return result


def process_stat(body):
    close = body.rfind(')')
    if close < 0 or '(' not in body[:close]:
        return None
    pid = unsigned(body[:body.find('(')].strip())
    fields = body[close + 1:].split()
    if not pid or len(fields) < 22:
        return None
    numbers = [unsigned(fields[index]) for index in (11, 12, 17, 19)]
    if any(value is None for value in numbers) or not numbers[-1] or numbers[0] + numbers[1] > (1 << 64) - 1:
        return None
    return {'pid': pid, 'state': fields[0], 'cpu_ticks': numbers[0] + numbers[1], 'threads': numbers[2], 'started': numbers[3]}


def base_script(include_hash=True):
    caps, commands = {}, []
    def file(key, path, cap=8192):
        caps[key] = cap; commands.append(f'adk_file {key} {cap} {shlex.quote(path)}')
    def cmd(key, args, cap=8192):
        caps[key] = cap; commands.append('adk_cmd ' + key + ' ' + str(cap) + ' ' + shlex.join(args))
    cmd('root_uid', ['id', '-u'], 32)
    commands.append('[ "$(id -u)" = 0 ] || exit 13')
    cmd('model', ['getprop', 'ro.product.model'], 128)
    cmd('sdk', ['getprop', 'ro.build.version.sdk'], 32)
    cmd('kernel', ['uname', '-r'], 256)
    cmd('clock_hz', ['getconf', 'CLK_TCK'], 32)
    file('boot', '/proc/sys/kernel/random/boot_id', 64)
    file('uptime', '/proc/uptime', 128)
    file('meminfo', '/proc/meminfo', 8192)
    file('module', MODULE_DIR + '/module.prop', 4096)
    cmd('image', ['stat', '-L', '-c', '%d:%i:%s', MODULE_DIR + '/' + MODULE], 128)
    if include_hash:
        cmd('image_sha', ['sha256sum', MODULE_DIR + '/' + MODULE], 512)
    for key, path, cap in [('pid', DATA + '/daemon.pid', 32), ('runtime', DATA + '/runtime.state', 128),
                           ('config', DATA + '/state.conf', 8192), ('metrics', DATA + '/metrics.conf', 16384),
                           ('queues', '/proc/net/netfilter/nfnetlink_queue', 32768), ('netlink', '/proc/net/netlink', 262144)]:
        file(key, path, cap)
    for family, program in (('ipv4', 'iptables'), ('ipv6', 'ip6tables')):
        # The exit marker survives filtering, so failed queries are not empty.
        script = ('{ timeout 3 ' + program + ' -w 1 -t mangle -S; adk_rc=$?; printf "\\nADK_ROUTE_EXIT:%s\\n" "$adk_rc"; } '
                  '| awk \'/ADK_NFQ_|^ADK_ROUTE_EXIT:/{print}\'')
        cmd(family, ['sh', '-c', script], 65536)
    cmd('disk', ['timeout', '3', 'df', '-k', DATA], 4096)
    for key, name in (('engine_log', 'engine.log'), ('dns_log', 'dns_query.log')):
        cmd(key, ['stat', '-L', '-c', '%s:%Y', DATA + '/log/' + name], 128)
    jobs = ('[ -d "$1" ] && [ ! -L "$1" ] || exit 1; adk_n=0; for adk_record in "$1"/update-*.state; do '
            '[ -e "$adk_record" ] || continue; adk_n=$((adk_n+1)); [ "$adk_n" -le 64 ] || exit 2; '
            '[ -f "$adk_record" ] && [ ! -L "$adk_record" ] && [ -r "$adk_record" ] || exit 2; '
            'adk_size=$(stat -L -c %s "$adk_record") || exit 2; [ "$adk_size" -le 4096 ] || exit 2; '
            'head -c 4096 "$adk_record" | awk -F= \'$1=="status"{print $2}\'; done')
    cmd('jobs', ['sh', '-c', jobs, 'sh', DATA + '/update_jobs'], 8192)
    commands.append('adk_pid=$(head -c 32 ' + shlex.quote(DATA + '/daemon.pid') + ' 2>/dev/null)')
    commands.append('adk_owned=0; case "$adk_pid" in ""|*[!0-9]*) ;; *) if [ "${#adk_pid}" -le 10 ] && [ "$adk_pid" -gt 1 ] 2>/dev/null; then '
                    'adk_expected=$(stat -L -c "%d:%i:%s" ' + shlex.quote(MODULE_DIR + '/' + MODULE) + ' 2>/dev/null); '
                    'adk_actual=$(stat -L -c "%d:%i:%s" "/proc/$adk_pid/exe" 2>/dev/null); '
                    '[ -n "$adk_expected" ] && [ "$adk_expected" = "$adk_actual" ] && adk_owned=1; fi ;; esac')
    for key, suffix, cap in [('pstat', 'stat', 4096), ('pstatus', 'status', 8192), ('pio', 'io', 4096),
                              ('cgroup', 'cgroup', 8192), ('mountinfo', 'mountinfo', 262144)]:
        caps[key] = cap
        commands.append(f'if [ "$adk_owned" = 1 ]; then adk_file {key} {cap} "/proc/$adk_pid/{suffix}"; else printf "@ADK:{key}:unverified\\n@END\\n"; fi')
    sockets = '[ -d /proc/"$1"/fd ] && [ -r /proc/"$1"/fd ] || exit 1; adk_n=0; for adk_fd in /proc/"$1"/fd/*; do [ -L "$adk_fd" ] || continue; adk_n=$((adk_n+1)); [ "$adk_n" -le 256 ] || exit 2; adk_link=$(readlink "$adk_fd" 2>/dev/null) || exit 2; case "$adk_link" in socket:\\[*\\]) printf "%s\\n" "$adk_link";; esac; done'
    caps['sockets'] = 16384
    commands.append('if [ "$adk_owned" = 1 ]; then adk_cmd sockets 16384 sh -c ' + shlex.quote(sockets) + ' sh "$adk_pid"; else printf "@ADK:sockets:unverified\\n@END\\n"; fi')
    caps['pstat_after'] = 4096
    commands.append('if [ "$adk_owned" = 1 ]; then adk_file pstat_after 4096 "/proc/$adk_pid/stat"; else printf "@ADK:pstat_after:unverified\\n@END\\n"; fi')
    file('pid_after', DATA + '/daemon.pid', 32)
    cmd('image_after', ['stat', '-L', '-c', '%d:%i:%s', MODULE_DIR + '/' + MODULE], 128)
    return '\n'.join(commands), caps


def route_summary(section):
    if section.get('status') != 'ok':
        return {'status': section.get('status', 'unknown')}
    lines = section.get('text', '').splitlines()
    if not lines or not re.fullmatch(r'ADK_ROUTE_EXIT:[0-9]+', lines[-1]):
        return {'status': 'unverified_query'}
    if lines[-1] != 'ADK_ROUTE_EXIT:0':
        return {'status': 'query_failed'}
    chains, rules, jumps = set(), 0, 0
    for line in lines[:-1]:
        if not line:
            continue
        try:
            args = shlex.split(line)
        except ValueError:
            return {'status': 'invalid_rules'}
        if len(args) < 2:
            return {'status': 'invalid_rules'}
        owned = args[1].startswith('ADK_NFQ_')
        if owned:
            chains.add(args[1]); rules += args[0] == '-A'
        if '-j' in args:
            index = args.index('-j') + 1
            if index < len(args) and args[index].startswith('ADK_NFQ_'):
                jumps += 1
    return {'status': 'ok', 'chains': sorted(chains), 'rule_count': rules, 'jump_count': jumps}


def queue_summary(sections, owned):
    if sections.get('queues', {}).get('status') != 'ok':
        return {'status': sections.get('queues', {}).get('status', 'unknown')}
    ports, sockets = {}, set(re.findall(r'^socket:\[([0-9]+)\]$', text(sections, 'sockets'), re.M))
    for line in text(sections, 'netlink').splitlines():
        fields = line.split()
        if len(fields) == 10 and fields[1] == '12' and fields[9] in sockets:
            ports[fields[2]] = fields[9]
    result = []
    for line in text(sections, 'queues').splitlines():
        fields = line.split()
        if not 9 <= len(fields) <= 16 or any(unsigned(value) is None for value in fields):
            return {'status': 'invalid_queue_records'}
        numbers = list(map(int, fields))
        if numbers[0] not in (0, 1):
            continue
        verified = owned and sections.get('sockets', {}).get('status') == sections.get('netlink', {}).get('status') == 'ok'
        result.append({'queue': numbers[0], 'owned': True if verified and fields[1] in ports else None,
                       'queued': numbers[2], 'kernel_dropped': numbers[5], 'user_dropped': numbers[6], 'sequence': numbers[7]})
    return {'status': 'ok', 'listeners': result}


def freezer_paths(membership, mountinfo):
    groups, seen = [], set()
    for line in membership.splitlines():
        fields = line.split(':', 2)
        if len(fields) != 3 or unsigned(fields[0]) is None:
            raise ValueError('invalid_membership')
        unified = fields[:2] == ['0', '']
        if not unified and 'freezer' not in fields[1].split(','):
            continue
        if unified in seen:
            raise ValueError('ambiguous_membership')
        seen.add(unified)
        path = PurePosixPath(fields[2])
        if not fields[2].startswith('/') or '..' in path.parts or any(ord(ch) < 32 for ch in fields[2]):
            raise ValueError('unsafe_membership')
        if fields[2] == '/':
            continue
        candidates = []
        for row in mountinfo.splitlines():
            left, separator, right = row.partition(' - ')
            columns, tail = left.split(), right.split()
            if not separator or len(columns) < 5 or len(tail) < 3:
                continue
            if tail[0] != ('cgroup2' if unified else 'cgroup') or (not unified and 'freezer' not in tail[2].split(',')) or columns[3] != '/':
                continue
            mount = re.sub(r'\\([0-7]{3})', lambda match: chr(int(match[1], 8)), columns[4])
            if not (mount == '/dev/freezer' or mount == '/sys/fs/cgroup' or mount.startswith('/sys/fs/cgroup/')) or '..' in PurePosixPath(mount).parts:
                continue
            candidates.append(mount)
        if not candidates:
            raise ValueError('full_root_mount_unavailable')
        groups.append((unified, candidates[0].rstrip('/') + fields[2] + ('/cgroup.events' if unified else '/freezer.state')))
    if not membership.strip() or len(groups) > 4:
        raise ValueError('unverified_membership')
    return groups


def freezer_summary(probe, sections, process):
    if not process:
        return {'state': 'unknown', 'reason': 'process_not_verified'}
    try:
        groups = freezer_paths(text(sections, 'cgroup'), text(sections, 'mountinfo'))
    except ValueError as error:
        return {'state': 'unknown', 'reason': str(error)}
    caps, commands = {}, []
    for index, (_, path) in enumerate(groups):
        caps['group_' + str(index)] = 4096
        commands.append('adk_file group_' + str(index) + ' 4096 ' + shlex.quote(path))
    for key, path, cap in [('cgroup', f"/proc/{process['pid']}/cgroup", 8192), ('mountinfo', f"/proc/{process['pid']}/mountinfo", 262144),
                           ('pstat', f"/proc/{process['pid']}/stat", 4096)]:
        caps[key] = cap; commands.append('adk_file ' + key + ' ' + str(cap) + ' ' + shlex.quote(path))
    after, error = probe.execute('\n'.join(commands), caps)
    stat = process_stat(text(after, 'pstat'))
    if error or not stat or (stat['pid'], stat['started']) != (process['pid'], process['started']) or any(text(after, key) != text(sections, key) for key in ('cgroup', 'mountinfo')):
        return {'state': 'unknown', 'reason': 'observation_changed_or_failed'}
    frozen, unknown = False, False
    for index, (unified, _) in enumerate(groups):
        body = text(after, 'group_' + str(index)).strip()
        if unified:
            events = [line.split() for line in body.splitlines() if line.split()[:1] == ['frozen']]
            if len(events) != 1 or events[0] not in (['frozen', '0'], ['frozen', '1']):
                unknown = True
            else:
                frozen |= events[0][1] == '1'
        elif body in ('FROZEN', 'FREEZING'):
            frozen = True
        elif body != 'THAWED':
            unknown = True
    return {'state': 'frozen' if frozen else 'unknown' if unknown else 'normal'}


def inspect(probe, include_hash=True):
    start = time.monotonic()
    script, caps = base_script(include_hash)
    sections, error = probe.execute(script, caps)
    result = {'schema': 1, 'observed_at': datetime.now(timezone.utc).isoformat(), 'status': 'unknown'}
    if error or text(sections, 'root_uid').strip() != '0':
        result['reason'] = error or 'root_not_verified'
        return result
    result['status'] = 'observed'
    module = values(text(sections, 'module'))
    result['module'] = {key: module[key] for key in ('id', 'version', 'versionCode', 'releaseChannel') if key in module}
    result['module']['status'] = sections.get('module', {}).get('status', 'unknown')
    image = text(sections, 'image').strip()
    result['module']['image_epoch'] = hashlib.sha256(image.encode()).hexdigest()[:16] if re.fullmatch(r'[0-9]+:[0-9]+:[0-9]+', image) else None
    digest = text(sections, 'image_sha').split()
    if digest and re.fullmatch(r'[0-9a-f]{64}', digest[0]):
        result['module']['binary_sha256'] = digest[0]
    boot = text(sections, 'boot').strip()
    result['device'] = {'model': text(sections, 'model').strip(), 'sdk': unsigned(text(sections, 'sdk').strip()),
                        'kernel': text(sections, 'kernel').strip(), 'boot_epoch': hashlib.sha256(boot.encode()).hexdigest()[:12] if re.fullmatch(r'[0-9a-f-]{36}', boot) else None}
    try:
        uptime = float(text(sections, 'uptime').split()[0])
        result['device']['uptime_seconds'] = uptime if math.isfinite(uptime) and uptime >= 0 else None
    except (ValueError, IndexError):
        result['device']['uptime_seconds'] = None
    result['device']['clock_hz'] = unsigned(text(sections, 'clock_hz').strip())
    memory = values(text(sections, 'meminfo'), ':')
    result['memory_kib'] = {key: unsigned(memory.get(key, '').split()[0]) if memory.get(key) else None for key in ('MemTotal', 'MemAvailable', 'MemFree')}
    before, after = process_stat(text(sections, 'pstat')), process_stat(text(sections, 'pstat_after'))
    owned = bool(result['module']['image_epoch'] and result['device']['boot_epoch'] and before and after and (before['pid'], before['started']) == (after['pid'], after['started']) and
                 text(sections, 'pid').strip() == text(sections, 'pid_after').strip() == str(before['pid']) and
                 text(sections, 'image').strip() == text(sections, 'image_after').strip() and module.get('id') == MODULE)
    result['process'] = {**(after if owned else {}), 'owner_verified': owned}
    if owned:
        status, io = values(text(sections, 'pstatus'), ':'), values(text(sections, 'pio'), ':')
        result['process']['memory_kib'] = {key: unsigned(status.get(key, '').split()[0]) if status.get(key) else None for key in ('VmRSS', 'VmHWM', 'VmData')}
        result['process']['io'] = {key: unsigned(io.get(key, '')) for key in ('read_bytes', 'write_bytes', 'syscr', 'syscw')}
    result['freezer'] = freezer_summary(probe, sections, after if owned else None)
    result['queues'] = queue_summary(sections, owned)
    metrics = values(text(sections, 'metrics'))
    bound = owned and metrics.get('metrics_format') == '2' and metrics.get('owner_boot') == boot and unsigned(metrics.get('owner_pid', '')) == after['pid'] and unsigned(metrics.get('owner_started', '')) == after['started']
    result['metrics'] = {'status': sections.get('metrics', {}).get('status', 'unknown'),
                         'quality': 'owner_verified_age_unavailable' if bound else 'unverified',
                         'counters': {key: value for key in sorted(METRIC_KEYS) if (value := unsigned(metrics.get(key, ''))) is not None}}
    result['routing'] = {family: route_summary(sections.get(family, {})) for family in ('ipv4', 'ipv6')}
    recorded = {}
    for state in text(sections, 'jobs').splitlines():
        key = state if state in ('running', 'success', 'failed', 'interrupted', 'queued') else 'unknown'
        recorded[key] = recorded.get(key, 0) + 1
    result['jobs'] = {'status': sections.get('jobs', {}).get('status', 'unknown'), 'recorded_states': recorded,
                      'live_identity_verification': 'not_performed'}
    config = values(text(sections, 'config'))
    result['settings'] = {'status': sections.get('config', {}).get('status', 'unknown'), **{key: config[key] for key in ('mode', 'scope', 'enabled', 'manual_stop', 'autostart', 'block_quic', 'dev_mode') if config.get(key) in ('true', 'false', '0', '1', '2')}}
    result['runtime'] = text(sections, 'runtime').strip() if re.fullmatch(r'[a-z_]+\s*', text(sections, 'runtime')) else 'unknown'
    result['logs'] = {}
    for key in ('engine_log', 'dns_log'):
        value = text(sections, key).strip().split(':')
        result['logs'][key] = {'status': sections.get(key, {}).get('status', 'unknown'),
                              'bytes': unsigned(value[0]) if len(value) == 2 else None, 'modified_at': unsigned(value[1]) if len(value) == 2 else None}
    disk = text(sections, 'disk').splitlines()
    result['disk'] = {'status': sections.get('disk', {}).get('status', 'unknown'), 'available_kib': None}
    if len(disk) >= 2:
        row = disk[-1].split()
        if len(row) >= 6:
            result['disk']['available_kib'] = unsigned(row[-3])
    result['sampling_overhead_ms'] = round((time.monotonic() - start) * 1000, 3)
    return result


def add_deltas(previous, current):
    before, after = previous.get('process', {}), current.get('process', {})
    same = before.get('owner_verified') and after.get('owner_verified') and all(before.get(key) == after.get(key) for key in ('pid', 'started')) and previous.get('device', {}).get('boot_epoch') == current.get('device', {}).get('boot_epoch') and previous.get('module', {}).get('image_epoch') == current.get('module', {}).get('image_epoch')
    current['delta'] = {'status': 'unverified_or_new_session', 'cpu_percent_one_core': None, 'read_bytes': None, 'write_bytes': None}
    if not same:
        return
    first, last = previous['device'].get('uptime_seconds'), current['device'].get('uptime_seconds')
    hz = current['device'].get('clock_hz')
    if first is None or last is None or not hz or last <= first or after['cpu_ticks'] < before['cpu_ticks']:
        return
    current['delta']['status'] = 'ok'
    current['delta']['cpu_percent_one_core'] = round(100 * (after['cpu_ticks'] - before['cpu_ticks']) / (hz * (last - first)), 4)
    for key in ('read_bytes', 'write_bytes'):
        left, right = before.get('io', {}).get(key), after.get('io', {}).get(key)
        if left is not None and right is not None and right >= left:
            current['delta'][key] = right - left


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True, help='explicit adb device serial; never stored in reports')
    parser.add_argument('--output', type=Path, required=True, help='new private local report directory')
    parser.add_argument('--duration', type=int, default=0, help='passive observation seconds, 0 for one snapshot')
    parser.add_argument('--interval', type=int, default=60)
    args = parser.parse_args()
    if not 0 <= args.duration <= 7 * 86400 or not 1 <= args.interval <= 3600:
        parser.error('duration/interval outside supported bounds')
    probe = Probe(args.serial)
    args.output.mkdir(mode=0o700, parents=True, exist_ok=False)
    metadata = {'schema': 1, 'started_at': datetime.now(timezone.utc).isoformat(), 'duration_requested_seconds': args.duration,
                'interval_seconds': args.interval, 'collector_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                'mode': 'passive_read_only', 'dns_history': 'not_collected', 'latency': 'not_probed', 'complete': False}
    sequence = 0
    def save_metadata():
        nonlocal sequence
        sequence += 1
        temporary = args.output / f'run.json.tmp.{os.getpid()}.{sequence}'
        fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'w') as stream:
            json.dump(metadata, stream, ensure_ascii=False, indent=2); stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary, args.output / 'run.json')
        directory = os.open(args.output, os.O_RDONLY | os.O_DIRECTORY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    save_metadata()
    started, previous, count, failed = None, {}, 0, 0
    fd = os.open(args.output / 'samples.jsonl', os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    print('Starting explicitly targeted, passive AD Killer baseline; no DNS history or active probes.', flush=True)
    try:
        with os.fdopen(fd, 'w') as stream:
            while True:
                sample = inspect(probe, include_hash=count % 60 == 0)
                if started is None:
                    started = time.monotonic()
                add_deltas(previous, sample)
                failed += sample.get('status') != 'observed'
                stream.write(json.dumps(sample, ensure_ascii=False, separators=(',', ':')) + '\n'); stream.flush()
                previous, count = sample, count + 1
                elapsed = time.monotonic() - started
                metadata.update(samples=count, failed_samples=failed, elapsed_seconds=round(elapsed, 3), last_status=sample['status'], acceptance='not_evaluated_requires_state_matrix_and_review')
                save_metadata()
                if count == 1 or count % 60 == 0:
                    print(json.dumps({'samples': count, 'failed': failed, 'elapsed_seconds': round(elapsed, 1), 'status': sample['status']}), flush=True)
                if elapsed >= args.duration:
                    os.fsync(stream.fileno())
                    metadata['complete'] = not failed
                    break
                time.sleep(min(args.interval, args.duration - elapsed))
    except (KeyboardInterrupt, RuntimeError, OSError, ValueError):
        metadata['reason'] = 'interrupted_or_collection_failed'
        raise
    finally:
        metadata['finished_at'] = datetime.now(timezone.utc).isoformat()
        save_metadata()
    return 0 if metadata['complete'] else 1


if __name__ == '__main__':
    sys_exit = main()
    raise SystemExit(sys_exit)
