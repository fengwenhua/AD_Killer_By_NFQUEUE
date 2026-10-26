"""Owned PackageManager boundary for disposable Linux CLI/engine tests."""
import os
from pathlib import Path
import sys
import tempfile


class PackageManager:
    def __init__(self):
        self.directory = tempfile.TemporaryDirectory(prefix='adk-package-manager-')
        self.root = Path(self.directory.name)
        self.previous_path = os.environ['PATH']
        script = f'''#!{sys.executable}
from pathlib import Path
import sys,time
root = Path({str(self.root)!r})
expected = ['package', 'list', 'packages', '-U', '--user', 'all']
if Path(sys.argv[0]).name == 'pm': expected = expected[1:]
if sys.argv[1:] != expected: sys.exit(64)
with (root / 'queries').open('a') as out: out.write('query\\n')
while (root / 'stall').exists(): time.sleep(.02)
if (root / 'error').exists():
    print('package manager unavailable', file=sys.stderr)
    sys.exit(1)
sys.stdout.write('package:android uid:1000\\npackage:vendor uid:1000\\n' + (root / 'catalog').read_text())
'''
        for name in ('cmd', 'pm'):
            path = self.root / name
            path.write_text(script)
            path.chmod(0o700)
        self.set('package:com.example.system uid:1000\n')
        os.environ['PATH'] = str(self.root) + os.pathsep + self.previous_path

    def set(self, text):
        temporary = self.root / 'catalog.new'
        temporary.write_text(text)
        temporary.replace(self.root / 'catalog')

    def count(self):
        path = self.root / 'queries'
        return len(path.read_text().splitlines()) if path.exists() else 0

    def close(self):
        os.environ['PATH'] = self.previous_path
        self.directory.cleanup()
