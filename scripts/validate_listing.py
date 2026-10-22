#!/usr/bin/env python3
"""Offline project checks; module.yaml uses the JSON subset of YAML 1.2.

The full upstream schema/ZIP check is performed by the SHA-pinned KernelSU
action in CI. No external Python dependencies are needed for local tests.
"""
import fnmatch
import json
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate key: ' + key)
        result[key] = value
    return result


def properties(text):
    result = {}
    for line in text.splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        key, separator, value = line.partition('=')
        if not separator or key in result:
            raise ValueError('invalid or duplicate module property: ' + key)
        result[key] = value
    return result


def validate_manifest(manifest, props, root=ROOT):
    if type(manifest) is not dict or type(manifest.get('schemaVersion')) is not int or manifest['schemaVersion'] != 1:
        raise ValueError('unsupported manifest schema')
    for key in ('id', 'name', 'summary', 'description'):
        if not isinstance(manifest.get(key), str) or not manifest[key].strip():
            raise ValueError('missing manifest ' + key)
    identity = manifest['id']
    if not re.fullmatch(r'[a-zA-Z][a-zA-Z0-9._-]{1,63}', identity) or identity != props.get('id'):
        raise ValueError('manifest/module.prop identity mismatch')
    lowered = identity.lower()
    if lowered in {'ksu', 'kernelsu', 'magisk', 'apatch', 'test', 'example', 'module'} or lowered.startswith(('org.kernelsu.', 'me.weishu.')) or lowered.endswith(('.json', '.html', '.htm', '.md', '.xml', '.txt')):
        raise ValueError('reserved module identity')
    if len(manifest['name']) > 100 or len(manifest['summary']) > 500:
        raise ValueError('manifest display text too long')
    document = Path(manifest['description'])
    if document.is_absolute() or '..' in document.parts or '\\' in str(document):
        raise ValueError('unsafe description path')
    resolved = (root / document).resolve()
    if not resolved.is_relative_to(root.resolve()) or not resolved.is_file():
        raise ValueError('description missing or outside repository')
    if manifest.get('license') != 'MIT' or 'MIT License' not in (root / 'LICENSE').read_text():
        raise ValueError('project license mismatch')
    if manifest.get('compatibility') != {'androidMinApi': 31, 'architectures': ['arm64-v8a']}:
        raise ValueError('compatibility must match the Android build target')
    if manifest.get('kind', 'module') != 'module' or manifest.get('metamodule', False):
        raise ValueError('this is a normal module, not a script/metamodule')
    pattern = manifest.get('release', {}).get('assetPattern', '')
    if not isinstance(pattern, str) or not 1 <= len(pattern) <= 128 or '/' in pattern or '\\' in pattern:
        raise ValueError('invalid release asset pattern')
    if not fnmatch.fnmatchcase('AD_Killer_By_NFQUEUE-' + props['version'] + '.zip', pattern):
        raise ValueError('asset pattern does not match our versioned ZIP')
    for key in ('id', 'name', 'version', 'versionCode', 'author', 'description'):
        if not props.get(key):
            raise ValueError('missing module property ' + key)
    code = props['versionCode']
    if not re.fullmatch(r'[0-9]{1,10}', code) or not 1 <= int(code) <= 2**31 - 1:
        raise ValueError('invalid module versionCode')
    return manifest


def validate(root=ROOT):
    manifest = json.loads((root / 'module.yaml').read_text(), object_pairs_hook=unique_object)
    return validate_manifest(manifest, properties((root / 'module/module.prop').read_text()), root)


if __name__ == '__main__':
    try:
        validate()
    except (ValueError, KeyError, OSError) as error:
        raise SystemExit('listing validation failed: ' + str(error))
    print('KernelSU listing identity, metadata, description, license and compatibility passed')
