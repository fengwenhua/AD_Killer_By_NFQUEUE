#!/usr/bin/env python3
"""Offline listing/release safety tests. All GitHub responses are synthetic."""
import copy
from contextlib import redirect_stdout, redirect_stderr
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
import release_receipt
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
        self.calls, self.assets, self.releases, self.asset_bytes = [], [], [], {}
        self.occupied = self.tag_exists = self.upload_fails = False
        self.immutable = True
        self.setting = {'enabled': True}
        self.artifact_body = b''
        self.artifact_name = 'AD_Killer_By_NFQUEUE-module'
        self.remote = None
        self.latest = 10

    def request(self, path, data=None, method='GET', timeout=30):
        self.calls.append((method, path, data))
        if path.startswith('/actions/runs/'):
            return copy.deepcopy(self.run)
        if path == '/immutable-releases':
            if isinstance(self.setting, Exception): raise self.setting
            return copy.deepcopy(self.setting)
        if path.startswith('/releases/tags/'):
            return {'id': 9} if self.occupied else None
        if path.startswith('/git/ref/'):
            return {'object': {'type': 'commit', 'sha': self.source}} if self.tag_exists else None
        if path == '/releases' and method == 'POST':
            self.remote = {**copy.deepcopy(data), 'id': 10, 'immutable': False, 'published_at': None,
                           'html_url': release.REPO + '/releases/tag/' + data['tag_name']}
            return copy.deepcopy(self.remote)
        if path == '/releases/10' and method == 'GET':
            return {**copy.deepcopy(self.remote), 'assets': copy.deepcopy(self.assets)} if self.remote else None
        if path == '/releases/10' and method == 'PATCH':
            self.tag_exists = True
            self.remote.update(draft=False, prerelease=data['prerelease'], immutable=self.immutable,
                               published_at='2026-10-26T00:00:00Z')
            return copy.deepcopy(self.remote)
        if path == '/releases/latest': return {'id': self.latest}
        raise AssertionError((method, path))

    def pages(self, path, key=None):
        self.calls.append(('GET', path, None))
        if '/jobs?' in path: return iter(copy.deepcopy(self.jobs))
        if path.endswith('/artifacts'):
            return iter([{'id': 8, 'name': self.artifact_name, 'expired': False,
                          'digest': 'sha256:' + hashlib.sha256(self.artifact_body).hexdigest()}])
        if path == '/releases':
            return iter(copy.deepcopy(self.releases + ([{**self.remote,'assets':self.assets}] if self.remote else [])))
        if path == '/releases/10/assets': return iter(copy.deepcopy(self.assets))
        raise AssertionError(path)

    def artifact(self, artifact_id):
        assert artifact_id == 8
        return self.artifact_body

    def asset(self, asset_id):
        self.calls.append(('GET', '/releases/assets/' + str(asset_id), None))
        return self.asset_bytes[asset_id]

    def source_file(self, source, path):
        assert source == self.source
        if path == 'module/module.prop': return self.props.encode()
        if path == 'module.yaml': return (ROOT / 'module.yaml').read_bytes()
        if path == 'CHANGELOG.md': return b'# v1.2.6\n\nRelease tests.\n'
        if path in ('.github/workflows/ci.yml','build.sh','frontend/package-lock.json'): return (ROOT/path).read_bytes()
        raise AssertionError(path)

    def public_download(self, url):
        if self.remote and url.endswith('/'+self.remote['tag_name']+'/update.json'):
            return self.asset_bytes[next(asset['id'] for asset in self.assets if asset['name']=='update.json')]
        return json.dumps({'versionCode': self.prior_code}).encode()

    def upload(self, release_id, name, body):
        assert release_id == 10
        self.calls.append(('UPLOAD', name, None))
        if self.upload_fails: raise OSError('synthetic interrupted upload')
        identity = 100 + len(self.assets)
        asset = {'id':identity,'name':name,'state':'uploaded','digest':'sha256:'+hashlib.sha256(body).hexdigest(),
                 'size':len(body),'browser_download_url':release.REPO+'/releases/download/'+self.remote['tag_name']+'/'+name}
        self.assets.append(asset); self.asset_bytes[identity] = body
        return copy.deepcopy(asset)


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
        self.manifest.update(schema=2, artifact_id=8, artifact_digest='sha256:'+hashlib.sha256(stream.getvalue()).hexdigest(),
                             build_context=release.build_context(self.github,self.github.source))
        (self.directory/'ci-artifact.zip').write_bytes(stream.getvalue())
        (self.directory/'manifest.json').write_text(json.dumps(self.manifest))

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
                           ('path', '.github/workflows/fuzz.yml'),
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
        self.github.artifact_name = 'AD_Killer_By_NFQUEUE-module-preview'
        stream = io.BytesIO()
        with zipfile.ZipFile(stream,'w') as archive:
            for name in self.manifest['assets']: archive.writestr(name,(self.directory/name).read_bytes())
        self.github.artifact_body = stream.getvalue()
        self.manifest['artifact_digest'] = 'sha256:'+hashlib.sha256(stream.getvalue()).hexdigest()
        (self.directory/'ci-artifact.zip').write_bytes(stream.getvalue())
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
        self.assertTrue(all(method == 'GET' for method, _, _ in self.github.calls))
        self.manifest['channel'] = 'preview'
        with self.assertRaises(ValueError):
            release.validate_assets(self.directory, self.manifest)

    def test_missing_owner_acknowledgement(self):
        with patch.dict(os.environ, {'KERNELSU_IMMUTABLE_RELEASES': ''}), self.assertRaises(ValueError):
            release.publish(self.github, self.directory)
        self.assertFalse(self.github.calls)

    def test_interrupted_upload_never_publishes_or_deletes(self):
        self.github.upload_fails = True
        with self.assertRaises(release.PublicationUnconfirmed):
            self.publish()
        self.assertFalse(any(method in ('PATCH', 'DELETE') for method, _, _ in self.github.calls))

    def test_mutable_result_is_reported_as_failure(self):
        self.github.immutable = False
        with self.assertRaisesRegex(release.PublicationUnconfirmed, 'published_pending_verification'):
            self.publish()

    def test_monotonic_codes_include_previews(self):
        self.github.releases = [{'tag_name': 'v1.2.5', 'draft': False, 'prerelease': True,
            'assets': [{'name': 'update.json', 'browser_download_url': release.REPO + '/releases/download/v1.2.5/update.json'}]}]
        for code, accepted in ((12500, True), (12601, False), (12700, False)):
            self.github.prior_code = code
            if accepted:
                release.check_monotonic(self.github, {'version':'v1.2.6','versionCode': 12601})
            else:
                with self.assertRaises(ValueError):
                    release.check_monotonic(self.github, {'version':'v1.2.6','versionCode': 12601})
        with self.assertRaisesRegex(ValueError,'version must exceed'):
            release.check_monotonic(self.github,{'version':'v1.1','versionCode':99999})

    def receipt(self):
        return json.loads((self.directory/'release-state.json').read_text())

    def make_draft(self):
        self.github.request('/releases', {'tag_name':self.manifest['version'], 'target_commitish':self.github.source,
            'name':self.manifest['version'], 'body':(self.directory/'changelog.md').read_text(),
            'draft':True,'prerelease':False,'make_latest':'false'},'POST')
        self.github.calls.clear()

    def resume(self, confirmation=None):
        with redirect_stdout(io.StringIO()), patch.object(release.time,'sleep'), patch.dict(os.environ,{'KERNELSU_IMMUTABLE_RELEASES':'enabled'}):
            release.publish(self.github,self.directory,10,confirmation or release.fingerprint(self.manifest))

    def test_draft_without_a_tag_is_already_occupied(self):
        self.make_draft()
        with self.assertRaisesRegex(ValueError,'draft already'):
            release.check_unused(self.github,self.manifest['version'])
        self.assertTrue(all(method=='GET' for method,_,_ in self.github.calls))

    def test_disabled_setting_stops_before_any_remote_write(self):
        self.github.setting={'enabled':False}
        with self.assertRaisesRegex(ValueError,'immutability disabled'): self.publish()
        self.assertTrue(all(method=='GET' for method,_,_ in self.github.calls))

    def test_setting_permission_failure_is_recorded_and_final_result_still_verified(self):
        self.github.setting=release.GitHubAPIError('GET',403)
        self.publish()
        self.assertEqual(self.receipt()['immutability'],{'status':'unverified','http_status':403})
        self.assertTrue(self.receipt()['immutable'])

    def test_hidden_setting_is_not_reported_as_enabled(self):
        self.github.setting=None
        self.publish()
        self.assertEqual(self.receipt()['immutability']['status'],'unverified')

    def test_setting_changed_during_upload_stops_publication(self):
        original=self.github.request; reads=0
        def request(path,*args,**kwargs):
            nonlocal reads
            if path=='/immutable-releases':
                reads+=1
                return {'enabled':reads==1}
            return original(path,*args,**kwargs)
        with patch.object(self.github,'request',side_effect=request),self.assertRaisesRegex(ValueError,'immutability disabled'):
            self.publish()
        self.assertEqual(len(self.github.assets),4)
        self.assertFalse(any(method=='PATCH' for method,_,_ in self.github.calls))

    def test_manifest_cannot_rebind_modified_files_to_an_original_ci_archive(self):
        with zipfile.ZipFile(self.package,'w') as archive:
            archive.writestr('module.prop',self.props)
            archive.writestr('unexpected.txt','different bytes')
        write_checksum(self.package); generate(self.package,self.directory/'source-notes.md')
        self.manifest['assets']={name:hashlib.sha256((self.directory/name).read_bytes()).hexdigest() for name in self.manifest['assets']}
        (self.directory/'manifest.json').write_text(json.dumps(self.manifest))
        with self.assertRaisesRegex(ValueError,'original CI bytes'): self.publish()
        self.assertTrue(all(method=='GET' for method,_,_ in self.github.calls))

    def test_partial_draft_inspection_and_explicit_resume_upload_only_missing_bytes(self):
        original=self.github.upload; count=0
        def upload(*args):
            nonlocal count
            count+=1
            if count==3: raise TimeoutError('connection lost')
            return original(*args)
        with patch.object(self.github,'upload',side_effect=upload),self.assertRaises(release.PublicationUnconfirmed): self.publish()
        self.assertEqual(len(self.github.assets),2)
        preserved=copy.deepcopy(self.github.assets)
        with redirect_stdout(io.StringIO()): report=release.inspect(self.github,self.directory)
        self.assertTrue(report['recoverable']); self.assertEqual(len(report['missing_assets']),2)
        self.resume(report['manifest_sha256'])
        self.assertEqual(self.github.assets[:2],preserved)
        self.assertEqual(len(self.github.assets),4)
        self.assertEqual(sum(method=='POST' for method,_,_ in self.github.calls),1)
        self.assertEqual(sum(method=='PATCH' for method,_,_ in self.github.calls),1)
        self.assertEqual(self.receipt()['phase'],'published_verified')

    def test_wrong_confirmation_does_not_resume_a_draft(self):
        self.make_draft()
        with self.assertRaisesRegex(ValueError,'exact draft ID'): self.resume('0'*64)
        self.assertTrue(all(method=='GET' for method,_,_ in self.github.calls))

    def test_changed_draft_identity_cannot_resume(self):
        self.make_draft()
        for field,value in [('target_commitish','b'*40),('body','changed'),('name','changed'),('prerelease',True),('tag_name','v9.0')]:
            with self.subTest(field=field),patch.dict(self.github.remote,{field:value}),self.assertRaises(ValueError): self.resume()
        self.assertTrue(all(method=='GET' for method,_,_ in self.github.calls))

    def test_resume_checks_remote_bytes_and_refuses_corruption_or_duplicates(self):
        self.make_draft(); name=next(iter(self.manifest['assets']))
        asset=self.github.upload(10,name,(self.directory/name).read_bytes())
        self.github.calls.clear()
        original=self.github.asset_bytes[asset['id']]
        self.github.asset_bytes[asset['id']]=original+b'changed'
        with self.assertRaisesRegex(ValueError,'remote bytes differ'): self.resume()
        self.github.asset_bytes[asset['id']]=original
        self.github.assets.append(copy.deepcopy(asset))
        with self.assertRaisesRegex(ValueError,'duplicate'): self.resume()
        self.assertTrue(all(method=='GET' for method,_,_ in self.github.calls))

    def test_upload_response_loss_keeps_one_copy_and_requires_explicit_resume(self):
        original=self.github.upload
        def upload(*args):
            original(*args); raise TimeoutError('upload response lost')
        with patch.object(self.github,'upload',side_effect=upload),self.assertRaises(release.PublicationUnconfirmed): self.publish()
        self.assertEqual(len(self.github.assets),1)
        with self.assertRaisesRegex(ValueError,'prior publication request'): self.publish()
        self.resume()
        self.assertEqual(len(self.github.assets),4)
        self.assertEqual(len({item['name'] for item in self.github.assets}),4)

    def test_create_response_loss_never_retries_post(self):
        original=self.github.request
        def request(path,data=None,method='GET',**kwargs):
            result=original(path,data,method,**kwargs)
            if path=='/releases' and method=='POST': raise TimeoutError('response lost')
            return result
        with patch.object(self.github,'request',side_effect=request),self.assertRaises(release.PublicationUnconfirmed): self.publish()
        self.assertEqual(self.receipt()['phase'],'draft_creation_unconfirmed')
        with self.assertRaisesRegex(ValueError,'prior publication request'): self.publish()
        self.assertEqual(sum(method=='POST' for method,_,_ in self.github.calls),1)
        with redirect_stdout(io.StringIO()): report=release.inspect(self.github,self.directory)
        self.assertEqual(report['release_id'],10)
        self.resume(report['manifest_sha256'])
        self.assertEqual(sum(method=='POST' for method,_,_ in self.github.calls),1)

    def test_publish_response_loss_is_verified_only_with_gets(self):
        original=self.github.request
        def request(path,data=None,method='GET',**kwargs):
            result=original(path,data,method,**kwargs)
            if method=='PATCH': raise TimeoutError('publication response lost')
            return result
        with patch.object(self.github,'request',side_effect=request): self.publish()
        self.assertEqual(self.receipt()['phase'],'published_verified')
        self.assertEqual(sum(method=='PATCH' for method,_,_ in self.github.calls),1)

    def test_bounded_readback_handles_immutable_tag_and_latest_visibility(self):
        original=self.github.request; immutable_lag=tag_lag=latest_lag=True
        def request(path,*args,**kwargs):
            nonlocal immutable_lag,tag_lag,latest_lag
            result=original(path,*args,**kwargs)
            if self.github.remote and not self.github.remote['draft']:
                if path=='/releases/10' and immutable_lag:
                    immutable_lag=False; result['immutable']=False
                elif path.startswith('/git/ref/') and tag_lag:
                    tag_lag=False; return None
                elif path=='/releases/latest' and latest_lag:
                    latest_lag=False; return {'id':9}
            return result
        with patch.object(self.github,'request',side_effect=request): self.publish()
        self.assertFalse(immutable_lag or tag_lag or latest_lag)
        self.assertEqual(sum(method=='PATCH' for method,_,_ in self.github.calls),1)

    def test_receipt_failure_before_create_makes_no_remote_write(self):
        original=release.Receipt.write
        def write(receipt,phase,**facts):
            if phase=='draft_create_requested': raise OSError('disk full')
            return original(receipt,phase,**facts)
        with patch.object(release.Receipt,'write',new=write),self.assertRaises(OSError): self.publish()
        self.assertTrue(all(method=='GET' for method,_,_ in self.github.calls))

    def test_receipt_failure_after_publication_is_not_reported_as_safe_to_retry(self):
        original=release_receipt.atomic_json
        def write(path,value):
            if self.github.remote and not self.github.remote['draft']: raise OSError('disk full')
            return original(path,value)
        with patch.object(release_receipt,'atomic_json',side_effect=write),self.assertRaises(release.PublicationUnconfirmed): self.publish()
        self.assertFalse(self.github.remote['draft'])
        self.assertEqual(sum(method=='PATCH' for method,_,_ in self.github.calls),1)
        self.assertEqual(self.receipt()['phase'],'publication_requested')

    def test_local_double_click_is_rejected_before_remote_requests(self):
        with release.release_lock(self.directory),self.assertRaisesRegex(ValueError,'another local'): self.publish()
        self.assertFalse(self.github.calls)

    def test_inspection_without_visible_release_never_creates_one(self):
        with redirect_stdout(io.StringIO()): report=release.inspect(self.github,self.directory)
        self.assertFalse(report['recoverable']); self.assertEqual(report['candidates'],[])
        self.assertTrue(all(method=='GET' for method,_,_ in self.github.calls))

    def test_inspection_confirms_an_already_published_release_without_any_writes(self):
        self.publish(); self.github.calls.clear(); self.github.latest=99
        with redirect_stdout(io.StringIO()): report=release.inspect(self.github,self.directory,10)
        self.assertEqual(report['phase'],'published_verified')
        self.assertFalse(report['recoverable']); self.assertFalse(report['latest'])
        self.assertTrue(all(method=='GET' for method,_,_ in self.github.calls))

    def test_ambiguous_drafts_are_diagnosed_and_cannot_resume(self):
        self.make_draft(); self.github.releases.append({**self.github.remote,'id':11})
        with redirect_stdout(io.StringIO()): report=release.inspect(self.github,self.directory)
        self.assertEqual(len(report['candidates']),2); self.assertFalse(report['recoverable'])
        with self.assertRaisesRegex(ValueError,'ambiguous'): self.resume()
        self.assertTrue(all(method=='GET' for method,_,_ in self.github.calls))

    def test_workflow_summary_binds_ci_source_channel_and_asset_digests(self):
        summary=self.directory/'summary.md'
        with patch.dict(os.environ,{'GITHUB_STEP_SUMMARY':str(summary)}): self.publish()
        text=summary.read_text()
        for value in (self.github.source,'123','stable',self.manifest['artifact_digest'],*self.manifest['assets'].values()):
            self.assertIn(value,text)
        self.assertNotIn('Authorization',text)

    def test_draft_and_asset_visibility_lag_never_repeats_uploads(self):
        original_request=self.github.request; original_pages=self.github.pages
        first=True; assets_lag=True
        def request(path,*args,**kwargs):
            nonlocal first
            if path=='/releases/10' and first:
                first=False; return None
            return original_request(path,*args,**kwargs)
        def pages(path,key=None):
            nonlocal assets_lag
            if path=='/releases/10/assets' and len(self.github.assets)==4 and self.github.remote['draft'] and assets_lag:
                assets_lag=False; return iter(copy.deepcopy(self.github.assets[:-1]))
            return original_pages(path,key)
        with patch.object(self.github,'request',side_effect=request),patch.object(self.github,'pages',side_effect=pages): self.publish()
        self.assertFalse(first or assets_lag)
        self.assertEqual(sum(method=='UPLOAD' for method,_,_ in self.github.calls),4)

    def test_published_version_can_be_prepared_for_read_only_inspection(self):
        self.publish(); self.github.calls.clear()
        with redirect_stdout(io.StringIO()),patch.object(release.subprocess,'run'):
            release.prepare(self.github,'123','v1.2.6','stable',self.directory/'inspect',allow_existing=True)
        self.assertTrue(all(method=='GET' for method,_,_ in self.github.calls))

    def test_resume_never_modifies_an_already_published_release(self):
        self.publish(); self.github.calls.clear()
        previous=(self.directory/'release-state.json').read_bytes()
        with self.assertRaises(ValueError): self.resume()
        self.assertTrue(all(method=='GET' for method,_,_ in self.github.calls))
        self.assertEqual((self.directory/'release-state.json').read_bytes(),previous)

    def test_malformed_visibility_does_not_claim_known_publication(self):
        original=self.github.request
        def request(path,*args,**kwargs):
            result=original(path,*args,**kwargs)
            if path=='/releases/10' and result and result.get('draft') is False: result.pop('draft')
            return result
        with patch.object(self.github,'request',side_effect=request),self.assertRaises(release.PublicationUnconfirmed): self.publish()
        self.assertEqual(self.receipt()['phase'],'publication_unconfirmed')
        self.assertFalse(self.receipt().get('published',False))
        self.assertEqual(sum(method=='PATCH' for method,_,_ in self.github.calls),1)

    def test_modified_build_recipe_is_rejected_before_writes(self):
        self.manifest['build_context']['source_sha256']['build.sh']='0'*64
        (self.directory/'manifest.json').write_text(json.dumps(self.manifest))
        with self.assertRaisesRegex(ValueError,'recipe/source'): self.publish()
        self.assertTrue(all(method=='GET' for method,_,_ in self.github.calls))

    def test_summary_failure_does_not_turn_verified_publication_into_a_retry(self):
        blocked=self.directory/'summary-directory'; blocked.mkdir()
        with patch.dict(os.environ,{'GITHUB_STEP_SUMMARY':str(blocked)}),redirect_stderr(io.StringIO()) as errors: self.publish()
        self.assertIn('summary unavailable',errors.getvalue())
        self.assertEqual(self.receipt()['phase'],'published_verified')
        self.assertEqual(sum(method=='PATCH' for method,_,_ in self.github.calls),1)

    def test_cli_inspect_enforces_read_only_mode_without_owner_ack(self):
        arguments=['publish_release.py','inspect','--output',str(self.directory)]
        with patch.object(sys,'argv',arguments),patch.object(release,'GitHub',return_value=self.github) as client,redirect_stdout(io.StringIO()):
            release.main()
        self.assertTrue(client.call_args.kwargs['read_only'])
        self.assertTrue(all(method=='GET' for method,_,_ in self.github.calls))

    def test_cli_rerun_does_not_repeat_a_publication_attempt(self):
        arguments=['publish_release.py','publish','--output',str(self.directory)]
        with patch.object(sys,'argv',arguments),patch.object(release,'GitHub',return_value=self.github),patch.dict(os.environ,{'GITHUB_RUN_ATTEMPT':'2'}),self.assertRaisesRegex(ValueError,'do not rerun'):
            release.main()
        self.assertFalse(self.github.calls)

    def test_cli_resume_requires_explicit_confirmation(self):
        arguments=['publish_release.py','resume','--output',str(self.directory),'--release-id','10']
        with patch.object(sys,'argv',arguments),patch.object(release,'GitHub',return_value=self.github),self.assertRaisesRegex(ValueError,'resume requires'):
            release.main()
        self.assertFalse(self.github.calls)

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
