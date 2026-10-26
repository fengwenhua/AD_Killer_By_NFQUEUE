#!/usr/bin/env python3
"""Publish a new tag once from a completed main CI artifact; never replace assets.

prepare is read-only on GitHub. publish leaves a draft on upload failure for
inspection. Explicit recovery accepts only a verified draft and matching CI
artifact; no mutation is retried, deleted, overwritten, or silently resumed.
"""
import argparse
import base64
import hashlib
import http.client
import io
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile
from contextlib import contextmanager
import fcntl
import stat

from validate_listing import properties, unique_object, validate_manifest
from release_receipt import Receipt, atomic_json, fingerprint

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from validate_manager_update import validate_manager_update

REPOSITORY = 'fengwenhua/AD_Killer_By_NFQUEUE'
API = 'https://api.github.com/repos/' + REPOSITORY
REPO = 'https://github.com/' + REPOSITORY
REQUIRED_JOBS = {'host-tests', 'webui-layout', 'android-build'}
MAX_DOWNLOAD = 128 * 1024 * 1024
READ_ATTEMPTS = 4


def require(condition, message):
    if not condition:
        raise ValueError(message)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args):
        return None


class HTTPSRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, fp, code, message, headers, url):
        require(urllib.parse.urlsplit(url).scheme == 'https', 'insecure download redirect')
        return super().redirect_request(request, fp, code, message, headers, url)


class GitHubAPIError(ValueError):
    def __init__(self, method, status):
        self.status = status
        super().__init__(f'GitHub API {method} failed (HTTP {status})')


class PublicationUnconfirmed(ValueError):
    """A write may have succeeded; callers must inspect, never blindly retry."""


