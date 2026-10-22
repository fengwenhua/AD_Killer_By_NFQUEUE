#!/usr/bin/env python3
"""Offline listing/release safety tests. All GitHub responses are synthetic."""
import copy
from contextlib import redirect_stdout
import hashlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import publish_release as release
from release_metadata import generate
from package_module import write_checksum
from validate_listing import properties, validate, validate_manifest, unique_object


class FakeGitHub:
    def __init__(self):
        self.source = 'a' * 40
        self.run = {'head_repository': {'full_name': release.REPOSITORY},
                    'path': '.github/workflows/ci.yml', 'head_branch': 'main', 'event': 'push',
                    'head_sha': self.source, 'status': 'completed', 'conclusion': 'success'}
        self.jobs = [{'name': name, 'status': 'completed', 'conclusion': 'success'} for name in release.REQUIRED_JOBS]
        self.calls, self.assets, self.releases = [], [], []
        self.occupied = self.tag_exists = self.upload_fails = False
        self.immutable = True
        self.artifact_body = b''

    def request(self, path, data=None, method='GET'):
        self.calls.append((method, path, data))
        if path.startswith('/actions/runs/'):
            return copy.deepcopy(self.run)
        if path.startswith('/releases/tags/'):
            return {'id': 9} if self.occupied else None
        if path.startswith('/git/ref/'):
            return {'object': {'type': 'commit', 'sha': self.source}} if self.tag_exists else None
        if path == '/releases' and method == 'POST':
            return {'id': 10, 'draft': True, 'tag_name': data['tag_name']}
        if path == '/releases/10' and method == 'GET':
            return {'assets': self.assets}
        if path == '/releases/10' and method == 'PATCH':
            self.tag_exists = True
            return {'id': 10, 'draft': False, 'prerelease': data['prerelease'],
                    'immutable': self.immutable, 'html_url': release.REPO + '/releases/tag/v1.2.6'}
        if path == '/releases/latest':
            return {'id': 10}
        raise AssertionError((method, path))

    def pages(self, path, key=None):
        if '/jobs?' in path:
            return iter(copy.deepcopy(self.jobs))
        if path.endswith('/artifacts'):
            return iter([{'id': 8, 'name': 'AD_Killer_By_NFQUEUE-module', 'expired': False,
                          'digest': 'sha256:' + hashlib.sha256(self.artifact_body).hexdigest()}])
        if path == '/releases':
            return iter(self.releases)
        raise AssertionError(path)

    def artifact(self, artifact_id):
        assert artifact_id == 8
        return self.artifact_body

    def source_file(self, source, path):
        assert source == self.source
        if path == 'module/module.prop':
            return self.props.encode()
        if path == 'module.yaml':
            return (ROOT / 'module.yaml').read_bytes()
        if path == 'CHANGELOG.md':
            return b'# v1.2.6\n\nRelease tests.\n'
        raise AssertionError(path)

    def public_download(self, url):
        return json.dumps({'versionCode': self.prior_code}).encode()

    def upload(self, release_id, name, body):
        assert release_id == 10
        if self.upload_fails:
            raise OSError('synthetic interrupted upload')
        self.calls.append(('UPLOAD', name, None))
        asset = {'name': name, 'digest': 'sha256:' + hashlib.sha256(body).hexdigest(), 'size': len(body)}
        self.assets.append(asset)
        return asset


