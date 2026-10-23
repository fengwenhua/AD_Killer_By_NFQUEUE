#!/usr/bin/env python3
"""Real CLI multi-process barriers; all network/process identity effects isolated."""
import json
import fcntl
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

binary = sys.argv[1]
root = Path(subprocess.check_output([binary, 'root'], text=True).strip())
assert root.name == 'lifecycle-data' and not root.exists()


def run(*args):
    return subprocess.run([binary, *args], capture_output=True, text=True, timeout=15)


def wait(test):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        if test():
            return
        time.sleep(.005)
    raise AssertionError('barrier timeout')


def launch(ticket, boot=False):
    return subprocess.Popen([binary, '--daemon'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            env={**os.environ, 'ADK_NFQ_START_REVISION': ticket, 'ADK_NFQ_START_BOOT': '1' if boot else '0'})


def begin(boot=False):
    result = run('begin-boot' if boot else 'begin-start')
    assert result.returncode == 0, result.stderr
    return result.stdout.strip()


def reset():
    if root.exists():
        shutil.rmtree(root)  # Only the compile-time, exclusive test data root.
    root.mkdir(mode=0o700)
    (root / 'state.conf').write_text('enabled=false\nmanual_stop=true\nautostart=false\n')
    (root / 'queue.fixture').write_text('')


children = []
try:
    reset()
    # Direct daemon entry has no authority to bypass a persisted pause.
    direct = run('--daemon')
    assert direct.returncode != 0 and json.loads(direct.stdout)['status'] == 'error_start_cancelled'
    assert not (root / 'daemon.started').exists()

    # Pause arrives while the real daemon entry is before PID publication.
    ticket = begin()
    (root / 'startup.pause').touch()
    daemon = launch(ticket); children.append(daemon)
    wait(lambda: (root / 'startup.entered').exists() or daemon.poll() is not None)
    assert daemon.poll() is None, daemon.communicate()
    stop = subprocess.Popen([binary, '--stop'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True); children.append(stop)
    wait(lambda: 'manual_stop=true' in (root / 'state.conf').read_text())
    (root / 'startup.release').touch()
    daemon.communicate(timeout=10); assert daemon.returncode != 0
    out, err = stop.communicate(timeout=10)
    assert stop.returncode == 0, (out, err)
    assert not (root / 'daemon.pid').exists() and not (root / 'test.routes').exists()
    assert not (root / 'daemon.started').exists(), 'late startup reached engine after pause'

    # Old stop cannot clean the replacement instance: it holds the operation
    # lock while the newer start is registered but has not published a PID.
    reset()
    old = launch(begin()); children.append(old)
    wait(lambda: (root / 'daemon.started').exists())
    (root / 'cleanup.pause').touch()
    stop = subprocess.Popen([binary, '--stop'], stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True); children.append(stop)
    wait(lambda: (root / 'cleanup.entered').exists())
    new = launch(begin()); children.append(new)
    time.sleep(.1)
    assert new.poll() is None and not (root / 'daemon.pid').exists()
    assert not (root / 'test.routes').exists()
    (root / 'cleanup.release').touch()
    stop.communicate(timeout=10); assert stop.returncode == 0
    old.communicate(timeout=10); assert old.returncode == 0
    wait(lambda: (root / 'daemon.started').read_text() == str(new.pid))
    assert (root / 'test.routes').read_text() == str(new.pid)
    new.terminate(); new.communicate(timeout=10)

    # Independent windows: only the latest accepted request may publish.
    reset()
    earlier, later = begin(), begin()
    rejected = launch(earlier); children.append(rejected)
    rejected.communicate(timeout=10); assert rejected.returncode != 0
    valid = launch(later); children.append(valid)
    wait(lambda: (root / 'daemon.started').exists())
    assert (root / 'daemon.pid').read_text() == str(valid.pid)
    valid.terminate(); valid.communicate(timeout=10)

    # Boot authorization is checked again, not converted into a manual resume.
    reset()
    (root / 'state.conf').write_text('enabled=true\nmanual_stop=false\nautostart=true\n')
    ticket = begin(boot=True)
    assert run('--autostart-off').returncode == 0
    rejected = launch(ticket, boot=True); children.append(rejected)
    rejected.communicate(timeout=10); assert rejected.returncode != 0
    assert not (root / 'daemon.started').exists()
    assert 'autostart=false' in (root / 'state.conf').read_text()
    # Real stop control flow must propagate partial/unknown cleanup, keep the
    # failure durable and permit an explicit, successful retry.
    for fault, expected in [('partial', 'partial'), ('unknown', 'unknown')]:
        reset()
        (root / f'cleanup.{fault}').touch()
        failed = run('--stop')
        value = json.loads(failed.stdout)
        assert failed.returncode != 0 and value['status'].startswith('error_cleanup_')
        assert value['cleanup']['routes'] == expected
        assert (root / 'runtime.state').read_text() == 'error'
        assert 'cleanup_' in (root / 'engine_error.conf').read_text()
        (root / f'cleanup.{fault}').unlink()
        assert json.loads(run('--stop').stdout)['status'] == 'stopped'
        assert not (root / 'engine_error.conf').read_text()
    reset()
    (root / 'queue.fixture').write_text('0 3139568836 0 2 4096 0 0 9 1\n')
    failed = run('--stop'); value = json.loads(failed.stdout)
    assert failed.returncode != 0 and value['cleanup']['queues'] == 'unknown'
    assert value['cleanup']['routes'] == 'clean' and value['cleanup']['process_gone']
    (root / 'queue.fixture').write_text('')
    with (root / '.daemon.lock').open('w') as descriptor:
        fcntl.flock(descriptor, fcntl.LOCK_EX)
        failed = run('--stop'); value = json.loads(failed.stdout)
        assert failed.returncode != 0 and not value['cleanup']['process_gone']
    assert run('--stop').returncode == 0
    print('actual CLI barriers: paused late start, old cleanup/new PID, latest window and boot recheck passed')
finally:
    for marker in ('startup.release', 'cleanup.release'):
        if root.exists():
            (root / marker).touch()
    for child in children:
        if child.poll() is None:
            child.terminate()
            try:
                child.communicate(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill(); child.communicate(timeout=5)
    if root.exists():
        shutil.rmtree(root)