class GitHub:
    def __init__(self, token, read_only=False):
        require(bool(token), 'GITHUB_TOKEN is required')
        self.headers = {'Authorization': 'Bearer ' + token, 'Accept': 'application/vnd.github+json',
                        'X-GitHub-Api-Version': '2026-03-10', 'User-Agent': 'AD-Killer-release'}
        self.deadline = None
        self.read_only = read_only

    def timeout(self, maximum):
        if self.deadline is None: return maximum
        remaining = self.deadline - time.monotonic()
        if remaining <= 0: raise TimeoutError('readback budget expired')
        return min(maximum, remaining)

    def read(self, response):
        body = bytearray()
        while True:
            self.timeout(10)
            try: chunk = response.read1(min(65536, MAX_DOWNLOAD + 1 - len(body)))
            except http.client.HTTPException:
                raise OSError('incomplete HTTP response; request completion must be inspected') from None
            if not chunk: return bytes(body)
            body.extend(chunk)
            require(len(body) <= MAX_DOWNLOAD, 'response exceeds size limit')

    def request(self, path, data=None, method='GET', timeout=30):
        require(not self.read_only or (method == 'GET' and data is None), 'read-only release operation forbids writes')
        require(path.startswith('/') and not path.startswith('//'), 'invalid API path')
        headers = dict(self.headers)
        if data is not None:
            headers['Content-Type'] = 'application/json'
            data = json.dumps(data).encode()
        try:
            with urllib.request.build_opener(NoRedirect).open(urllib.request.Request(
                    API + path, data=data, headers=headers, method=method), timeout=self.timeout(timeout)) as response:
                body = self.read(response)
                require(len(body) <= MAX_DOWNLOAD, 'API response too large')
                return json.loads(body, object_pairs_hook=unique_object) if body else None
        except urllib.error.HTTPError as error:
            if method == 'GET' and error.code == 404:
                return None
            raise GitHubAPIError(method, error.code) from None
        except http.client.HTTPException:
            raise OSError('invalid HTTP response; request completion must be inspected') from None

    def pages(self, path, key=None):
        for page in range(1, 101):
            result = self.request(path + ('&' if '?' in path else '?') + f'per_page=100&page={page}', timeout=10)
            require(result is not None, 'missing paginated API response')
            require(key is None or (type(result) is dict and key in result), 'invalid paginated API envelope')
            values = result[key] if key else result
            require(type(values) is list and len(values) <= 100 and all(type(item) is dict for item in values), 'invalid paginated API records')
            yield from values
            if len(values) < 100:
                return
        raise ValueError('API pagination exceeded safety limit')

    def artifact(self, artifact_id):
        # Artifact download negotiates the REST JSON media type before its
        # signed redirect; release asset content requires octet-stream.
        return self.binary(f'/actions/artifacts/{artifact_id}/zip', 'application/vnd.github+json')

    def asset(self, asset_id):
        require(type(asset_id) is int and asset_id > 0, 'invalid release asset ID')
        return self.binary(f'/releases/assets/{asset_id}')

    def binary(self, path, accept='application/octet-stream'):
        headers = {**self.headers, 'Accept': accept}
        try:
            with urllib.request.build_opener(NoRedirect).open(urllib.request.Request(
                    API + path, headers=headers), timeout=self.timeout(10)) as response:
                body = self.read(response)
        except urllib.error.HTTPError as error:
            if error.code != 302: raise GitHubAPIError('GET', error.code) from None
            return self.public_download(error.headers.get('Location', ''))
        except http.client.HTTPException:
            raise OSError('invalid binary download response') from None
        require(len(body) <= MAX_DOWNLOAD, 'artifact too large')
        return body

    def public_download(self, url):
        require(url.startswith('https://'), 'download must use HTTPS')
        # Deliberately omit Authorization on redirects and public download hosts.
        try:
            with urllib.request.build_opener(HTTPSRedirect).open(urllib.request.Request(url, headers={'User-Agent': 'AD-Killer-release'}), timeout=self.timeout(10)) as response:
                require(response.url.startswith('https://'), 'insecure download redirect')
                body = self.read(response)
        except http.client.HTTPException:
            raise OSError('invalid public download response') from None
        require(len(body) <= MAX_DOWNLOAD, 'download exceeds size limit')
        return body

    def source_file(self, source, path):
        result = self.request('/contents/' + path + '?ref=' + source)
        require(result is not None and result.get('encoding') == 'base64', 'missing CI source file: ' + path)
        return base64.b64decode(result['content'], validate=False)

    def upload(self, release_id, name, body):
        require(not self.read_only, 'read-only release operation forbids uploads')
        url = f'https://uploads.github.com/repos/{REPOSITORY}/releases/{release_id}/assets?name=' + urllib.parse.quote(name, safe='')
        headers = {**self.headers, 'Content-Type': 'application/octet-stream'}
        try:
            with urllib.request.build_opener(NoRedirect).open(urllib.request.Request(url, data=body,
                    headers=headers, method='POST'), timeout=30) as response:
                return json.loads(self.read(response), object_pairs_hook=unique_object)
        except http.client.HTTPException:
            raise OSError('upload response incomplete; inspect the draft before retrying') from None


def check_run(github, run_id, expected_source=None):
    require(re.fullmatch(r'[1-9][0-9]{0,19}', str(run_id)) is not None, 'invalid CI run ID')
    run = github.request(f'/actions/runs/{run_id}')
    require(run is not None and run.get('head_repository', {}).get('full_name') == REPOSITORY, 'CI must belong to this repository')
    require(run.get('path') == '.github/workflows/ci.yml' and run.get('head_branch') == 'main'
            and run.get('event') in ('push', 'workflow_dispatch'), 'only trusted main CI runs may publish')
    require(run.get('status') == 'completed' and run.get('conclusion') == 'success', 'CI is not fully green')
    require(re.fullmatch(r'[0-9a-f]{40}', run.get('head_sha', '')) is not None, 'invalid CI source SHA')
    require(expected_source is None or run['head_sha'] == expected_source, 'CI source changed')
    jobs = list(github.pages(f'/actions/runs/{run_id}/jobs?filter=latest', 'jobs'))
    require(REQUIRED_JOBS <= {job['name'] for job in jobs}, 'missing required CI jobs')
    require(all(job.get('status') == 'completed' and job.get('conclusion') == 'success' for job in jobs), 'CI has skipped or failed jobs')
    return run


