#!/usr/bin/env python3
"""Create reproducible bounded long-domain inputs for the RSS benchmark."""
import argparse
import gzip
import hashlib
import json
from pathlib import Path


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(65536), b''):
            value.update(chunk)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=('near-limit', 'duplicates'), required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--gzip', action='store_true')
    parser.add_argument('--mib-per-source', type=int, default=60)
    args = parser.parse_args()
    assert 1 <= args.mib_per_source <= 60
    args.output.mkdir(mode=0o700, parents=True, exist_ok=False)
    inputs, rows, sequence = [], [], 0
    for number in range(4 if args.case == 'near-limit' else 1):
        name = f'{number:02d}.txt' + ('.gz' if args.gzip else '')
        path = args.output/name
        url = f'https://memory-benchmark.invalid/{number}'
        decoded, lines = 0, 0
        with path.open('wb') as raw:
            target = gzip.GzipFile(filename='', fileobj=raw, mode='wb', mtime=0, compresslevel=3) if args.gzip else raw
            try:
                while True:
                    value = sequence if args.case == 'near-limit' else sequence % 512
                    host = f'r{value:08d}.' + '.'.join(['a'*60]*3+['b'*30]) + '.example.test'
                    line = (('@@' if value % 97 == 0 else '') +
                            (host if value % 17 == 0 else '||'+host+'^')+'\n').encode()
                    if decoded+len(line) > args.mib_per_source*1024*1024:
                        break
                    target.write(line)
                    decoded += len(line); lines += 1; sequence += 1
            finally:
                if target is not raw:
                    target.close()
        path.chmod(0o600)
        rows.append(f'auto\t{name}\t{url}\n')
        inputs.append({'name': f'{args.case}-{number}', 'file': name, 'url': url, 'format': 'auto',
                       'bytes': path.stat().st_size, 'lines': lines, 'decompressed_bytes': decoded,
                       'sha256': digest(path)})
    (args.output/'sources.tsv').write_text(''.join(rows))
    manifest = {'schema': 1, 'case': args.case, 'compressed': args.gzip,
                'raw_bytes': sum(item['bytes'] for item in inputs),
                'raw_lines': sum(item['lines'] for item in inputs), 'inputs': inputs}
    (args.output/'benchmark-inputs.json').write_text(json.dumps(manifest, indent=2)+'\n')
    print(json.dumps(manifest))


if __name__ == '__main__':
    main()
