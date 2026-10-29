#!/usr/bin/env python3
"""Audit both independently built release channels before uploading artifacts."""
import hashlib
from decimal import Decimal
import json
from pathlib import Path
import re
import sys
import zipfile


def embedded_version_code(html):
    # Minifiers may emit 13000 as 13e3. Compare the exact integer value, while
    # still rejecting missing/ambiguous metadata and arbitrary expressions.
    values = re.findall(r'\bversionCode\s*:\s*(\d+(?:\.\d+)?(?:[eE][+-]?\d+)?)(?=\s*[,}])', html)
    assert len(values) == 1, 'missing or ambiguous embedded versionCode'
    value = Decimal(values[0])
    assert value.is_finite() and 1 <= value <= 2**31 - 1 and value == value.to_integral_value(), 'invalid embedded versionCode'
    return int(value)


def main():
    root = Path(sys.argv[1])
    stable = json.loads((root / 'update.json').read_text())
    preview = json.loads((root / 'preview/update.json').read_text())
    assert stable['version'] == preview['version']
    assert stable['versionCode'] == preview['versionCode'] + 1
    assert stable['sha256'] != preview['sha256']
    for directory, metadata, channel in ((root, stable, 'stable'), (root / 'preview', preview, 'preview')):
        assert metadata['releaseChannel'] == channel
        package = directory / f"AD_Killer_By_NFQUEUE-{metadata['version']}.zip"
        assert hashlib.sha256(package.read_bytes()).hexdigest() == metadata['sha256']
        with zipfile.ZipFile(package) as archive:
            props = dict(line.split('=', 1) for line in archive.read('module.prop').decode().splitlines() if '=' in line)
            assert props['releaseChannel'] == channel
            assert int(props['versionCode']) == metadata['versionCode']
            assert props['version'] == metadata['version']
            html = archive.read('webroot/index.html').decode()
            assert embedded_version_code(html) == metadata['versionCode']
            assert re.search(rf'releaseChannel:([\x22\x27`]){channel}\1', html)
            assert channel.encode() in archive.read('AD_Killer_By_NFQUEUE')
    print('Stable/preview package identity, UI, versionCode and metadata separation passed')


if __name__ == '__main__':
    main()
