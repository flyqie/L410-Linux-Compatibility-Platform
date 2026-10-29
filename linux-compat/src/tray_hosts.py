"""Allocate invisible Harmony application contexts for Linux SNI items.

The Linux application keeps speaking StatusNotifierItem/DBusMenu.  A host is
only the native StatusBarExtensionKit owner required by HarmonyOS; it has no
home-screen ability and does not participate in AnCo window lifetime.
"""
import json
import os
import re
import subprocess
import time
from pathlib import Path

from desktop_entries import scan
import entry_builds

BASE = Path('/data/vendor/l410-linux')
ROOT = BASE / 'rootfs-bookworm'
STATE = BASE / 'config/tray-hosts.json'
CATALOG = ROOT / 'opt/l410/anco-host-record/catalog-window-capabilities-r2/packages.json'
BRIDGE_STATE = ROOT / 'opt/l410/anco-host-record/catalog-resource-icons-r136/tray-bridge/tray-hosts.json'
HOST_DIR = BASE / 'tray-hosts'


def read():
    return json.loads(STATE.read_text()) if STATE.exists() else {}


def save(value):
    candidate = STATE.with_suffix('.new')
    candidate.write_text(json.dumps(value, ensure_ascii=False))
    os.chmod(candidate, 0o600)
    candidate.replace(STATE)
    # The SNI watcher runs inside the Linux mount namespace, where the host
    # /data/vendor tree is intentionally absent.  Publish the same internal
    # identity map beside its existing tray snapshot so it can start the
    # original native TrayServiceAbility without exposing host configuration.
    BRIDGE_STATE.parent.mkdir(parents=True, exist_ok=True)
    bridge_candidate = BRIDGE_STATE.with_suffix('.new')
    bridge_candidate.write_text(json.dumps(value, ensure_ascii=False))
    os.chmod(bridge_candidate, 0o644)
    bridge_candidate.replace(BRIDGE_STATE)


def installed(bundle):
    # BMS can retain /data/app bundle files after uninstall on this image, so
    # filesystem presence is not an installation check.  Starting the hidden
    # service is also the intended action when an active SNI is discovered.
    try:
        result = subprocess.run(
            ['/system/bin/aa', 'start', '-b', bundle,
             '-a', 'TrayServiceAbility', '-m', 'entry'],
            capture_output=True, text=True, timeout=8,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    output = (result.stdout or '') + (result.stderr or '')
    return 'start ability successfully.' in output


def queue(semantic_bundle):
    if not re.fullmatch(r'org\.l410\.linux\.[a-z0-9_.]+', semantic_bundle or ''):
        raise ValueError('托盘应用身份无效')
    catalog = json.loads(CATALOG.read_text()) if CATALOG.exists() else {}
    record = catalog.get(semantic_bundle)
    desktop_id = record.get('desktop', '') if isinstance(record, dict) else ''
    app = next((item for item in scan(ROOT) if item['id'] == desktop_id), None)
    if not app or not app['supported']:
        raise ValueError('托盘应用没有有效 desktop 身份')
    tray_bundle = entry_builds.formal_tray_bundle(desktop_id)
    state = read()
    item = state.setdefault(semantic_bundle, {})
    if installed(tray_bundle):
        next_state, error = 'ready', ''
    elif item.get('bundle') == tray_bundle and item.get('state') in ('building', 'error'):
        # Keep an in-flight build stable and retain one actionable failure.
        # A later package reinstall/removal or explicit state reset can queue
        # it again without an unbounded rebuild loop on every SNI refresh.
        next_state, error = item['state'], item.get('error', '')
    else:
        next_state, error = 'queued', ''
    item.update(
        semanticBundle=semantic_bundle, desktop=desktop_id, bundle=tray_bundle,
        targetBundle=semantic_bundle, targetAbility=semantic_bundle + '.Main',
        name=app['name'][:128], state=next_state, error=error,
        updated=time.time(),
    )
    save(state)
    return item


def claim():
    state = read()
    for semantic_bundle, item in state.items():
        if item.get('state') != 'queued':
            continue
        app = next((candidate for candidate in scan(ROOT)
                    if candidate['id'] == item.get('desktop')), None)
        if not app:
            item.update(state='error', error='应用已卸载', updated=time.time())
            save(state)
            continue
        item.update(state='building', error='', updated=time.time())
        save(state)
        return dict(item, id=item['desktop'], icon=entry_builds.icon_for(app), trayOnly=True)
    return None


def finish(semantic_bundle, hap, error=''):
    state = read()
    item = state.get(semantic_bundle)
    if not item:
        raise ValueError('托盘宿主构建记录丢失')
    if error:
        item.update(state='error', error=str(error)[:1500], updated=time.time())
    else:
        HOST_DIR.mkdir(mode=0o700, parents=True, exist_ok=True)
        target = HOST_DIR / (item['bundle'] + '.hap')
        target.write_bytes(Path(hap).read_bytes())
        os.chmod(target, 0o600)
        item.update(state='ready', error='', hap=str(target), updated=time.time())
    save(state)
    return item
