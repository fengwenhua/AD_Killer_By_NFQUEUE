#!/usr/bin/env python3
"""Real HTTP/cache/retry/progress contract. Disposable Linux/root only."""
from collections import Counter
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import gzip
import json
import os
import shutil
import subprocess
import sys
import threading
import time

DATA = Path('/data/adb/AD_Killer_By_NFQUEUE')
BINARY = sys.argv[1]

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        with self.server.lock:
            self.server.requests[self.path] += 1
            number = self.server.requests[self.path]
            mode = self.server.modes.get(self.path, 'ok')
        time.sleep(.07)
        if mode == 'bad' or (mode == 'recover' and number <= 2):
            self.send_response(404); self.end_headers(); return
        tag = '"' + self.path.removeprefix('/') + '-v1"'
        if self.headers.get('If-None-Match') == tag:
            self.send_response(304); self.send_header('ETag', tag); self.end_headers(); return
        body = f'0.0.0.0 {self.path.removeprefix("/")}.example.org\n'.encode()
        self.send_response(200); self.send_header('ETag', tag); self.send_header('Content-Length', str(len(body)))
        self.end_headers(); self.wfile.write(body)

def command(*args, text=None, ok=True):
    p = subprocess.run([BINARY, *args], input=text, text=True, capture_output=True, timeout=25)
    assert (p.returncode == 0) == ok, (args, p.stdout, p.stderr)
    return json.loads(p.stdout.splitlines()[-1]) if p.stdout.splitlines() and p.stdout.splitlines()[-1].startswith('{') else p.stdout

def report(value):
    head, *sections = value.split('\n[source_')
    parse = lambda block: dict(line.split('=', 1) for line in block.splitlines() if '=' in line)
    return parse(head), [parse(s) for s in sections]

