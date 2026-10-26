#!/usr/bin/env python3
"""Compare identical frozen workloads, complete artifacts and measured RSS."""
import argparse
import hashlib
import json
from pathlib import Path


def digest(path):
    value = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(65536), b''):
            value.update(block)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    args = parser.parse_args()
    before, after = [json.loads((path/'report.json').read_text()) for path in (args.before, args.after)]
    assert before.get('complete') and after.get('complete')
    assert before['mode'] == after['mode'] == 'cached_sync_replay_not_fresh_network'
    assert before['input_manifest_sha256'] == after['input_manifest_sha256']
    assert before['canonical'] == after['canonical'], 'complete canonical sets differ'
    for name in ('hosts_rules.gz', 'adguard_rules.gz'):
        assert digest(args.before/name) == digest(args.after/name), f'gzip bytes differ: {name}'
    counts = []
    for path in (args.before, args.after):
        rows = dict(line.split('=', 1) for line in (path/'rule_stats.conf').read_text().splitlines())
        counts.append({key: int(rows[key]) for key in ('black', 'white')})
    assert counts[0] == counts[1], 'engine typed counts differ'
    print(json.dumps({'equal': True, 'complete_gzip_bytes_equal': True, 'counts': counts[0],
                      'before': before['phases'], 'after': after['phases']}))


if __name__ == '__main__':
    main()
