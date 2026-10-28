#!/usr/bin/env python3
"""Bounded smoke/extended fuzz runner, persistent corpora and failure artifacts."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import time

from seeds import TARGETS, seed_inputs

ROOT = Path(__file__).resolve().parents[2]
BASE = ['-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
        '-fno-omit-frame-pointer', '-fno-sanitize-recover=all', '-pthread']
MAX_CORPUS_BYTES = 8 * 1024 * 1024
MAX_CORPUS_FILES = 512


def number(name, default, minimum, maximum):
    raw = os.environ.get(name, str(default))
    if not raw.isascii() or not raw.isdecimal() or not minimum <= int(raw) <= maximum:
        raise ValueError(f'{name} must be {minimum}..{maximum}')
    return int(raw)


def corpus_files(path, max_len, protected=()):
    if path.is_symlink() or not path.is_dir():
        raise ValueError('corpus must be a real directory')
    protected = set(protected)
    files = []
    for entry in path.iterdir():
        info = entry.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
            raise ValueError('corpus entry must be a regular unlinked file')
        if info.st_size > max_len:
            if entry.name in protected: raise ValueError('committed seed exceeds input bound')
            entry.unlink()
            continue
        files.append((entry, info.st_size))
        if len(files) > 8192:
            raise ValueError('corpus inventory limit exceeded')
    required = [(entry, size) for entry, size in files if entry.name in protected]
    if (len(required) != len(protected) or len(required) > MAX_CORPUS_FILES or
            sum(size for _, size in required) > MAX_CORPUS_BYTES):
        raise ValueError('committed seeds exceed corpus quota or are missing')
    # Keep every current committed seed/regression, then small discovered cases.
    files.sort(key=lambda item: (item[0].name not in protected, item[1], item[0].name))
    total, kept = 0, []
    for entry, size in files:
        if len(kept) >= MAX_CORPUS_FILES or total + size > MAX_CORPUS_BYTES:
            entry.unlink()
        else:
            total += size
            kept.append(entry)
    return kept


def save_report(path, report):
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(report, indent=2) + '\n')
    temporary.replace(path)


def main():
    runs = number('FUZZ_RUNS', 500, 1, 100000000)
    seconds = number('FUZZ_SECONDS', 0, 0, 3600)
    max_len = number('FUZZ_MAX_LEN', 131072, 4096, 131072)
    compiler = os.environ.get('FUZZ_CXX', 'clang++')
    if not shutil.which(compiler):
        raise ValueError('missing fuzz compiler: ' + compiler)
    output = Path(os.environ.get('FUZZ_OUTPUT', str(ROOT / 'dist/fuzz'))).absolute()
    if output.is_symlink():
        raise ValueError('fuzz output cannot be a symlink')
    output.mkdir(parents=True, exist_ok=True)
    report = {'schema': 1, 'mode': 'probing', 'requested_seconds_per_target': seconds,
              'requested_runs_per_target': None if seconds else runs, 'max_len': max_len, 'targets': {}, 'complete': False}
    report_path = output / 'report.json'
    for section in ('corpus', 'logs', 'artifacts'):
        if (output / section).is_symlink():
            raise ValueError('fuzz section cannot be a symlink')
        (output / section).mkdir(exist_ok=True)
    save_report(report_path, report)
    build = Path(tempfile.mkdtemp(prefix='adk-fuzz-build-'))
    try:
        version = subprocess.run([compiler, '--version'], capture_output=True, text=True, check=True, timeout=10)
        report['compiler'] = version.stdout.splitlines()[0]
        probe = build / 'probe.cpp'
        probe.write_text('#include <cstddef>\n#include <cstdint>\nextern "C" int LLVMFuzzerTestOneInput(const uint8_t*, size_t) {return 0;}\n')
        flags = BASE + ['-fsanitize=fuzzer,address,undefined']
        probe_log = output / 'compiler-probe.log'
        with probe_log.open('wb') as log:
            compiled = subprocess.run([compiler, *flags, str(probe), '-o', str(build / 'probe')], stdout=log, stderr=log, timeout=120).returncode == 0
            runtime = compiled and subprocess.run([str(build / 'probe'), '-runs=1'], stdout=log, stderr=log, timeout=15).returncode == 0
        mode = 'libfuzzer' if runtime else 'standalone'
        report['mode'] = mode
        if not runtime and (seconds or os.environ.get('FUZZ_REQUIRE_LIBFUZZER') == '1'):
            raise ValueError('libFuzzer is required; see compiler-probe.log (no standalone success substitution)')
        driver = [] if runtime else [str(ROOT / 'tests/fuzz/fuzz_standalone_main.cpp')]
        if not runtime:
            flags = BASE + ['-fsanitize=address,undefined']
            print('libFuzzer unavailable: deterministic ASan/UBSan seed replay and random smoke only', flush=True)
        seeds = seed_inputs(ROOT / 'tests/fuzz')
        failures = []
        for target, sources in TARGETS.items():
            started = time.monotonic()
            item = {'status': 'building', 'mode': mode}
            report['targets'][target] = item
            save_report(report_path, report)
            executable = build / ('fuzz_' + target)
            log_dir, corpus, artifacts = (output / name / target for name in ('logs', 'corpus', 'artifacts'))
            for directory in (log_dir, corpus, artifacts):
                if directory.is_symlink(): raise ValueError('fuzz directory cannot be a symlink')
                directory.mkdir(parents=True, exist_ok=True)
            log_path = log_dir / 'run.log'
            with log_path.open('wb') as log:
                compile_result = subprocess.run([compiler, *flags, '-I' + str(ROOT / 'src'),
                    str(ROOT / 'tests/fuzz' / ('fuzz_' + target + '.cpp')), *driver,
                    *(str(ROOT / 'src' / source) for source in sources), '-lz', '-o', str(executable)],
                    stdout=log, stderr=log, timeout=180)
                if compile_result.returncode:
                    item.update(status='compile_failed', exit_code=compile_result.returncode)
                    failures.append(target)
                    item['elapsed_seconds'] = round(time.monotonic() - started, 3)
                    log.flush()
                    save_report(report_path, report)
                    print(log_path.read_text(errors='replace')[-8000:], file=sys.stderr)
                    continue
                corpus_files(corpus, max_len)
                all_seeds = dict(seeds[target])
                regressions = ROOT / 'tests/fuzz/regressions' / target
                if regressions.exists():
                    for file in regressions.iterdir():
                        if file.is_symlink() or not file.is_file() or file.stat().st_size > max_len:
                            raise ValueError('invalid committed regression fixture')
                        all_seeds['regression-' + file.name] = file.read_bytes()
                protected = set()
                for body in all_seeds.values():
                    name = 'seed-' + hashlib.sha256(body).hexdigest()
                    protected.add(name)
                    destination = corpus / name
                    if destination.is_symlink(): raise ValueError('corpus seed cannot be a symlink')
                    destination.write_bytes(body)
                files = corpus_files(corpus, max_len, protected)
                item.update(seed_count=len(all_seeds), regression_count=sum(k.startswith('regression-') for k in all_seeds))
                if runtime:
                    command = [str(executable), str(corpus), '-max_len=' + str(max_len), '-timeout=10',
                               '-rss_limit_mb=1024', '-malloc_limit_mb=512', '-print_final_stats=1',
                               '-artifact_prefix=' + str(artifacts) + '/',
                               '-max_total_time=' + str(seconds) if seconds else '-runs=' + str(runs)]
                else:
                    command = [str(executable), str(runs), str(max_len), *map(str, files)]
                print(f'[fuzz] {target}: {mode}, seeds={len(files)}, seconds={seconds}, runs={None if seconds else runs}', flush=True)
                try:
                    result = subprocess.run(command, stdout=log, stderr=log, timeout=seconds + 45 if seconds else 180)
                    item.update(status='passed' if result.returncode == 0 else 'failed', exit_code=result.returncode)
                except subprocess.TimeoutExpired:
                    item.update(status='runner_timeout', exit_code=None)
                if item['status'] != 'passed': failures.append(target)
                item['retained_inputs'] = len(corpus_files(corpus, max_len, protected))
                item['elapsed_seconds'] = round(time.monotonic() - started, 3)
                log.flush()
            save_report(report_path, report)
            if item['status'] != 'passed':
                print(log_path.read_text(errors='replace')[-8000:], file=sys.stderr)
        report['failures'] = failures
        report['complete'] = not failures
        if failures: return 1
        print('All fuzz targets passed; report: ' + str(report_path), flush=True)
        return 0
    finally:
        save_report(report_path, report)
        if os.environ.get('KEEP_TEST_BUILD') == '1': print('fuzz build kept at: ' + str(build), flush=True)
        else: shutil.rmtree(build)


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        raise SystemExit('fuzz infrastructure failed: ' + str(error))
