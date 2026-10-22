#!/usr/bin/env python3
"""Manager protocol fixtures; no phone/root/network and no forged installed version."""
import json
from pathlib import Path
import sys
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from package_module import write_checksum
from release_metadata import generate
from validate_manager_update import UPDATE_URL, validate_manager_update


def main():
    with tempfile.TemporaryDirectory(prefix='adk-manager-contract-') as directory:
        base = Path(directory)
        package = base / 'AD_Killer_By_NFQUEUE-v1.2.3.zip'
        notes = base / 'release.md'
        notes.write_text('# v1.2.3\n\n原生管理器更新日志。\n', encoding='utf-8')
        def fixture(code='12301', channel='stable', url=UPDATE_URL):
            with zipfile.ZipFile(package, 'w') as archive:
                archive.writestr('module.prop', f'id=AD_Killer_By_NFQUEUE\nversion=v1.2.3\nversionCode={code}\nreleaseChannel={channel}\nupdateJson={url}\n')
            write_checksum(package)
        def rejected():
            try:
                validate_manager_update(package)
            except (ValueError, KeyError):
                return
            raise AssertionError('invalid manager update contract was accepted')

        fixture()
        stable = generate(package, notes)
        assert validate_manager_update(package) == stable
        # Mirrors the manager's numeric decision, not WebUI semantic versioning.
        for installed, available in ((12201, True), (12300, True), (12301, False), (12401, False)):
            assert (stable['versionCode'] > installed) is available
        fixture('12300', 'preview')
        preview = generate(package, notes)
        assert validate_manager_update(package) == preview
        assert stable['versionCode'] > preview['versionCode']
        assert preview['versionCode'] < 12301, 'a preview must not upgrade an installed same-number stable'
        fixture()
        original = generate(package, notes)
        for key, invalid in (
            ('versionCode', '12301'), ('versionCode', True), ('versionCode', 2**31),
            ('versionCode', 12300), ('version', 'v1.2.4'),
            ('zipUrl', 'https://example.org/not-the-release.zip'),
            ('changelog', ''), ('changelog', UPDATE_URL),
            ('releaseChannel', 'preview'), ('module', 'other'),
            ('sha256', '0'*64), ('notes', 'outdated notes'),
        ):
            (base/'update.json').write_text(json.dumps({**original, key: invalid}))
            rejected()
        for key in ('version', 'versionCode', 'zipUrl', 'changelog'):
            (base/'update.json').write_text(json.dumps({k:v for k,v in original.items() if k!=key}))
            rejected()
        (base/'update.json').write_text(json.dumps(original)[:-1] + ',"versionCode":12301}')
        rejected()
        (base/'update.json').write_text('[]')
        rejected()
        for code in ('0', '-1', '2147483648', '１２３０１'):
            fixture(code)
            (base/'update.json').write_text(json.dumps(original))
            rejected()
            try:
                generate(package, notes)
            except ValueError:
                pass
            else:
                raise AssertionError('producer accepted an invalid manager versionCode')
        fixture('12301', 'stable', 'https://example.org/update.json')
        (base/'update.json').write_text(json.dumps(original))
        rejected()
        fixture()
        generate(package, notes)
        (base/'changelog.md').write_text('')
        rejected()
        generate(package, notes)
        package.write_bytes(package.read_bytes() + b'tampered')
        rejected()
    print('Native manager standard fields, numeric upgrades, channels, int32, asset identity, tampering and failure contracts passed')


if __name__ == '__main__':
    main()
