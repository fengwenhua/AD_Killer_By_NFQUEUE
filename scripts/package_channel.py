#!/usr/bin/env python3
"""Stamp only the staged package; module/module.prop remains the version source."""
from pathlib import Path
import sys


def stamp(path, channel, code):
    if channel not in ('stable', 'preview') or not code.isdigit() or int(code) < 1:
        raise ValueError('invalid package channel/versionCode')
    values = {'releaseChannel': channel, 'versionCode': code}
    lines = path.read_text().splitlines()
    for key, value in values.items():
        lines = [line for line in lines if not line.startswith(key + '=')]
        lines.append(key + '=' + value)
    path.write_text('\n'.join(lines) + '\n')


if __name__ == '__main__':
    stamp(Path(sys.argv[1]), sys.argv[2], sys.argv[3])
