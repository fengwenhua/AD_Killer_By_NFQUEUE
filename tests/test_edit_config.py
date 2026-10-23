#!/usr/bin/env python3
"""Actual bounded stdin/JSON protocol; disposable files only, no root/network."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix='adk-edit-protocol-') as temporary:
    root = Path(temporary)
    def call(operation, target, text='', revision=None):
        args = [sys.argv[1], temporary, operation, target]
        if revision is not None:
            args.append(revision)
        result = subprocess.run(args, input=text, text=True, capture_output=True, timeout=10)
        body = json.loads(result.stdout)
        assert (result.returncode == 0) == (body['status'] == 'ok'), result
        return body
    assert call('read', 'custom')['status'] == 'missing'
    first = call('save', 'custom', '# 注释 😀\nads.example.com\n', 'missing')
    assert first['status'] == 'ok'
    read = call('read', 'custom')
    assert read['content'] == '# 注释 😀\nads.example.com\n'
    assert read['revision'] == hashlib.sha256(read['content'].encode()).hexdigest()
    assert call('save', 'custom', '', 'missing')['status'] == 'conflict'
    assert call('save', '../../outside', '', 'missing')['status'] == 'invalid_target'
    assert call('save', 'custom', 'x' * (1024 * 1024 + 1), read['revision'])['status'] == 'invalid_content'
    assert call('read', 'custom')['content'] == read['content']
    (root / 'custom_rules.txt').write_bytes(b'\xff')
    assert call('read', 'custom')['status'] == 'encoding_error'
print('editable config bounded stdin, JSON, revisions and failed-read protocol passed')