class ListingTests(unittest.TestCase):
    def test_real_manifest(self):
        self.assertEqual(validate()['id'], 'AD_Killer_By_NFQUEUE')

    def test_bad_manifest(self):
        original = json.loads((ROOT / 'module.yaml').read_text())
        props = properties((ROOT / 'module/module.prop').read_text())
        for key, value in [('schemaVersion', True), ('id', 'other'), ('id', 'kernelsu'),
                           ('description', '../README.md'), ('description', '/etc/passwd'),
                           ('description', 'missing.md'), ('license', 'GPL-3.0'),
                           ('compatibility', {'androidMinApi': 21}), ('metamodule', True),
                           ('release', {'assetPattern': '*.apk'}), ('summary', '')]:
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                validate_manifest({**original, key: value}, props)
        with self.assertRaises(ValueError):
            properties('id=a\nid=b\n')
        with self.assertRaises(ValueError):
            json.loads('{"id":"a","id":"b"}', object_pairs_hook=unique_object)


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='adk-publication-test-')
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.github = FakeGitHub()
        self.package = self.directory / 'AD_Killer_By_NFQUEUE-v1.2.6.zip'
        self.props = (ROOT / 'module/module.prop').read_text().replace('v1.2.5', 'v1.2.6').replace('12501', '12601')
        # Build fixtures independently of the project's current version.
        self.props = '\n'.join(line for line in self.props.splitlines() if not line.startswith(('version=', 'versionCode='))) + '\nversion=v1.2.6\nversionCode=12601\n'
        self.github.props = self.props
        with zipfile.ZipFile(self.package, 'w') as archive:
            archive.writestr('module.prop', self.props)
        write_checksum(self.package)
        notes = self.directory / 'source-notes.md'
        notes.write_bytes(self.github.source_file(self.github.source, 'CHANGELOG.md'))
        generate(self.package, notes)
        names = (self.package.name, self.package.name + '.sha256', 'update.json', 'changelog.md')
        self.manifest = {'version': 'v1.2.6', 'channel': 'stable', 'run_id': '123', 'source': self.github.source,
                         'assets': {name: hashlib.sha256((self.directory / name).read_bytes()).hexdigest() for name in names}}
        (self.directory / 'manifest.json').write_text(json.dumps(self.manifest))
        stream = io.BytesIO()
        with zipfile.ZipFile(stream, 'w') as archive:
            for name in names:
                archive.writestr(name, (self.directory / name).read_bytes())
        self.github.artifact_body = stream.getvalue()

    def publish(self):
        with redirect_stdout(io.StringIO()), patch.object(release.time, 'sleep'), patch.dict(os.environ, {'KERNELSU_IMMUTABLE_RELEASES': 'enabled'}):
            release.publish(self.github, self.directory)

    def test_order_and_immutable_result(self):
        self.publish()
        operations = [call[0] for call in self.github.calls if call[0] != 'GET']
        self.assertEqual(operations, ['POST', 'UPLOAD', 'UPLOAD', 'UPLOAD', 'UPLOAD', 'PATCH'])
        draft = next(data for method, _, data in self.github.calls if method == 'POST')
        self.assertTrue(draft['draft'])
        self.assertFalse(draft['prerelease'])
        self.assertEqual(draft['target_commitish'], self.github.source)

    def test_existing_tag_or_release_is_never_modified(self):
        for field in ('occupied', 'tag_exists'):
            with self.subTest(field=field):
                github = FakeGitHub()
                setattr(github, field, True)
                with self.assertRaises(ValueError):
                    release.check_unused(github, 'v1.2.6')
                self.assertTrue(all(method == 'GET' for method, _, _ in github.calls))

    def test_untrusted_or_incomplete_ci(self):
        for key, value in [('status', 'in_progress'), ('conclusion', 'failure'), ('head_branch', 'topic'),
                           ('event', 'pull_request'), ('path', '.github/workflows/other.yml'),
                           ('head_repository', {'full_name': 'attacker/fork'})]:
            with self.subTest(key=key):
                github = FakeGitHub()
                github.run[key] = value
                with self.assertRaises(ValueError):
                    release.check_run(github, '123')
        self.github.jobs.pop()
        with self.assertRaises(ValueError):
            release.check_run(self.github, '123')
        with self.assertRaises(ValueError):
            release.check_run(FakeGitHub(), '123; echo unsafe')
        github = FakeGitHub()
        github.jobs[0]['conclusion'] = 'skipped'
        with self.assertRaises(ValueError):
            release.check_run(github, '123')

    def test_preview_is_never_latest_or_stable(self):
        with zipfile.ZipFile(self.package, 'w') as archive:
            archive.writestr('module.prop', self.props.replace('releaseChannel=stable', 'releaseChannel=preview').replace('versionCode=12601', 'versionCode=12600'))
        write_checksum(self.package)
        generate(self.package, self.directory / 'source-notes.md')
        self.manifest['channel'] = 'preview'
        self.manifest['assets'] = {name: hashlib.sha256((self.directory / name).read_bytes()).hexdigest() for name in self.manifest['assets']}
        (self.directory / 'manifest.json').write_text(json.dumps(self.manifest))
        self.publish()
        draft = next(data for method, _, data in self.github.calls if method == 'POST')
        publication = next(data for method, _, data in self.github.calls if method == 'PATCH')
        self.assertTrue(draft['prerelease'])
        self.assertTrue(publication['prerelease'])
        self.assertEqual(publication['make_latest'], 'false')
        self.assertFalse(any(path == '/releases/latest' for _, path, _ in self.github.calls))

    def test_tampered_assets_and_wrong_channel_fail_before_any_writes(self):
        self.package.write_bytes(self.package.read_bytes() + b'tampered')
        with self.assertRaises(ValueError):
            self.publish()
        self.assertFalse(self.github.calls)
        self.manifest['channel'] = 'preview'
        with self.assertRaises(ValueError):
            release.validate_assets(self.directory, self.manifest)

    def test_missing_owner_acknowledgement(self):
        with patch.dict(os.environ, {'KERNELSU_IMMUTABLE_RELEASES': ''}), self.assertRaises(ValueError):
            release.publish(self.github, self.directory)
        self.assertFalse(self.github.calls)

    def test_interrupted_upload_never_publishes_or_deletes(self):
        self.github.upload_fails = True
        with self.assertRaises(OSError):
            self.publish()
        self.assertFalse(any(method in ('PATCH', 'DELETE') for method, _, _ in self.github.calls))

    def test_mutable_result_is_reported_as_failure(self):
        self.github.immutable = False
        with self.assertRaisesRegex(ValueError, 'NOT immutable'):
            self.publish()

    def test_monotonic_codes_include_previews(self):
        self.github.releases = [{'tag_name': 'v1.2.5', 'draft': False, 'prerelease': True,
            'assets': [{'name': 'update.json', 'browser_download_url': release.REPO + '/releases/download/v1.2.5/update.json'}]}]
        for code, accepted in ((12500, True), (12601, False), (12700, False)):
            self.github.prior_code = code
            if accepted:
                release.check_monotonic(self.github, {'versionCode': 12601})
            else:
                with self.assertRaises(ValueError):
                    release.check_monotonic(self.github, {'versionCode': 12601})

    def test_prepare_is_read_only_and_preserves_ci_bytes(self):
        destination = self.directory / 'prepared'
        with redirect_stdout(io.StringIO()), patch.object(release.subprocess, 'run'):
            # The synthetic ZIP is deliberately not an Android ELF. Real builds
            # run validate_module_zip in CI and at publication preparation.
            release.prepare(self.github, '123', 'v1.2.6', 'stable', destination)
        self.assertEqual((destination / self.package.name).read_bytes(), self.package.read_bytes())
        self.assertTrue(all(method == 'GET' for method, _, _ in self.github.calls))
        with self.assertRaises(ValueError):
            release.prepare(self.github, '123', 'v1.2.7', 'stable', self.directory / 'wrong')

    def test_artifact_digest_mismatch_fails_before_extraction(self):
        destination = self.directory / 'tampered'
        with patch.object(self.github, 'artifact', return_value=b'not-the-CI-artifact'), self.assertRaisesRegex(ValueError, 'digest mismatch'):
            release.prepare(self.github, '123', 'v1.2.6', 'stable', destination)
        self.assertFalse(destination.exists())


if __name__ == '__main__':
    unittest.main()
