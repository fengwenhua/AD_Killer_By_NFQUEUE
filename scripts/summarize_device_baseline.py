#!/usr/bin/env python3
"""Summarize existing private collector reports; never runs ADB or device code."""
import argparse
import collections
import json
import math
import os
from pathlib import Path
import re
import stat
import tempfile

from adb_inspect import METRIC_KEYS, MODULE

MAX_BYTES = 128 * 1024 * 1024
MAX_LINE = 65536
MAX_SAMPLES = 100000
GAUGES = frozenset(("reassembly_flows", "reassembly_bytes", "dns_log_pending", "dns_log_pending_bytes"))
OPERATION_ERRORS = frozenset(("queue_errors", "verdict_failures", "response_failures", "dns_log_dropped", "dns_log_write_failures"))


def object_value(value):
    return value if type(value) is dict else {}


def unsigned(value, maximum=(1 << 64) - 1):
    return value if type(value) is int and 0 <= value <= maximum else None


def finite(value, maximum=1e12):
    if type(value) not in (int, float):
        return None
    return float(value) if 0 <= value <= maximum and math.isfinite(value) else None


def matching(value, expression):
    return value if type(value) is str and re.fullmatch(expression, value) else None


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate report key')
        result[key] = value
    return result


def parse_record(data):
    def reject_constant(_):
        raise ValueError('nonfinite report JSON')
    try:
        value = json.loads(data, object_pairs_hook=unique_object, parse_constant=reject_constant)
    except (ValueError, RecursionError):
        raise ValueError('invalid report JSON') from None
    if type(value) is not dict or type(value.get('schema')) is not int or value['schema'] != 1:
        raise ValueError('unsupported report schema')
    return value


def signature(path):
    info = path.lstat()
    return info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns


def open_report(path, limit):
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        info = os.fstat(fd)
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size > limit:
            raise ValueError('report must be a bounded regular file without links')
        return os.fdopen(fd, 'rb'), (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns)
    except BaseException:
        os.close(fd)
        raise


def identity(sample):
    process = object_value(sample.get('process'))
    module = object_value(sample.get('module'))
    device = object_value(sample.get('device'))
    pid, started = unsigned(process.get('pid'), (1 << 31) - 1), unsigned(process.get('started'))
    boot = matching(device.get('boot_epoch'), '[0-9a-f]{12}')
    image = matching(module.get('image_epoch'), '[0-9a-f]{16}')
    version = matching(module.get('version'), 'v[0-9]{1,6}\\.[0-9]{1,6}(?:\\.[0-9]{1,6})?')
    code = matching(module.get('versionCode'), '[0-9]{1,10}')
    channel = module.get('releaseChannel')
    if (sample.get('status') != 'observed' or process.get('owner_verified') is not True or
            module.get('id') != MODULE or not pid or not started or not boot or not image or not version or
            not code or not 0 < int(code) < (1 << 31) or channel not in ('stable', 'preview')):
        return None
    return boot, image, pid, started, version, int(code), channel