def check_unused(github, version):
    require(re.fullmatch(r'v[0-9]{1,6}\.[0-9]{1,6}(?:\.[0-9]{1,6})?', version) is not None, 'invalid version tag')
    require(github.request('/releases/tags/' + version) is None, 'release/tag already occupied; use a new version')
    require(github.request('/git/ref/tags/' + version) is None, 'tag already exists; never move or reuse it')
    # The tag endpoint documents published releases only. Authenticated listing
    # is also needed to detect a failed, not-yet-tagged draft.
    require(not any(item.get('tag_name') == version for item in github.pages('/releases')),
            'draft already occupies this version; inspect it and use explicit recovery')


def check_monotonic(github, metadata):
    def parts(value):
        require(isinstance(value,str) and re.fullmatch(r'v[0-9]{1,6}\.[0-9]{1,6}(?:\.[0-9]{1,6})?',value) is not None, 'cannot audit semantic release version')
        return tuple((list(map(int,value[1:].split('.')))+[0])[:3])
    for release in github.pages('/releases'):
        if release['draft']:
            continue
        require(parts(metadata['version']) > parts(release['tag_name']), 'version must exceed every published stable/preview version')
        assets = [asset for asset in release['assets'] if asset['name'] == 'update.json']
        if not assets:
            # Historical releases without manager metadata still cannot be reused
            # or semantically downgraded. Current releases always include JSON.
            old = release['tag_name']
            require(re.fullmatch(r'v[0-9]+\.[0-9]+(?:\.[0-9]+)?', old) is not None, 'cannot audit historical release ' + old)
            continue
        require(len(assets) == 1, 'duplicate historical update.json')
        url = assets[0]['browser_download_url']
        require(url == REPO + '/releases/download/' + release['tag_name'] + '/update.json', 'unexpected historical metadata URL')
        old = json.loads(github.public_download(url), object_pairs_hook=unique_object)
        code = old.get('versionCode')
        require(type(code) is int and 1 <= code <= 2**31 - 1, 'cannot audit historical versionCode')
        require(metadata['versionCode'] > code, 'versionCode must exceed every published stable/preview release')


def validate_assets(directory, manifest):
    require(re.fullmatch(r'v[0-9]{1,6}\.[0-9]{1,6}(?:\.[0-9]{1,6})?', manifest.get('version', '')) is not None, 'invalid manifest version')
    require(manifest.get('channel') in ('stable', 'preview'), 'invalid manifest channel')
    require(set(manifest['assets']) == {'AD_Killer_By_NFQUEUE-' + manifest['version'] + '.zip',
            'AD_Killer_By_NFQUEUE-' + manifest['version'] + '.zip.sha256', 'update.json', 'changelog.md'}, 'unexpected release assets')
    for name, digest in manifest['assets'].items():
        require(Path(name).name == name and name not in ('.', '..'), 'unsafe asset name')
        require(re.fullmatch(r'[0-9a-f]{64}', digest) is not None, 'invalid asset digest')
        path = directory / name
        require(not path.is_symlink() and path.is_file() and path.stat().st_size <= MAX_DOWNLOAD, 'unsafe artifact file: ' + name)
        require(hashlib.sha256(path.read_bytes()).hexdigest() == digest, 'artifact changed: ' + name)
    metadata = validate_manager_update(directory / ('AD_Killer_By_NFQUEUE-' + manifest['version'] + '.zip'))
    require(metadata['releaseChannel'] == manifest['channel'], 'artifact channel mismatch')
    return metadata


def ci_artifact(github, run_id, channel):
    name = 'AD_Killer_By_NFQUEUE-module' + ('-preview' if channel == 'preview' else '')
    matches = [item for item in github.pages(f'/actions/runs/{run_id}/artifacts', 'artifacts')
               if item['name'] == name and not item['expired']]
    require(len(matches) == 1, 'missing or ambiguous CI artifact')
    result = matches[0]
    require(type(result.get('id')) is int and result['id'] > 0 and
            re.fullmatch(r'sha256:[0-9a-f]{64}', result.get('digest', '')), 'invalid CI artifact identity')
    return result


