"""Explicit user-selected AnCo launcher aliases, claimed by device or workstation."""
import json
import re
import threading
import time
from pathlib import Path

from desktop_entries import scan
from desktop_icons import icon_asset

BASE = Path('/data/vendor/l410-linux')
BUILDER_PUBLIC = Path('/vendor/l410-linux/entry-builder')
ROOT = BASE / 'rootfs-bookworm'
STATE = BASE / 'config/entry-builds.json'
CATALOG = ROOT / 'opt/l410/anco-host-record/catalog-window-capabilities-r2/packages.json'
lock = threading.Lock()
worker_seen = 0
worker_mode = 'workstation'


def formal_slug(desktop_id):
    """Return the stable Harmony launcher identity for one desktop file.

    The localized desktop Name is deliberately not part of the identity: it
    can change with locale or an application update.  The freedesktop desktop
    file ID is the package identity, while Name remains the visible label.
    """
    if not re.fullmatch(r'[A-Za-z0-9_.+-]{1,160}\.desktop', desktop_id):
        raise ValueError('desktop id 无效')
    slug = re.sub(r'[^a-z0-9]+', '_', desktop_id[:-8].lower()).strip('_')
    if not slug:
        raise ValueError('desktop id 不能生成入口身份')
    if slug[0].isdigit():
        slug = 'app_' + slug
    # Harmony bundle names are bounded.  Refuse an ambiguous truncation so two
    # different desktop IDs can never silently become the same application.
    if len(slug) > 80:
        raise ValueError('desktop id 过长，不能生成无歧义的正式入口身份')
    return slug


def formal_bundle(desktop_id):
    return 'com.l410.linux.app.' + formal_slug(desktop_id)


def formal_tray_bundle(desktop_id):
    slug = formal_slug(desktop_id)
    # ``tray_`` is part of the final Harmony bundle segment, whose accepted
    # length is 81 characters including its first letter.  Refuse an
    # ambiguous truncation rather than silently colliding long desktop IDs.
    if len(slug) > 76:
        raise ValueError('desktop id 过长，不能生成无歧义的托盘身份')
    return 'com.l410.linux.tray_' + slug


def device_template(bundle):
    return BUILDER_PUBLIC / 'templates' / bundle / 'modules.abc'


def read():
    return json.loads(STATE.read_text()) if STATE.exists() else {}


def save(value):
    candidate = STATE.with_suffix('.new')
    candidate.write_text(json.dumps(value, ensure_ascii=False))
    candidate.replace(STATE)


def anco_target(desktop_id):
    records = json.loads(CATALOG.read_text()) if CATALOG.exists() else {}
    matches = [record for record in records.values() if record.get('desktop') == desktop_id]
    if len(matches) != 1:
        raise ValueError('此应用尚未登记到原生 AnCo 目录')
    target = matches[0]
    bundle = target.get('bundle', '')
    ability = target.get('ability', '')
    if not re.fullmatch(r'org\.l410\.linux\.[a-z0-9_.]+', bundle):
        raise ValueError('AnCo bundle 无效')
    if not ability.startswith(bundle + '.') or not re.fullmatch(r'[A-Za-z0-9_.]+', ability):
        raise ValueError('AnCo ability 无效')
    return bundle, ability


