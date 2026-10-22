#!/usr/bin/env python3
"""Publish a new tag once from a completed main CI artifact; never replace assets.

prepare is read-only on GitHub. publish leaves a draft on upload failure for
inspection; it never deletes a release, moves a tag, or resumes an occupied tag.
"""
import argparse
import base64
import hashlib
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

from validate_listing import properties, unique_object, validate_manifest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests'))
from validate_manager_update import validate_manager_update

REPOSITORY = 'fengwenhua/AD_Killer_By_NFQUEUE'
API = 'https://api.github.com/repos/' + REPOSITORY
REPO = 'https://github.com/' + REPOSITORY
REQUIRED_JOBS = {'host-tests', 'webui-layout', 'android-build'}
MAX_DOWNLOAD = 128 * 1024 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args):
        return None


class GitHub:
    def __init__(self, token):
        require(bool(token), 'GITHUB_TOKEN is required')
        self.headers = {'Authorization': 'Bearer ' + token, 'Accept': 'application/vnd.github+json',
                        'X-GitHub-Api-Version': '2026-03-10', 'User-Agent': 'AD-Killer-release'}

    def request(self, path, data=None, method='GET'):
        require(path.startswith('/') and not path.startswith('//'), 'invalid API path')
        headers = dict(self.headers)
        if data is not None:
            headers['Content-Type'] = 'application/json'
            data = json.dumps(data).encode()
        try:
            with urllib.request.build_opener(NoRedirect).open(urllib.request.Request(
                    API + path, data=data, headers=headers, method=method), timeout=60) as response:
                body = response.read(MAX_DOWNLOAD + 1)
                require(len(body) <= MAX_DOWNLOAD, 'API response too large')
                return json.loads(body) if body else None
        except urllib.error.HTTPError as error:
            if method == 'GET' and error.code == 404:
                return None
            raise ValueError(f'GitHub API {method} failed (HTTP {error.code})') from None

    def pages(self, path, key=None):
        for page in range(1, 101):
            result = self.request(path + ('&' if '?' in path else '?') + f'per_page=100&page={page}')
            require(result is not None, 'missing paginated API response')
            values = result[key] if key else result
            yield from values
            if len(values) < 100:
                return
        raise ValueError('API pagination exceeded safety limit')

    def artifact(self, artifact_id):
        try:
            with urllib.request.build_opener(NoRedirect).open(urllib.request.Request(
                    API + f'/actions/artifacts/{artifact_id}/zip', headers=self.headers), timeout=30) as response:
                body = response.read(MAX_DOWNLOAD + 1)
        except urllib.error.HTTPError as error:
            require(error.code == 302, f'artifact download HTTP {error.code}')
            return self.public_download(error.headers.get('Location', ''))
        require(len(body) <= MAX_DOWNLOAD, 'artifact too large')
        return body

    @staticmethod
    def public_download(url):
        require(url.startswith('https://'), 'download must use HTTPS')
        # Deliberately omit Authorization on redirects and public download hosts.
        with urllib.request.urlopen(urllib.request.Request(url, headers={'User-Agent': 'AD-Killer-release'}), timeout=60) as response:
            require(response.url.startswith('https://'), 'insecure download redirect')
            body = response.read(MAX_DOWNLOAD + 1)
        require(len(body) <= MAX_DOWNLOAD, 'download exceeds size limit')
        return body

    def source_file(self, source, path):
        result = self.request('/contents/' + path + '?ref=' + source)
        require(result is not None and result.get('encoding') == 'base64', 'missing CI source file: ' + path)
        return base64.b64decode(result['content'], validate=False)

    def upload(self, release_id, name, body):
        url = f'https://uploads.github.com/repos/{REPOSITORY}/releases/{release_id}/assets?name=' + urllib.parse.quote(name, safe='')
        headers = {**self.headers, 'Content-Type': 'application/octet-stream'}
        with urllib.request.build_opener(NoRedirect).open(urllib.request.Request(url, data=body,
                headers=headers, method='POST'), timeout=60) as response:
            return json.load(response)


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


def check_monotonic(github, metadata):
    for release in github.pages('/releases'):
        if release['draft']:
            continue
        assets = [asset for asset in release['assets'] if asset['name'] == 'update.json']
        if not assets:
            # Historical releases without manager metadata still cannot be reused
            # or semantically downgraded. Current releases always include JSON.
            old = release['tag_name']
            require(re.fullmatch(r'v[0-9]+\.[0-9]+(?:\.[0-9]+)?', old) is not None, 'cannot audit historical release ' + old)
            parts = lambda value: tuple((list(map(int, value[1:].split('.'))) + [0])[:3])
            require(parts(metadata['version']) > parts(old), 'version must exceed all historical releases')
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
    metadata = validate_manager_update(directory / ('AD_Killer_By_NFQUEUE-' + manifest['version'] + '.zip'))
    require(metadata['releaseChannel'] == manifest['channel'], 'artifact channel mismatch')
    for name, digest in manifest['assets'].items():
        require(Path(name).name == name and name not in ('.', '..'), 'unsafe asset name')
        require(re.fullmatch(r'[0-9a-f]{64}', digest) is not None, 'invalid asset digest')
        require(hashlib.sha256((directory / name).read_bytes()).hexdigest() == digest, 'artifact changed: ' + name)
    return metadata


