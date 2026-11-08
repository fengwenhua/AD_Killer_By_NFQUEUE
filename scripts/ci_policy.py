#!/usr/bin/env python3
"""Require every independent host CI shard before reporting aggregate success."""
import json
import os

HOST_SHARDS = frozenset({'host-unit', 'host-download', 'host-sanitize', 'engine-nft', 'engine-legacy'})
REQUIRED_JOBS = HOST_SHARDS | {'host-tests', 'webui-layout', 'android-build'}


def verify_host_results(results):
    if type(results) is not dict or set(results) != HOST_SHARDS:
        raise ValueError('host CI shard inventory is incomplete or unexpected')
    failed = [name for name in sorted(HOST_SHARDS)
              if type(results[name]) is not dict or results[name].get('result') != 'success']
    if failed:
        raise ValueError('host CI shards did not all succeed: ' + ', '.join(failed))


def main():
    try:
        verify_host_results(json.loads(os.environ.get('CI_NEEDS_JSON', '')))
    except (ValueError, TypeError):
        raise SystemExit('host CI gate refused: every declared shard must complete successfully') from None
    print('All five host CI shards completed successfully')


if __name__ == '__main__':
    main()