def verify_archive(data, manifest):
    require('sha256:' + hashlib.sha256(data).hexdigest() == manifest['artifact_digest'], 'CI artifact digest mismatch')
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        require(len(archive.namelist()) == 4 and set(archive.namelist()) == set(manifest['assets']), 'unexpected CI artifact layout')
        require(sum(item.file_size for item in archive.infolist()) <= MAX_DOWNLOAD, 'artifact unpacked size too large')
        for name, digest in manifest['assets'].items():
            require(hashlib.sha256(archive.read(name)).hexdigest() == digest, 'manifest differs from original CI bytes: ' + name)


def build_context(github, source):
    paths = ('.github/workflows/ci.yml', 'build.sh', 'frontend/package-lock.json')
    files = {path: github.source_file(source, path) for path in paths}
    workflow, build = files[paths[0]].decode(), files[paths[1]].decode()
    return {'source_sha256': {path: hashlib.sha256(body).hexdigest() for path, body in files.items()},
            'declared_ndk_versions': sorted(set(re.findall(r'ndk;([0-9.]+)', workflow))),
            'declared_android_targets': sorted(set(re.findall(r'aarch64-linux-android([0-9]+)-clang', build))),
            'scope': 'source_recipe_not_a_runtime_attestation'}


def prepare(github, run_id, version, channel, output, release_id=None, allow_existing=False):
    require(channel in ('stable', 'preview'), 'invalid release channel')
    require(re.fullmatch(r'v[0-9]{1,6}\.[0-9]{1,6}(?:\.[0-9]{1,6})?', version) is not None, 'invalid version tag')
    if release_id is None and not allow_existing: check_unused(github, version)
    if release_id is not None: require(type(release_id) is int and release_id > 0, 'invalid recovery release ID')
    run = check_run(github, run_id)
    source = run['head_sha']
    props = properties(github.source_file(source, 'module/module.prop').decode())
    listing = json.loads(github.source_file(source, 'module.yaml'), object_pairs_hook=unique_object)
    validate_manifest(listing, props)
    require(props['version'] == version, 'requested version differs from green CI source')
    artifact = ci_artifact(github, run_id, channel)
    data = github.artifact(artifact['id'])
    require(artifact.get('digest') == 'sha256:' + hashlib.sha256(data).hexdigest(), 'CI artifact digest mismatch')
    name = 'AD_Killer_By_NFQUEUE-' + version + '.zip'
    names = {name, name + '.sha256', 'update.json', 'changelog.md'}
    output.mkdir(parents=True, exist_ok=False)
    with zipfile.ZipFile(io.BytesIO(data)) as archive:
        require(len(archive.namelist()) == len(names) and set(archive.namelist()) == names, 'unexpected CI artifact layout')
        require(sum(item.file_size for item in archive.infolist()) <= MAX_DOWNLOAD, 'artifact unpacked size too large')
        for item in names:
            (output / item).write_bytes(archive.read(item))
    subprocess.run([sys.executable, str(ROOT / 'tests/validate_module_zip.py'), str(output / name)], check=True)
    record = {'schema': 2, 'run_id': str(run_id), 'source': source, 'version': version, 'channel': channel,
              'artifact_id': artifact['id'], 'artifact_digest': artifact['digest'],
              'build_context': build_context(github, source),
              'assets': {item: hashlib.sha256((output / item).read_bytes()).hexdigest() for item in sorted(names)}}
    metadata = validate_assets(output, record)
    require(metadata['versionCode'] == int(props['versionCode']) - (channel == 'preview'), 'CI package/source versionCode mismatch')
    require((output / 'changelog.md').read_bytes() == github.source_file(source, 'CHANGELOG.md'), 'CI release notes/source mismatch')
    if release_id is None and not allow_existing: check_monotonic(github, metadata)
    (output / 'ci-artifact.zip').write_bytes(data)
    atomic_json(output / 'manifest.json', record)
    receipt = Receipt(output, record, 'prepare')
    receipt.write('prepared', recovery_release_id=release_id)
    receipt.summary()
    print(json.dumps({'prepared': str(output), **record}))


