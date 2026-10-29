"""Prepare persistent Linux package identity and original AnCo metadata on L410.

Only reserves Linux catalog identities. Publication/acceptance is separate.
Run with external rootfs Python on the host, with anco_metadata.py alongside.
"""
import argparse
import configparser
import fcntl
import json
import os
from pathlib import Path
import re
import shlex
import sqlite3
import time
from anco_metadata import encode
from rootfs_paths import rootfs_executable

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--rootfs', type=Path, required=True)
p.add_argument('--catalog', type=Path, required=True)
p.add_argument('--desktop', required=True)
p.add_argument('--package', required=True)
p.add_argument('--bundle', required=True)
a = p.parse_args()
if not re.fullmatch(r'org\.l410\.linux\.[a-z][a-z0-9_.]*', a.bundle):
    raise SystemExit('platform bundle namespace required')
if Path(a.desktop).name != a.desktop or not a.desktop.endswith('.desktop'):
    raise SystemExit('desktop basename required')
root = a.rootfs.resolve(strict=True)
desktop = root / 'usr/share/applications' / a.desktop
parser = configparser.ConfigParser(interpolation=None, strict=False)
parser.read(desktop, encoding='utf-8')
entry = parser['Desktop Entry']
if entry.get('Type') != 'Application' or entry.getboolean('Hidden', False):
    raise SystemExit('not an enabled application')
if entry.getboolean('Terminal', False) or entry.getboolean('DBusActivatable', False):
    raise SystemExit('terminal/DBus activation requires its own semantic binding')
argv = []
for word in shlex.split(entry['Exec']):
    if word in ('%f', '%F', '%u', '%U'):
        continue  # Ordinary no-document launch only.
    if '%' in word:
        raise SystemExit('additional Exec field expansion not wired here')
    argv.append(word)
if not argv:
    raise SystemExit('empty Exec')
if not argv[0].startswith('/'):
    if '/' in argv[0]:
        raise SystemExit('relative executable path')
    argv[0] = next(('/' + d + '/' + argv[0] for d in
        ('usr/local/bin', 'usr/bin', 'bin') if rootfs_executable(root,'/' + d + '/' + argv[0]) is not None), '')
if not argv[0] or rootfs_executable(root,argv[0]) is None:
    raise SystemExit('executable unavailable')
cwd = entry.get('Path', '/home/linux')
if not cwd.startswith('/') or '..' in Path(cwd).parts or not (root / cwd[1:]).is_dir():
    raise SystemExit('working directory unavailable')

# Read package manager records, not a guessed executable version string.
package = None
for paragraph in (root / 'var/lib/dpkg/status').read_text().split('\n\n'):
    fields = dict(line.split(': ', 1) for line in paragraph.splitlines()
                  if line and not line[0].isspace() and ': ' in line)
    if fields.get('Package') == a.package:
        package = fields
        break
if not package or package.get('Status') != 'install ok installed' or package.get('Architecture') != 'arm64':
    raise SystemExit('installed ARM64 package required')

# Existing native BMS entries share the guest appId namespace. Read only.
db = sqlite3.connect('file:/data/service/el1/public/bundle_framework_broker/'
                     'bundle_framework_broker.db?mode=ro', uri=True)
host_ids = {}
for key, value in db.execute('SELECT KEY, VALUE FROM installed_bundle'):
    info = json.loads(value)['applicationInfo']
    host_ids[info['bundleName']] = info['uid']
db.close()
a.catalog.mkdir(parents=True, exist_ok=True)
with (a.catalog / 'catalog.lock').open('a') as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    path = a.catalog / 'packages.json'
    records = json.loads(path.read_text()) if path.exists() else {}
    previous = records.get(a.bundle)
    if a.bundle in host_ids and previous is None:
        raise SystemExit('existing host bundle is not owned by this catalog')
    if previous and (previous['package'] != a.package or previous['desktop'] != a.desktop):
        raise SystemExit('persistent identity belongs to another package/entry')
    used = {r['app_id'] for r in records.values()} | set(host_ids.values())
    app_id = previous['app_id'] if previous else next((i for i in range(10000, 20000) if i not in used), None)
    if app_id is None:
        raise SystemExit('appId namespace exhausted')
    if any(i == app_id and b != a.bundle for b, i in host_ids.items()):
        raise SystemExit('appId occupied by another host entry')
    now = time.time_ns() // 1000000
    changed = previous is None or previous['version'] != package['Version']
    record = dict(bundle=a.bundle, ability=a.bundle + '.Main', app_id=app_id,
        guest_user=0, privacy=False, label=entry.get('Name[zh_CN]', entry['Name']),
        version=package['Version'], version_code=(previous['version_code'] + int(changed)) if previous else 1,
        install_time_ms=previous['install_time_ms'] if previous else now,
        update_time_ms=now if changed else previous['update_time_ms'],
        # Original SupportWindowMode values: FULLSCREEN=0, SPLIT=1,
        # FLOATING=2, MIDSCENE=3.  An ordinary Linux desktop GUI is backed by
        # the existing full-screen/freeform Task paths; split and middle-scene
        # Task semantics are not advertised until they are connected.
        code_path=argv[0], window_modes=[0, 2], orientation=0,
        package=a.package, desktop=a.desktop, argv=argv, cwd=cwd)
    # These times describe enrollment/update in this compatibility catalog.
    payload = encode(record)
    records[a.bundle] = record
    temporary = a.catalog / 'packages.json.new'
    with temporary.open('w') as stream:
        json.dump(records, stream, ensure_ascii=False, indent=2)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)
    metadata = a.catalog / (a.bundle + '.json')
    metadata.write_bytes(payload)
    print(json.dumps({'bundle': a.bundle, 'app_id': app_id,
        'metadata': str(metadata), 'registered': False, 'argv': argv}, ensure_ascii=False))
