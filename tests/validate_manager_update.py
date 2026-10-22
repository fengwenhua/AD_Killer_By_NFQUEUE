#!/usr/bin/env python3
"""Audit the manager update contract against the actual package, offline."""
import hashlib
import json
from pathlib import Path
import re
import sys
import zipfile

REPO = 'https://github.com/fengwenhua/AD_Killer_By_NFQUEUE'
UPDATE_URL = REPO + '/releases/latest/download/update.json'
INT32_MAX = 2**31 - 1


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError('duplicate update JSON key: ' + key)
        value[key] = item
    return value


def validate_manager_update(package):
    package = Path(package)
    with zipfile.ZipFile(package) as archive:
        props = {}
        for line in archive.read('module.prop').decode('utf-8').splitlines():
            if '=' not in line or line.startswith('#'):
                continue
            key, value = line.split('=', 1)
            if key in props:
                raise ValueError('duplicate module property: ' + key)
            props[key] = value
    code = props.get('versionCode', '')
    version = props.get('version', '')
    if props.get('id') != 'AD_Killer_By_NFQUEUE' or not re.fullmatch(r'v[0-9]{1,6}\.[0-9]{1,6}(?:\.[0-9]{1,6})?', version):
        raise ValueError('invalid module identity/version')
    if not re.fullmatch(r'[0-9]{1,10}', code) or not 1 <= int(code) <= INT32_MAX:
        raise ValueError('versionCode must be a positive signed int32 for root managers')
    if props.get('updateJson') != UPDATE_URL:
        raise ValueError('installed package must declare the canonical stable updateJson URL')
    channel = props.get('releaseChannel')
    if channel not in ('stable', 'preview'):
        raise ValueError('package releaseChannel must be explicit')
    name = f'AD_Killer_By_NFQUEUE-{version}.zip'
    if package.name != name:
        raise ValueError('package filename/version mismatch')
    metadata = json.loads((package.parent / 'update.json').read_text(encoding='utf-8'), object_pairs_hook=unique_object)
    if not isinstance(metadata, dict):
        raise ValueError('update JSON must be an object')
    # These four fields are consumed directly by KernelSU/Magisk/APatch.
    if metadata.get('version') != version or type(metadata.get('versionCode')) is not int or metadata['versionCode'] != int(code):
        raise ValueError('manager version/versionCode does not match the package')
    prefix = REPO + '/releases/download/' + version + '/'
    if metadata.get('zipUrl') != prefix + name or metadata.get('changelog') != prefix + 'changelog.md':
        raise ValueError('manager download/changelog must use exact versioned release assets')
    if metadata.get('module') != props['id'] or metadata.get('releaseChannel') != channel:
        raise ValueError('WebUI and manager release identity/channel differ')
    digest = hashlib.sha256(package.read_bytes()).hexdigest()
    if metadata.get('sha256') != digest:
        raise ValueError('update JSON checksum does not match the actual package')
    if (package.parent / (name + '.sha256')).read_text(encoding='ascii') != f'{digest}  {name}\n':
        raise ValueError('sidecar checksum does not match the actual package')
    notes = (package.parent / 'changelog.md').read_text(encoding='utf-8')
    if not notes.strip() or metadata.get('notes') != notes[:8000]:
        raise ValueError('missing or inconsistent manager changelog')
    return metadata


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: validate_manager_update.py PACKAGE_ZIP')
    try:
        result = validate_manager_update(sys.argv[1])
    except (ValueError, KeyError, OSError, zipfile.BadZipFile) as error:
        raise SystemExit('manager update validation failed: ' + str(error))
    print(f"KernelSU/Magisk/APatch update contract passed: {result['version']} / {result['versionCode']} / {result['releaseChannel']}")
