#!/usr/bin/env python3
"""Explicit disposable Linux/root: actual cached CLI sync and daemon RSS phases.

Compile the CLI with unavailable network backends so frozen validated groups
take the real cache-fallback path. This is not fresh-network or Android RSS.
wait4 reports the CLI's kernel peak; daemon phases use /proc VmHWM, with an
identity-checked clear_refs=5 reset before reload. Never sum fork/CoW RSS.
"""
import argparse
import fcntl
import gzip
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import time

DATA = Path('/data/adb/AD_Killer_By_NFQUEUE')


def memory(pid):
    fields = {}
    for line in Path(f'/proc/{pid}/status').read_text().splitlines():
        if line.startswith(('VmRSS:', 'VmHWM:')):
            key, value, unit = line.split()
            assert unit == 'kB'
            fields[key[:-1]] = int(value)
    return fields


def identity(pid, binary):
    text = Path(f'/proc/{pid}/stat').read_text()
    tail = text[text.rfind(')')+1:].split()
    assert tail[0] not in ('Z', 'X')
    image = Path(f'/proc/{pid}/exe').stat()
    expected = binary.stat()
    assert (image.st_dev, image.st_ino) == (expected.st_dev, expected.st_ino)
    return pid, int(tail[19]), image.st_dev, image.st_ino


def launch(binary, arguments, log):
    with log.open('wb') as output:
        pid = os.posix_spawn(str(binary), [str(binary), *arguments], os.environ,
                             setsid=True, file_actions=[(os.POSIX_SPAWN_DUP2, output.fileno(), 1),
                                                       (os.POSIX_SPAWN_DUP2, output.fileno(), 2)])
    return pid


def measure(binary, arguments, log, seconds=600):
    start = time.monotonic()
    pid = launch(binary, arguments, log)
    reaped = False
    try:
        while True:
            found, status, usage = os.wait4(pid, os.WNOHANG)
            if found:
                reaped = True
                code = os.waitstatus_to_exitcode(status)
                assert code == 0, (arguments, code, log.read_text()[-4000:])
                return {'elapsed_seconds': round(time.monotonic()-start, 4),
                        'peak_kib': usage.ru_maxrss,
                        'user_seconds': usage.ru_utime, 'system_seconds': usage.ru_stime}
            assert time.monotonic()-start < seconds, ('process timeout', arguments)
            time.sleep(.01)
    except BaseException:
        # The unreaped leader pins this process-group identity until wait4.
        if not reaped:
            os.killpg(pid, signal.SIGKILL)
            os.wait4(pid, 0)
        raise


def command(binary, *args):
    result = subprocess.run([str(binary), *args], check=True, capture_output=True, text=True, timeout=30)
    return result.stdout


def wait(check, description, seconds=90):
    deadline = time.monotonic()+seconds
    while time.monotonic() < deadline:
        value = check()
        if value:
            return value
        time.sleep(.02)
    raise AssertionError(description)


def snapshot(output):
    generation = (DATA/'rules.current').read_text().strip()
    assert re.fullmatch(r'[A-Za-z0-9_-]{1,128}', generation)
    source = DATA/'rulesets'/generation
    summary = {}
    for name in ('hosts_rules.gz', 'adguard_rules.gz'):
        shutil.copyfile(source/name, output/name)
        body = gzip.decompress((output/name).read_bytes())
        lines = sorted({line for line in body.splitlines() if line and not line.startswith(b'#')})
        summary[name] = {'canonical_sha256': hashlib.sha256(b'\n'.join(lines)+b'\n').hexdigest(),
                         'unique_lines': len(lines), 'decompressed_bytes': len(body)}
    shutil.copyfile(DATA/'available_rule_stats.conf', output/'rule_stats.conf')
    return summary


