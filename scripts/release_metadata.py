#!/usr/bin/env python3
"""Create manager-compatible update.json from a verified, versioned module ZIP."""
import hashlib
import json
from pathlib import Path
import re
import sys
import zipfile

REPO = 'https://github.com/fengwenhua/AD_Killer_By_NFQUEUE'


def generate(package: Path, notes: Path):
    with zipfile.ZipFile(package) as archive:
        props = dict(line.split('=', 1) for line in archive.read('module.prop').decode().splitlines() if '=' in line)
    version = props['version'].removeprefix('v')
    if props.get('releaseChannel', 'stable') not in ('stable', 'preview'):
        raise ValueError('invalid release channel')
    if props['id'] != 'AD_Killer_By_NFQUEUE' or not re.fullmatch(r'\d{1,6}\.\d{1,6}(?:\.\d{1,6})?', version):
        raise ValueError('invalid module identity or version')
    name = f'AD_Killer_By_NFQUEUE-v{version}.zip'
    if package.name != name or not props['versionCode'].isdigit() or int(props['versionCode']) <= 0:
        raise ValueError('package name/versionCode mismatch')
    digest = hashlib.sha256(package.read_bytes()).hexdigest()
    if package.with_name(name+'.sha256').read_text() != f'{digest}  {name}\n':
        raise ValueError('package checksum mismatch')
    url = f'{REPO}/releases/download/v{version}/'
    metadata = {'version': f'v{version}', 'versionCode': int(props['versionCode']),
                'releaseChannel': props.get('releaseChannel', 'stable'),
                'zipUrl': url+name, 'changelog': url+'changelog.md',
                'sha256': digest, 'module': props['id'],
                'notes': notes.read_text(encoding='utf-8')[:8000]}
    (package.parent/'changelog.md').write_text(notes.read_text(encoding='utf-8'), encoding='utf-8')
    (package.parent/'update.json').write_text(json.dumps(metadata, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    return metadata


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit('usage: release_metadata.py PACKAGE_ZIP CHANGELOG_MD')
    generate(Path(sys.argv[1]), Path(sys.argv[2]))
