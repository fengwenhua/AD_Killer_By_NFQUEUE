"""Durable local release journal and Actions summary; never stores credentials."""
import hashlib
import json
import os
from pathlib import Path
import tempfile
import sys
import re
from validate_listing import unique_object


def fingerprint(manifest):
    identity = {key: manifest[key] for key in (
        'run_id', 'source', 'version', 'channel', 'artifact_id', 'artifact_digest', 'assets', 'build_context')}
    return hashlib.sha256(json.dumps(identity, sort_keys=True, separators=(',', ':')).encode()).hexdigest()


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent, delete=False) as stream:
            temporary = Path(stream.name)
            json.dump(value, stream, ensure_ascii=False, indent=2)
            stream.write('\n'); stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary, path)
        temporary = None
        descriptor = os.open(path.parent, os.O_RDONLY)
        try: os.fsync(descriptor)
        finally: os.close(descriptor)
    finally:
        if temporary is not None: temporary.unlink(missing_ok=True)


class Receipt:
    def __init__(self, directory, manifest, operation):
        self.path = Path(directory) / 'release-state.json'
        self.previous_phase = None
        previous = None
        if self.path.exists() or self.path.is_symlink():
            if self.path.is_symlink() or not self.path.is_file() or self.path.stat().st_size > 65536:
                raise ValueError('release receipt is not a bounded regular file')
            previous = json.loads(self.path.read_text(), object_pairs_hook=unique_object)
            if (type(previous) is not dict or previous.get('schema') != 1 or
                    not isinstance(previous.get('phase'), str) or not re.fullmatch('[a-z_]+', previous['phase']) or
                    type(previous.get('events')) is not list or len(previous['events']) > 64 or
                    any(not isinstance(event,str) or not re.fullmatch('[a-z_]+',event) for event in previous['events'])):
                raise ValueError('invalid release receipt')
            if previous.get('manifest_sha256') != fingerprint(manifest):
                raise ValueError('receipt belongs to a different CI artifact; use a fresh preparation directory')
            self.previous_phase = previous.get('phase')
        self.value = {'schema': 1, 'operation': operation, 'manifest_sha256': fingerprint(manifest),
                      'ci_run_id': manifest['run_id'], 'source': manifest['source'],
                      'version': manifest['version'], 'channel': manifest['channel'],
                      'artifact_id': manifest['artifact_id'], 'artifact_digest': manifest['artifact_digest'],
                      'build_context': manifest['build_context'],
                      'assets': manifest['assets'], 'phase': 'validated', 'events': []}
        if previous:
            self.value['phase'] = self.previous_phase
            self.value['events'] = previous.get('events', [])[-64:]
            if type(previous.get('release_id')) is int: self.value['release_id'] = previous['release_id']

    def write(self, phase, **facts):
        self.value.update(facts, phase=phase)
        events = self.value['events']
        events.append(phase)
        self.value['events'] = events[-64:]
        atomic_json(self.path, self.value)

    def summary(self):
        try: self._summary()
        except OSError:
            print('release summary unavailable; inspect release-state.json before any retry', file=sys.stderr)

    def _summary(self):
        destination = os.environ.get('GITHUB_STEP_SUMMARY')
        if not destination: return
        # All values below are validated identities or fixed program phases,
        # never arbitrary API text, signed URLs, tokens or exception reprs.
        rows = [('Outcome', self.value['phase']), ('Version', self.value['version']),
                ('Channel', self.value['channel']), ('CI run', self.value['ci_run_id']),
                ('CI source', self.value['source']), ('Artifact ID', self.value['artifact_id']),
                ('Artifact digest', self.value['artifact_digest']),
                ('Manifest confirmation', self.value['manifest_sha256'])]
        if 'release_id' in self.value: rows.append(('Release ID', self.value['release_id']))
        if 'latest' in self.value: rows.append(('Latest', self.value['latest']))
        if 'immutable' in self.value: rows.append(('Published immutable', self.value['immutable']))
        if 'immutability' in self.value: rows.append(('Immutability setting read', self.value['immutability']['status']))
        context = self.value['build_context']
        rows.append(('NDK declared by CI recipe', ', '.join(context['declared_ndk_versions']) or 'not declared'))
        rows.append(('Android API target declared by recipe', ', '.join(context['declared_android_targets']) or 'not declared'))
        with open(destination, 'a', encoding='utf-8') as stream:
            stream.write('\n### Release verification\n\n| Field | Value |\n| --- | --- |\n')
            for key, value in rows: stream.write(f'| {key} | `{value}` |\n')
            stream.write('\nAsset SHA-256:\n\n')
            for name, digest in sorted(self.value['assets'].items()): stream.write(f'- `{name}`: `{digest}`\n')
            stream.write('\nBuild recipe and frontend lock SHA-256 (source declarations, not runtime attestations):\n\n')
            for name, digest in context['source_sha256'].items(): stream.write(f'- `{name}`: `{digest}`\n')
            stream.write('\nThe report artifact contains the receipt. An interrupted request must be inspected before recovery; do not repeat publication blindly.\n')