def load_prepared(github, directory):
    path = directory / 'manifest.json'
    require(not path.is_symlink() and path.is_file() and path.stat().st_size <= 65536, 'manifest is not a bounded regular file')
    manifest = json.loads(path.read_text(), object_pairs_hook=unique_object)
    require(manifest.get('schema') == 2, 'prepare this CI artifact again with the current release tool')
    require(type(manifest.get('artifact_id')) is int and manifest['artifact_id'] > 0, 'invalid manifest artifact ID')
    run = check_run(github, manifest['run_id'], manifest['source'])
    artifact = ci_artifact(github, manifest['run_id'], manifest['channel'])
    require(artifact['id'] == manifest['artifact_id'] and artifact['digest'] == manifest['artifact_digest'], 'CI artifact identity changed')
    archive = directory / 'ci-artifact.zip'
    require(not archive.is_symlink() and archive.is_file() and archive.stat().st_size <= MAX_DOWNLOAD, 'original CI archive missing or unsafe; prepare again')
    verify_archive(archive.read_bytes(), manifest)
    require(manifest.get('build_context') == build_context(github, manifest['source']), 'CI build recipe/source changed')
    metadata = validate_assets(directory, manifest)
    props = properties(github.source_file(run['head_sha'], 'module/module.prop').decode())
    require(props['version'] == manifest['version'] and metadata['versionCode'] == int(props['versionCode']) - (manifest['channel'] == 'preview'), 'CI package/source identity changed')
    require((directory / 'changelog.md').read_bytes() == github.source_file(manifest['source'], 'CHANGELOG.md'), 'CI release notes/source changed')
    return manifest, metadata


def immutable_setting(github):
    try:
        value = github.request('/immutable-releases', timeout=10)
    except GitHubAPIError as error:
        if error.status not in (403, 404): raise
        return {'status': 'unverified', 'http_status': error.status}
    # 404 can mean disabled or permissions hidden by GitHub. Never describe
    # it as enabled. Owner acknowledgement remains required and publication
    # must still verify immutable=true; no extra private token is requested.
    if value is None: return {'status': 'unverified', 'http_status': 404}
    require(type(value) is dict and type(value.get('enabled')) is bool, 'invalid immutable-releases setting response')
    require(value['enabled'], 'repository explicitly reports release immutability disabled')
    return {'status': 'enabled', 'http_status': 200}


@contextmanager
def readback_budget(github):
    previous = getattr(github, 'deadline', None)
    github.deadline = min(previous or float('inf'), time.monotonic() + 45)
    try: yield
    finally: github.deadline = previous


class ReadbackPending(ValueError): pass


@contextmanager
def release_lock(directory):
    descriptor = os.open(directory / '.release-operation.lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW | os.O_CLOEXEC, 0o600)
    try:
        info = os.fstat(descriptor)
        require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, 'unsafe local release lock')
        try: fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError: raise ValueError('another local release operation is running') from None
        yield
    finally: os.close(descriptor)


def readback(github, check):
    last = None
    with readback_budget(github):
        for attempt in range(READ_ATTEMPTS):
            try: return check()
            except (ReadbackPending, GitHubAPIError, OSError, json.JSONDecodeError) as error:
                last = error
                if attempt + 1 < READ_ATTEMPTS: time.sleep(1)
    raise last


def release_identity(current, manifest, directory, release_id, draft):
    require(type(current) is dict, 'release not found or invalid; inspect the original request, do not recreate it')
    require(type(current.get('id')) is int and current['id'] == release_id, 'release ID mismatch')
    require(current.get('tag_name') == manifest['version'] and current.get('target_commitish') == manifest['source'], 'release source/tag mismatch')
    require(current.get('prerelease') is (manifest['channel'] == 'preview'), 'release channel changed')
    require(current.get('name') == manifest['version'] + (' Preview' if manifest['channel'] == 'preview' else ''), 'release title changed')
    require(current.get('body') == (directory / 'changelog.md').read_text(), 'release notes changed')
    require(current.get('draft') is draft, 'release is already public; inspect it, never republish or replace assets' if draft else 'release is not public yet')
    if draft:
        require(not current.get('immutable') and not current.get('published_at'), 'draft is not safely recoverable')
    else:
        require(current.get('html_url') == REPO + '/releases/tag/' + manifest['version'], 'published release URL mismatch')