def prepare(github, run_id, version, channel, output):
    require(channel in ('stable', 'preview'), 'invalid release channel')
    check_unused(github, version)
    run = check_run(github, run_id)
    source = run['head_sha']
    props = properties(github.source_file(source, 'module/module.prop').decode())
    listing = json.loads(github.source_file(source, 'module.yaml'), object_pairs_hook=unique_object)
    validate_manifest(listing, props)
    require(props['version'] == version, 'requested version differs from green CI source')
    artifact_name = 'AD_Killer_By_NFQUEUE-module' + ('-preview' if channel == 'preview' else '')
    matches = [item for item in github.pages(f'/actions/runs/{run_id}/artifacts', 'artifacts')
               if item['name'] == artifact_name and not item['expired']]
    require(len(matches) == 1, 'missing or ambiguous CI artifact')
    artifact = matches[0]
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
    record = {'run_id': str(run_id), 'source': source, 'version': version, 'channel': channel,
              'artifact_id': artifact['id'], 'assets': {item: hashlib.sha256((output / item).read_bytes()).hexdigest() for item in sorted(names)}}
    metadata = validate_assets(output, record)
    require(metadata['versionCode'] == int(props['versionCode']) - (channel == 'preview'), 'CI package/source versionCode mismatch')
    require((output / 'changelog.md').read_bytes() == github.source_file(source, 'CHANGELOG.md'), 'CI release notes/source mismatch')
    check_monotonic(github, metadata)
    (output / 'manifest.json').write_text(json.dumps(record, indent=2) + '\n')
    print(json.dumps({'prepared': str(output), **record}))


def publish(github, directory):
    require(os.environ.get('KERNELSU_IMMUTABLE_RELEASES') == 'enabled', 'owner must enable release immutability and acknowledge it in Actions variables first')
    manifest = json.loads((directory / 'manifest.json').read_text(), object_pairs_hook=unique_object)
    require(set(manifest['assets']) == {'AD_Killer_By_NFQUEUE-' + manifest['version'] + '.zip',
            'AD_Killer_By_NFQUEUE-' + manifest['version'] + '.zip.sha256', 'update.json', 'changelog.md'}, 'unexpected release assets')
    metadata = validate_assets(directory, manifest)
    check_run(github, manifest['run_id'], manifest['source'])
    check_unused(github, manifest['version'])
    check_monotonic(github, metadata)
    preview = manifest['channel'] == 'preview'
    draft = github.request('/releases', {'tag_name': manifest['version'], 'target_commitish': manifest['source'],
        'name': manifest['version'] + (' Preview' if preview else ''), 'body': (directory / 'changelog.md').read_text(),
        'draft': True, 'prerelease': preview, 'make_latest': 'false'}, 'POST')
    require(draft.get('draft') is True and draft.get('tag_name') == manifest['version'], 'unexpected draft response')
    release_id = draft['id']
    for name, digest in manifest['assets'].items():
        body = (directory / name).read_bytes()
        asset = github.upload(release_id, name, body)
        require(asset.get('digest') == 'sha256:' + digest and asset.get('size') == len(body), 'uploaded asset integrity mismatch')
    current = github.request(f'/releases/{release_id}')
    require({asset['name']: asset.get('digest') for asset in current['assets']} ==
            {name: 'sha256:' + digest for name, digest in manifest['assets'].items()}, 'draft assets changed; refusing publication')
    result = github.request(f'/releases/{release_id}', {'draft': False, 'prerelease': preview,
                            'make_latest': 'false' if preview else 'true'}, 'PATCH')
    require(not result.get('draft') and result.get('prerelease') is preview, 'unexpected publication state')
    immutable = result.get('immutable') is True
    for _ in range(3):
        if immutable:
            break
        time.sleep(1)
        immutable = github.request(f'/releases/{release_id}').get('immutable') is True
    require(immutable, 'release was published but is NOT immutable; stop and fix repository settings, do not replace its files')
    ref = github.request('/git/ref/tags/' + manifest['version'])
    require(ref is not None and ref['object']['type'] == 'commit' and ref['object']['sha'] == manifest['source'], 'published tag/source mismatch')
    if not preview:
        require(github.request('/releases/latest')['id'] == release_id, 'stable release is not Latest')
    print(json.dumps({'published': result['html_url'], 'immutable': True, 'versionCode': metadata['versionCode'], 'source': manifest['source']}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('operation', choices=('prepare', 'publish'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--run-id')
    parser.add_argument('--version')
    parser.add_argument('--channel', choices=('stable', 'preview'))
    args = parser.parse_args()
    github = GitHub(os.environ.get('GITHUB_TOKEN', ''))
    if args.operation == 'prepare':
        require(all((args.run_id, args.version, args.channel)), 'prepare requires run ID, version and channel')
        prepare(github, args.run_id, args.version, args.channel, args.output)
    else:
        publish(github, args.output)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, KeyError, OSError, zipfile.BadZipFile, subprocess.CalledProcessError) as error:
        raise SystemExit('release failed: ' + str(error))
