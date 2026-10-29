#!/usr/bin/env python3
"""Project the persistent AnCo desktop catalog into the resource catalog.

The virtual package owns the menu tile and Task identity.  The runtime catalog
uses the same package identity and command and advertises the original AnCo
ResourceManager icon ID so the launcher, title bar and Dock all ask SA 66894
for the Linux desktop icon.
"""
import argparse
import fcntl
import json
import os
from pathlib import Path
import re

from anco_runtime_metadata import encode


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source', type=Path, required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()

source = args.source.resolve(strict=True)
records = json.loads((source / 'packages.json').read_text(encoding='utf-8'))
if not isinstance(records, dict):
    raise SystemExit('catalog must be an object')
for bundle, record in records.items():
    if (not re.fullmatch(r'org\.l410\.linux\.[a-z0-9_.]+', bundle)
            or not isinstance(record, dict) or record.get('bundle') != bundle
            or record.get('ability') != bundle + '.Main'):
        raise SystemExit('invalid catalog record: ' + str(bundle))

output = args.output
output.mkdir(parents=True, exist_ok=True)
with (output / 'catalog.lock').open('a') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    expected = set()
    for bundle, record in sorted(records.items()):
        target = output / (bundle + '.json')
        temporary = target.with_name(target.name + '.new')
        temporary.write_bytes(encode(record))
        os.chmod(temporary, 0o644)
        os.replace(temporary, target)
        expected.add(target.name)
    for stale in output.glob('org.l410.linux.*.json'):
        if stale.name not in expected:
            stale.unlink()
    packages = output / 'packages.json'
    temporary = packages.with_name(packages.name + '.new')
    with temporary.open('w', encoding='utf-8') as stream:
        json.dump(records, stream, ensure_ascii=False, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    os.chmod(temporary, 0o644)
    os.replace(temporary, packages)

print(json.dumps({'count': len(records), 'output': str(output)}, ensure_ascii=False))