def tag_matches(ref, source):
    return (type(ref) is dict and type(ref.get('object')) is dict and
            ref['object'].get('type') == 'commit' and ref['object'].get('sha') == source)


def inventory(github, release_id, manifest, directory, published=False, verify_bytes=True, pending=False):
    present = {}
    for asset in github.pages(f'/releases/{release_id}/assets'):
        require(type(asset) is dict, 'invalid release asset record')
        name = asset.get('name')
        require(name in manifest['assets'] and name not in present, 'unexpected or duplicate remote release asset')
        if pending and asset.get('state') != 'uploaded': raise ReadbackPending('uploaded asset is not visible yet')
        require(type(asset.get('id')) is int and asset['id'] > 0 and asset.get('state') == 'uploaded', 'remote asset is incomplete or unverified')
        require(asset.get('digest') == 'sha256:' + manifest['assets'][name] and type(asset.get('size')) is int
                and asset['size'] == (directory / name).stat().st_size, 'remote asset integrity mismatch: ' + name)
        if published:
            require(asset.get('browser_download_url') == REPO + '/releases/download/' + manifest['version'] + '/' + name, 'published asset URL mismatch')
        if verify_bytes:
            body = github.asset(asset['id'])
            require(len(body) == asset['size'] and hashlib.sha256(body).hexdigest() == manifest['assets'][name], 'remote bytes differ from the original CI artifact: ' + name)
        present[name] = asset['id']
    return sorted(set(manifest['assets']) - set(present))


def inspect(github, directory, release_id=None):
    with release_lock(directory):
        return inspect_locked(github, directory, release_id)


def inspect_locked(github, directory, release_id):
    manifest, _ = load_prepared(github, directory)
    receipt = Receipt(directory, manifest, 'inspect')
    try:
        matches = [item for item in github.pages('/releases') if item.get('tag_name') == manifest['version']]
        candidates = [{'release_id': item['id'], 'draft': item.get('draft') is True} for item in matches if type(item.get('id')) is int]
        receipt.write('release_candidates', candidates=candidates, recoverable=False)
        if release_id is None:
            if len(matches) != 1:
                print(json.dumps(receipt.value)); return receipt.value
            release_id = matches[0]['id']
        require(type(release_id) is int and release_id > 0, 'explicit positive release ID required')
        current = github.request(f'/releases/{release_id}', timeout=10)
        require(current is not None, 'release ID not found')
        draft = current.get('draft') is True
        release_identity(current, manifest, directory, release_id, draft)
        if not draft:
            verify_publication(github, directory, manifest, release_id, receipt, require_latest=False)
            print(json.dumps(receipt.value)); return receipt.value
        missing = inventory(github, release_id, manifest, directory, not draft)
        ref = github.request('/git/ref/tags/' + manifest['version'], timeout=10)
        require(ref is None or tag_matches(ref, manifest['source']), 'tag belongs to another source')
        receipt.write('draft_inspected' if draft else 'published_inspected', release_id=release_id,
                      missing_assets=missing, immutable=current.get('immutable') is True, published=not draft,
                      recoverable=draft and len(matches) == 1 and matches[0].get('id') == release_id)
        print(json.dumps(receipt.value))
        return receipt.value
    finally: receipt.summary()