def summarize(directory):
    directory = Path(directory)
    if directory.is_symlink() or not directory.is_dir():
        raise ValueError('report directory must be a real directory')
    run_path, samples_path = directory / 'run.json', directory / 'samples.jsonl'
    stream, run_signature = open_report(run_path, MAX_LINE)
    with stream:
        run = parse_record(stream.read(MAX_LINE + 1))
    interval = unsigned(run.get('interval_seconds'), 3600)
    if not interval:
        raise ValueError('invalid sampling interval')
    issues = collections.Counter()
    count = verified = observed = metric_owners = route_reads = 0
    cpu_seconds = verified_seconds = 0.0
    rss, hwm, models, versions, channels, sessions = [], [], set(), set(), set(), set()
    deltas, maxima, gauge_ranges = collections.Counter(), {}, {}
    queues = {str(q): {'verified_samples': 0, 'max_queued': None, 'max_kernel_dropped': None,
                      'max_user_dropped': None} for q in (0, 1)}
    previous = None
    stream, samples_signature = open_report(samples_path, MAX_BYTES)
    with stream:
        remaining = samples_signature[2]
        while remaining:
            line = stream.readline(min(MAX_LINE + 1, remaining))
            if not line or len(line) > MAX_LINE or not line.endswith(b'\n'):
                raise ValueError('oversized, truncated or unfinished sample record')
            remaining -= len(line)
            count += 1
            if count > MAX_SAMPLES:
                raise ValueError('sample count limit exceeded')
            sample = parse_record(line)
            observed += sample.get('status') == 'observed'
            key = identity(sample)
            process = object_value(sample.get('process'))
            device = object_value(sample.get('device'))
            module = object_value(sample.get('module'))
            model = matching(device.get('model'), '[A-Za-z0-9_. -]{1,64}')
            version = matching(module.get('version'), 'v[0-9]{1,6}\\.[0-9]{1,6}(?:\\.[0-9]{1,6})?')
            if model: models.add(model)
            if version: versions.add(version)
            if module.get("releaseChannel") in ("stable", "preview"): channels.add(module["releaseChannel"])
            if len(models) > 128 or len(versions) > 128:
                raise ValueError('report identity inventory limit exceeded')
            if key:
                verified += 1
                sessions.add(key)
                memory = object_value(process.get('memory_kib'))
                for name, destination in (('VmRSS', rss), ('VmHWM', hwm)):
                    value = unsigned(memory.get(name), 1 << 40)
                    if value is not None: destination.append(value)
            else:
                issues['process_unverified_samples'] += 1
            freezer = object_value(sample.get('freezer')).get('state')
            if freezer != 'normal':
                issues['frozen_samples' if freezer == 'frozen' else 'freezer_unknown_samples'] += 1
            routing = object_value(sample.get('routing'))
            if all(object_value(routing.get(family)).get('status') == 'ok' for family in ('ipv4', 'ipv6')):
                route_reads += 1
            else:
                issues['routing_query_unknown_samples'] += 1
            metrics = object_value(sample.get('metrics'))
            owner = bool(key and metrics.get('quality') == 'owner_verified_age_unavailable' and metrics.get('status') == 'ok')
            metric_owners += owner
            if not owner: issues['metrics_unverified_samples'] += 1
            counters = {name: value for name, raw in object_value(metrics.get('counters')).items()
                        if name in METRIC_KEYS and (value := unsigned(raw)) is not None} if owner else {}
            if any(counters.get(name, 0) > 0 for name in OPERATION_ERRORS):
                issues['operational_error_counter_observations'] += 1
            for name, value in counters.items():
                if name in GAUGES:
                    previous_range = gauge_ranges.get(name, (value, value))
                    gauge_ranges[name] = (min(previous_range[0], value), max(previous_range[1], value))
                else: maxima[name] = max(maxima.get(name, 0), value)
            uptime, hz = finite(device.get('uptime_seconds')), unsigned(device.get('clock_hz'), 1000000)
            ticks = unsigned(process.get('cpu_ticks'))
            if key and previous and key == previous['key'] and uptime is not None and previous['uptime'] is not None:
                seconds = uptime - previous['uptime']
                overhead = finite(sample.get('sampling_overhead_ms'), 120000)
                gap_limit = interval + (overhead or 0) / 1000 + max(5, interval * .25)
                if seconds <= 0:
                    issues['uptime_not_increasing_intervals'] += 1
                elif seconds > gap_limit:
                    issues['sampling_gap_intervals'] += 1
                else:
                    verified_seconds += seconds
                    if hz and hz == previous['hz'] and ticks is not None and previous['ticks'] is not None and ticks >= previous['ticks']:
                        cpu_seconds += (ticks - previous['ticks']) / hz
                    else:
                        issues['cpu_interval_unknown'] += 1
                    if owner and previous['metric_owner']:
                        for name in (counters.keys() & previous['counters'].keys()) - GAUGES:
                            delta = counters[name] - previous['counters'][name]
                            if delta < 0: issues['metric_counter_decrease_intervals'] += 1
                            else: deltas[name] += delta
            previous = {'key': key, 'uptime': uptime, 'hz': hz, 'ticks': ticks,
                        'metric_owner': owner, 'counters': counters} if key else None
            listeners = object_value(sample.get('queues'))
            values = listeners.get('listeners')
            indexed = {}
            if listeners.get('status') == 'ok' and type(values) is list:
                for listener in values:
                    listener = object_value(listener)
                    q = unsigned(listener.get('queue'), 1)
                    if q is not None:
                        if q in indexed: raise ValueError('duplicate queue observation')
                        indexed[q] = listener
            for q in (0, 1):
                listener = indexed.get(q, {})
                fields = {name: unsigned(listener.get(name)) for name in ('queued','kernel_dropped','user_dropped')}
                if not key or listener.get('owned') is not True or any(value is None for value in fields.values()):
                    issues['queue_unverified_samples'] += 1
                    continue
                if fields['queued']: issues['queue_backlog_observations'] += 1
                if fields['kernel_dropped'] or fields['user_dropped']: issues['queue_drop_counter_observations'] += 1
                item = queues[str(q)]
                item['verified_samples'] += 1
                for name, value in fields.items():
                    field = 'max_' + name
                    item[field] = max(item[field] or 0, value)
    changed = signature(run_path) != run_signature or signature(samples_path) != samples_signature
    if changed: issues['input_changed_during_read'] += 1
    if unsigned(run.get('samples')) != count or unsigned(run.get('failed_samples')) != count - observed:
        issues['collection_metadata_mismatch'] += 1
    requested = unsigned(run.get('duration_requested_seconds'), 604800)
    elapsed = finite(run.get('elapsed_seconds'), 605000)
    if requested is None or elapsed is None:
        issues['duration_metadata_unknown'] += 1
    elif elapsed < requested:
        issues['requested_duration_not_completed'] += 1
    if not count: issues['no_samples'] += 1
    return {'schema': 1, 'kind': 'offline_observation_summary', 'samples': count,
            'collection_complete': run.get('complete') is True and not changed and not issues.get('collection_metadata_mismatch') and not issues.get('duration_metadata_unknown') and not issues.get('requested_duration_not_completed') and observed == count and count > 0,
            'requested_seconds': requested, 'reported_elapsed_seconds': elapsed,
            'verified_process_samples': verified, 'verified_sessions': len(sessions),
            'verified_interval_seconds': round(verified_seconds, 3),
            'cpu_percent_one_core_average': round(100 * cpu_seconds / verified_seconds, 4) if verified_seconds and not issues.get('cpu_interval_unknown') else None,
            'rss_mib_range': [round(min(rss)/1024, 4), round(max(rss)/1024, 4)] if rss else None,
            'hwm_mib_max': round(max(hwm)/1024, 4) if hwm else None,
            'recorded_models': sorted(models), 'recorded_versions': sorted(versions), 'recorded_channels': sorted(channels),
            'routing_query_readable_samples': route_reads, 'metrics_owner_samples': metric_owners,
            'counter_deltas_in_verified_intervals': dict(sorted(deltas.items())),
            'counter_observed_maxima': dict(sorted(maxima.items())),
            'gauge_observed_ranges': dict(sorted(gauge_ranges.items())), 'queues': queues,
            'issues': dict(sorted(issues.items())),
            'acceptance': 'not_evaluated_requires_device_state_matrix',
            'limitations': ['recorded observations only; no ADB, live refresh or authenticity attestation',
                            'metric age unavailable; readable route summaries do not prove full routing correctness',
                            'collection completion and observed duration do not establish 24-hour stability']}


def write_new(path, value):
    path = Path(path)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(value, stream, ensure_ascii=False, indent=2, allow_nan=False)
            stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
        os.link(temporary, path, follow_symlinks=False)  # exclusive atomic publication
    finally:
        if temporary is not None: temporary.unlink(missing_ok=True)
    fd = os.open(path.parent, os.O_RDONLY)
    try: os.fsync(fd)
    finally: os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--compare', type=Path, help='another report; workloads/devices are not assumed equivalent')
    parser.add_argument('--output', type=Path, help='new local JSON file; never overwrite an existing file')
    args = parser.parse_args()
    value = summarize(args.report)
    if args.compare:
        value = {'schema': 1, 'kind': 'uncontrolled_observation_comparison',
                 'current': value, 'baseline': summarize(args.compare),
                 'interpretation': 'side-by-side records, not matched-workload performance or power evidence'}
    if args.output: write_new(args.output, value)
    else: print(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False))


if __name__ == '__main__':
    try: main()
    except (ValueError, OSError) as error: raise SystemExit('baseline summary failed: ' + str(error))