def clean_data():
    if not DATA.exists():
        return
    assert not DATA.is_symlink()
    lock = DATA.parent/'.AD_Killer_By_NFQUEUE.operations.lock'
    fd = os.open(lock, os.O_RDWR | os.O_NOFOLLOW)
    try:
        # A killed sync can still have an inherited helper lease. Preserve the
        # whole disposable tree unless every writer is proven quiescent.
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        shutil.rmtree(DATA)
    finally:
        os.close(fd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--prime', type=Path, required=True)
    parser.add_argument('--snapshot', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    assert os.geteuid() == 0 and os.uname().sysname == 'Linux' and not os.path.lexists(DATA), 'requires clean disposable Linux/root'
    args.binary = args.binary.resolve()
    args.output.mkdir(mode=0o700, parents=True, exist_ok=False)
    copied = args.output/'inputs'
    shutil.copytree(args.snapshot, copied)
    manifest = json.loads((copied/'benchmark-inputs.json').read_text())
    expected_rows = [(item['format'], item['file'], item['url']) for item in manifest['inputs']
                     if item['format'] != 'paired-hosts']
    actual_rows = [tuple(line.split('\t')) for line in (copied/'sources.tsv').read_text().splitlines()]
    assert actual_rows == expected_rows, 'source identities/order differ from frozen manifest'
    for item in manifest['inputs']:
        path = copied/item['file']
        assert path.parent == copied and hashlib.sha256(path.read_bytes()).hexdigest() == item['sha256']
    for path in copied.rglob('*'):
        path.chmod(0o700 if path.is_dir() else 0o600)
    pid = None
    report = {'schema': 1, 'mode': 'cached_sync_replay_not_fresh_network',
              'binary_sha256': hashlib.sha256(args.binary.read_bytes()).hexdigest(),
              'input_manifest_sha256': hashlib.sha256((copied/'benchmark-inputs.json').read_bytes()).hexdigest(),
              'kernel': os.uname().release, 'phases': {}}
    try:
        command(args.binary, '--install')
        measure(args.prime.resolve(), [str(copied)], args.output/'prime.log')
        report['phases']['cached_sync'] = measure(args.binary, ['--update'], args.output/'sync.log')
        report['canonical'] = snapshot(args.output)
        start = time.monotonic()
        pid = launch(args.binary, ['--daemon', '--foreground'], args.output/'daemon.log')
        expected = wait(lambda: identity(pid, args.binary), 'daemon identity missing')
        def running():
            assert identity(pid, args.binary) == expected
            return (DATA/'runtime.state').exists() and (DATA/'runtime.state').read_text().strip() == 'running'
        wait(running, 'daemon did not become running')
        time.sleep(1)
        report['phases']['startup'] = {**memory(pid), 'elapsed_seconds': round(time.monotonic()-start, 4)}
        assert identity(pid, args.binary) == expected
        Path(f'/proc/{pid}/clear_refs').write_text('5\n')
        assert identity(pid, args.binary) == expected
        start = time.monotonic()
        reply = json.loads(command(args.binary, '--reload').splitlines()[-1])
        revision = reply['revision']
        wait(lambda: json.loads(command(args.binary, '--apply-status', revision))['status'] == 'applied',
             'exact reload revision did not apply')
        assert identity(pid, args.binary) == expected
        at_ack = memory(pid)
        time.sleep(1)
        report['phases']['reload'] = {'at_ack': at_ack, 'settled': memory(pid),
                                       'elapsed_seconds': round(time.monotonic()-start, 4),
                                       'peak_reset': 'owned_process_clear_refs_5'}
        command(args.binary, '--stop')
        _, status, _ = os.wait4(pid, 0)
        pid = None
        assert os.waitstatus_to_exitcode(status) == 0
        report['measurements_complete'] = True
    finally:
        report['complete'] = False
        try:
            if pid is not None:
                subprocess.run([str(args.binary), '--stop'], check=True, capture_output=True, timeout=30)
                deadline = time.monotonic()+30
                while os.wait4(pid, os.WNOHANG)[0] == 0:
                    assert time.monotonic() < deadline, 'daemon exit unknown; preserve test data'
                    time.sleep(.05)
            clean_data()
            report['cleanup_complete'] = True
            report['complete'] = report.get('measurements_complete', False)
        finally:
            (args.output/'report.json').write_text(json.dumps(report, indent=2)+'\n')
    print(json.dumps(report))


if __name__ == '__main__':
    main()