def verify_publication(github, directory, manifest, release_id, receipt, require_latest=True):
    verified_assets = False
    def check():
        nonlocal verified_assets
        current = github.request(f'/releases/{release_id}', timeout=10)
        if current is None: raise ReadbackPending('publication is not visible yet')
        require(type(current) is dict, 'invalid publication response')
        require(type(current.get('draft')) is bool, 'publication visibility is unknown')
        require(type(current.get('id')) is int and current['id'] == release_id, 'publication release ID mismatch')
        if current.get('draft') is True: raise ReadbackPending('publication is not visible yet')
        receipt.write('published_pending_verification', release_id=release_id, published=True, immutable=current.get('immutable') is True)
        release_identity(current, manifest, directory, release_id, False)
        if current.get('immutable') is not True: raise ReadbackPending('published release is NOT immutable yet')
        missing = inventory(github, release_id, manifest, directory, True, not verified_assets, True)
        if missing: raise ReadbackPending('published assets are not all visible yet')
        verified_assets = True
        ref = github.request('/git/ref/tags/' + manifest['version'], timeout=10)
        if ref is None: raise ReadbackPending('published tag not visible yet')
        require(tag_matches(ref, manifest['source']), 'published tag/source mismatch')
        if manifest['channel'] == 'stable':
            latest = github.request('/releases/latest', timeout=10)
            if latest is None: raise ReadbackPending('Latest is not visible yet')
            require(type(latest) is dict and type(latest.get('id')) is int, 'invalid Latest response')
            receipt.value['latest'] = latest.get('id') == release_id
            if require_latest and not receipt.value['latest']: raise ReadbackPending('stable release is not Latest yet')
        return current
    try:
        result = readback(github, check)
    except (ValueError, OSError):
        phase = 'published_pending_verification' if receipt.value.get('published') else 'publication_unconfirmed'
        receipt.write(phase, release_id=release_id, action='inspect_only_do_not_repeat_publication')
        raise PublicationUnconfirmed(phase + ': inspect the existing release; no write was retried') from None
    receipt.write('published_verified', release_id=release_id, published=True, immutable=True,
                  release_url=result['html_url'])
    return result


def publish(github, directory, release_id=None, confirmation=None):
    with release_lock(directory):
        return publish_locked(github, directory, release_id, confirmation)