def main():
    assert os.geteuid() == 0 and not DATA.exists(), 'disposable Linux/root only'
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.lock = threading.Lock(); server.requests = Counter(); server.modes = {'/bad': 'bad', '/recover': 'recover', '/never': 'bad'}
    thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
    prefix = f'http://127.0.0.1:{server.server_port}'
    def configure(*paths):
        command('--sources-save', text=''.join(prefix+p+'\n' for p in paths)+'# disabled: builtin:hagezi-pro-minus-fake\n')
    def reset_requests():
        with server.lock: server.requests.clear()
    def metadata(path):
        return next(p for p in (DATA/'source_cache').glob('*/metadata') if dict(line.split('=',1) for line in p.read_text().splitlines() if '=' in line).get('url_0') == (prefix+path).encode().hex())
    def age(path, seconds):
        p = metadata(path); stamp = str(int(time.time())-seconds)
        p.write_text(''.join(line.split('=',1)[0]+'='+stamp+'\n' if line.startswith(('verified_at=', 'fetched_')) else line for line in p.read_text().splitlines(keepends=True)))
    def sync(expected):
        value = command('--update-start'); assert value['status']=='started'
        job = value['job_id']; observed = set(); last = None
        until = time.monotonic()+20
        while time.monotonic() < until:
            state = command('--update-status', job)
            if state.get('progress'):
                head, rows = report(state['progress'])
                assert head['job_id']==job and int(head['source_count'])==len(rows)
                assert len(rows) <= 65 and len({r['url'] for r in rows})==len(rows)
                observed.update(r['status'] for r in rows)
                last = (head, rows)
            if state['status']!='running':
                assert state['status']==expected, state
                assert last is not None and (DATA/'update_jobs'/f'update-{job}.progress').exists()
                return last, observed
            time.sleep(.025)
        raise AssertionError('sync did not finish')
    try:
        command('--install')
        settings = command('--sync-settings')
        assert (settings['concurrency'],settings['interval_minutes'],settings['retries'])==(10,30,2)
        conf = DATA/'state.conf'; saved = conf.read_bytes()
        for arguments in [('0','30','2'),('21','30','2'),('10','-1','2'),('10','1441','2'),('10','30','-1'),('10','30','6'),('10','1.5','2')]:
            command('--sync-options',*arguments,ok=False); assert conf.read_bytes()==saved
        old_config = ''.join(line for line in conf.read_text().splitlines(keepends=True) if not line.startswith(('sync_interval_minutes=', 'sync_retries=')))
        conf.write_text(old_config); before = conf.read_bytes(); settings = command('--sync-settings')
        assert settings['interval_minutes']==30 and settings['retries']==2 and conf.read_bytes()==before
        command('--sync-options','3','30','2'); configure('/good','/recover','/bad')
        (head, rows), seen = sync('failed')
        assert not (DATA/'rules.current').exists(), 'failed complete set published a subset'
        assert server.requests==Counter({'/good':1,'/recover':3,'/bad':3}), server.requests
        assert head['publish_status']=='incomplete_sources' and int(head['accepted_sources'])==2
        assert [r['attempts'] for r in rows]==['1','3','3']
        assert any(x in seen for x in ('queued','downloading','validating')), 'no live per-source progress'
        for path in ['/good','/recover']: age(path,300)
        originals = {p: metadata(p).read_bytes() for p in ['/good','/recover']}
        server.modes['/bad']='ok'; reset_requests()
        (head, rows), _ = sync('success')
        assert server.requests==Counter({'/bad':1}), server.requests
        assert [r['provenance'] for r in rows]==['cache_fresh','cache_fresh','downloaded']
        assert all(metadata(path).read_bytes()==content for path,content in originals.items()), 'cache reuse extended freshness timestamp'
        reset_requests(); original_generation = (DATA/'rules.current').read_bytes()
        (head, rows), _ = sync('success')
        assert not server.requests and all(r['provenance']=='cache_fresh' for r in rows)
        assert (DATA/'rules.current').read_bytes()==original_generation
        # Expired success caches revalidate. Download failures retry exactly twice,
        # then may use the previously verified complete source as bounded fallback.
        server.modes['/recover']='ok'; server.modes['/bad']='bad'
        for path in ['/good','/recover','/bad']: age(path,1802)
        reset_requests(); (head, rows), _ = sync('success')
        assert server.requests==Counter({'/good':1,'/recover':1,'/bad':3}), server.requests
        assert rows[2]['provenance']=='cache_fallback' and rows[2]['attempts']=='3'
        # Increasing the freshness window must not suppress retries for the
        # source whose previous attempt failed and used fallback.
        command('--sync-options','3','60','2'); reset_requests(); sync('success')
        assert server.requests==Counter({'/bad':3}), server.requests
        server.modes['/bad']='ok'; reset_requests(); sync('success')
        assert server.requests==Counter({'/bad':1}), server.requests
        # Zero disables retries; a failed new source never replaces the last
        # complete ruleset, and successful fresh caches remain network-free.
        command('--sync-options','3','30','0'); configure('/good','/recover','/never')
        original_generation = (DATA/'rules.current').read_bytes(); reset_requests()
        (head, rows), _ = sync('failed')
        assert server.requests==Counter({'/never':1}) and rows[2]['attempts']=='1'
        assert (DATA/'rules.current').read_bytes()==original_generation
        # Integrity checks remain mandatory even for cache hits inside the TTL.
        configure('/good'); raw = metadata('/good').parent/'body-0'; raw.write_bytes(b'x'*raw.stat().st_size)
        reset_requests(); sync('success'); assert server.requests==Counter({'/good':1})
        generation = DATA/'rulesets'/(DATA/'rules.current').read_text().strip()
        assert b'good.example.org' in gzip.decompress((generation/'hosts_rules.gz').read_bytes())
        assert not (DATA/'daemon.pid').exists(), 'settings/sync started a daemon'
        assert not (DATA/'source_staging').exists(), 'source staging leaked'
        print('sync options: defaults/atomic validation, independent 2 retries, live job-bound progress, all-source evaluation, 30min cache reuse, unchanged timestamps, 304, fallback retry, zero retry and integrity passed')
    finally:
        server.shutdown(); server.server_close(); thread.join()
        if DATA.exists(): shutil.rmtree(DATA)

if __name__=='__main__': main()