def queue_app(desktop_id, registry, force=False):
    app = next((item for item in scan(ROOT) if item['id'] == desktop_id), None)
    if not app or not app['supported']:
        raise ValueError('应用不存在或尚不支持启动')
    target_bundle, target_ability = anco_target(desktop_id)
    with lock:
        state = read()
        asset = icon_asset(ROOT, app['icon'])
        stamp = asset['stamp'] if asset else None
        used = {item['bundle'] for key, item in registry.items() if key != desktop_id} | {
            item['bundle'] for key, item in state.items() if key != desktop_id and item.get('bundle')
        }
        bundle = formal_bundle(desktop_id)
        if bundle in used:
            raise ValueError('正式入口身份冲突')
        if desktop_id not in state:
            state[desktop_id] = {
                'id': desktop_id, 'bundle': bundle, 'name': app['name'][:128]
            }
        item = state[desktop_id]
        # A desktop file owns one stable, human-readable Harmony identity.
        # Numeric slot identities were an implementation leak and must never be
        # retained or reallocated, even when an older job was already ready.
        identity_changed = item.get('bundle') != bundle
        if identity_changed:
            item['bundle'] = bundle
        changed = (
            item.get('name') != app['name'][:128]
            or item.get('iconStamp') != stamp
            or item.get('targetBundle') != target_bundle
            or item.get('targetAbility') != target_ability
        )
        item.update(
            name=app['name'][:128], pendingIconStamp=stamp,
            targetBundle=target_bundle, targetAbility=target_ability,
        )
        if force or changed or identity_changed or item.get('state') not in ('queued', 'building', 'ready'):
            item.update(state='queued', error='', updated=time.time())
        save(state)
        return {
            'queued': True, 'name': item['name'], 'bundle': item['bundle'],
            'output': '入口构建已排队；将直接启动原生 AnCo 应用', 'build': item['state'],
        }


def cancel(desktop_id):
    with lock:
        state = read()
        if desktop_id not in state:
            return None
        state[desktop_id].update(state='cancelled', updated=time.time())
        save(state)
        return {'enabled': False, 'name': state[desktop_id]['name']}


def remove(desktop_id):
    with lock:
        state = read()
        item = state.pop(desktop_id, None)
        if not item:
            return None
        save(state)
        (BASE / 'launchers' / (item['bundle'] + '.hap')).unlink(missing_ok=True)
        return item


def icon_for(app):
    asset = icon_asset(ROOT, app['icon'])
    if asset:
        asset.pop('stamp', None)
    return asset


def claim(mode='workstation'):
    global worker_seen, worker_mode
    with lock:
        worker_seen = time.time()
        worker_mode = mode
        state = read()
        changed = False
        for item in state.values():
            if item['state'] == 'queued' or (
                item['state'] == 'building' and time.time() - item['updated'] > 600
            ):
                has_device_template = device_template(item.get('bundle', '')).is_file()
                if mode == 'device' and not has_device_template:
                    continue
                if mode == 'workstation' and has_device_template:
                    continue
                app = next((candidate for candidate in scan(ROOT) if candidate['id'] == item['id']), None)
                if not app:
                    item.update(state='error', error='应用已卸载')
                    changed = True
                    continue
                target_bundle, target_ability = anco_target(item['id'])
                item.update(
                    state='building', updated=time.time(),
                    targetBundle=target_bundle, targetAbility=target_ability,
                )
                save(state)
                return {'job': dict(item, icon=icon_for(app))}
        if changed:
            save(state)
        return {'job': None}


def finish(desktop_id, error, install):
    with lock:
        state = read()
        item = state.get(desktop_id)
        if not item:
            raise ValueError('unknown build request')
        if item['state'] == 'cancelled':
            return {'state': 'cancelled'}
        if error:
            item.update(state='error', error=str(error)[:1500], updated=time.time())
        else:
            try:
                install(True, desktop_id)
                item.update(
                    state='ready', error='', updated=time.time(),
                    iconStamp=item.pop('pendingIconStamp', None),
                )
            except Exception as exc:
                item.update(state='error', error=str(exc)[:1500], updated=time.time())
        save(state)
        return {'state': item['state'], 'error': item.get('error', '')}


def status():
    items = list(read().values())
    online = time.time() - worker_seen < 30
    labels = {
        'queued': '等待构建', 'building': '正在构建', 'ready': '入口已生成',
        'error': '构建失败', 'cancelled': '已取消',
    }
    output = (
        ('设备构建器在线' if worker_mode == 'device' else '工作站签名助手在线')
        if online else '构建器离线；可运行 scripts/launcher-worker.py --watch'
    )
    output += '\n' + '\n'.join(
        item['name'] + '：' + labels.get(item['state'], item['state'])
        + (' · ' + item['error'] if item.get('error') else '') for item in items
    )
    return {'builds': items, 'workerOnline': online, 'workerMode': worker_mode, 'output': output}