def publish_locked(github, directory, release_id=None, confirmation=None):
    require(os.environ.get('KERNELSU_IMMUTABLE_RELEASES') == 'enabled', 'owner must enable release immutability and acknowledge it in Actions variables first')
    manifest, metadata = load_prepared(github, directory)
    recovery = release_id is not None
    receipt = Receipt(directory, manifest, 'resume' if recovery else 'publish')
    try:
        if recovery:
            require(type(release_id) is int and release_id > 0 and confirmation == fingerprint(manifest), 'recovery requires the exact draft ID and inspected manifest SHA-256')
            current = github.request(f'/releases/{release_id}', timeout=10)
            # Preserve the prior receipt when an already-public or changed
            # release is mistakenly selected for draft recovery.
            release_identity(current, manifest, directory, release_id, True)
        else:
            require(receipt.previous_phase in (None, 'prepared', 'validated'), 'prior publication request exists; inspect it and use explicit recovery')
        receipt.write('validated', immutability=immutable_setting(github))
        check_monotonic(github, metadata)
        if not recovery:
            check_unused(github, manifest['version'])
            receipt.write('draft_create_requested')
            try:
                draft = github.request('/releases', {'tag_name': manifest['version'], 'target_commitish': manifest['source'],
                    'name': manifest['version'] + (' Preview' if manifest['channel'] == 'preview' else ''),
                    'body': (directory / 'changelog.md').read_text(), 'draft': True,
                    'prerelease': manifest['channel'] == 'preview', 'make_latest': 'false'}, 'POST')
                require(type(draft) is dict and type(draft.get('id')) is int and draft['id'] > 0, 'unexpected draft response')
                release_id = draft['id']
            except (ValueError, OSError):
                receipt.write('draft_creation_unconfirmed', action='inspect_matching_drafts_do_not_repeat_create')
                raise PublicationUnconfirmed('draft creation may have succeeded; inspect matching drafts, do not repeat POST') from None
        receipt.write('draft_selected', release_id=release_id)
        def draft_visible():
            current = github.request(f'/releases/{release_id}', timeout=10)
            if current is None: raise ReadbackPending('draft is not visible yet')
            release_identity(current, manifest, directory, release_id, True)
            return current
        readback(github, draft_visible)
        ref = github.request('/git/ref/tags/' + manifest['version'], timeout=10)
        require(ref is None or tag_matches(ref, manifest['source']), 'existing recovery tag belongs to another source')
        matches = [item for item in github.pages('/releases') if item.get('tag_name') == manifest['version']]
        require(len(matches) == 1 and matches[0].get('id') == release_id, 'ambiguous or replaced draft')
        missing = inventory(github, release_id, manifest, directory)
        for name in missing:
            receipt.write('asset_upload_requested', release_id=release_id, asset=name)
            try:
                asset = github.upload(release_id, name, (directory / name).read_bytes())
                require(type(asset) is dict and asset.get('name') == name and asset.get('digest') == 'sha256:' + manifest['assets'][name]
                        and asset.get('size') == (directory / name).stat().st_size, 'uploaded asset integrity mismatch')
            except (ValueError, OSError):
                receipt.write('draft_upload_interrupted', release_id=release_id, action='inspect_then_explicit_resume')
                raise PublicationUnconfirmed('draft upload interrupted; inspect existing bytes before explicit recovery') from None
        def draft_ready():
            draft_visible()
            if inventory(github, release_id, manifest, directory, pending=True): raise ReadbackPending('draft assets are not all visible yet')
        readback(github, draft_ready)
        receipt.write('draft_ready', release_id=release_id, immutability=immutable_setting(github))
        receipt.write('publication_requested', release_id=release_id)
        try:
            github.request(f'/releases/{release_id}', {'draft': False, 'prerelease': manifest['channel'] == 'preview',
                           'make_latest': 'false' if manifest['channel'] == 'preview' else 'true'}, 'PATCH')
        except (ValueError, OSError):
            # A lost PATCH response is not evidence that publication failed.
            # Only GET readback follows; this PATCH is never issued twice.
            receipt.write('publication_response_unconfirmed', release_id=release_id)
        result = verify_publication(github, directory, manifest, release_id, receipt)
        print(json.dumps({'published': result['html_url'], 'immutable': True, 'versionCode': metadata['versionCode'],
                          'source': manifest['source'], 'receipt': str(receipt.path)}))
    except OSError:
        if receipt.value['phase'].startswith(('publication', 'published')):
            raise PublicationUnconfirmed('publication may already be complete; receipt I/O failed, inspect the release before retrying') from None
        raise
    finally: receipt.summary()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('prepare', 'publish', 'inspect', 'resume'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--run-id')
    parser.add_argument('--version')
    parser.add_argument('--channel', choices=('stable', 'preview'))
    parser.add_argument('--release-id', type=int)
    parser.add_argument('--inspect-existing', action='store_true', help='read-only preparation for occupied versions')
    parser.add_argument('--confirm-manifest', help='exact SHA-256 from a reviewed inspect report; resume only')
    args = parser.parse_args()
    github = GitHub(os.environ.get('GITHUB_TOKEN', ''), read_only=args.operation in ('prepare','inspect'))
    if args.operation == 'prepare':
        require(all((args.run_id, args.version, args.channel)), 'prepare requires run ID, version and channel')
        require(args.confirm_manifest is None, 'confirmation is only used by resume')
        prepare(github, args.run_id, args.version, args.channel, args.output, args.release_id, args.inspect_existing)
    elif args.operation == 'inspect':
        require(args.confirm_manifest is None, 'inspect is read-only and takes no confirmation')
        inspect(github, args.output, args.release_id)
    elif args.operation == 'resume':
        require(args.release_id is not None and args.confirm_manifest is not None, 'resume requires release ID and inspected manifest SHA-256')
        publish(github, args.output, args.release_id, args.confirm_manifest)
    else:
        require(args.release_id is None and args.confirm_manifest is None, 'use explicit resume for an existing draft')
        require(os.environ.get('GITHUB_RUN_ATTEMPT', '1') == '1', 'do not rerun a publication job; inspect and explicitly resume its draft instead')
        publish(github, args.output)


if __name__ == '__main__':
    try:
        main()
    except PublicationUnconfirmed as error:
        raise SystemExit('release requires read-only inspection: ' + str(error))
    except (ValueError, KeyError, OSError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        raise SystemExit('release failed: ' + str(error))
