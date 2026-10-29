#!/usr/bin/python3
"""Bridge the standard Linux StatusNotifierItem protocol to a host snapshot.

Applications only see the KDE/freedesktop D-Bus contracts.  The HarmonyOS
launcher ability consumes the snapshot and translates it to the native status
bar API.  This process runs as the same uid and on the same private session bus
as the Linux applications; it never exposes another application-facing tray
protocol.
"""
import asyncio
import json
import os
import re
import sys
import time
from pathlib import Path

from dbus_next import Message, MessageType, Variant
from dbus_next.aio import MessageBus

WATCHER = 'org.kde.StatusNotifierWatcher'
WATCHER_PATH = '/StatusNotifierWatcher'
HOST_BASE = Path('/data/vendor/l410-linux')
ENTRY_BUILDS = HOST_BASE / 'config/entry-builds.json'
TRAY_HOSTS = HOST_BASE / 'config/tray-hosts.json'
WATCHER_XML = '''<node>
 <interface name="org.kde.StatusNotifierWatcher">
  <method name="RegisterStatusNotifierItem"><arg name="service" type="s" direction="in"/></method>
  <method name="RegisterStatusNotifierHost"><arg name="service" type="s" direction="in"/></method>
  <property name="RegisteredStatusNotifierItems" type="as" access="read"/>
  <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
  <property name="ProtocolVersion" type="i" access="read"/>
  <signal name="StatusNotifierItemRegistered"><arg type="s"/></signal>
  <signal name="StatusNotifierItemUnregistered"><arg type="s"/></signal>
  <signal name="StatusNotifierHostRegistered"/>
  <signal name="StatusNotifierHostUnregistered"/>
 </interface>
 <interface name="org.freedesktop.DBus.Properties">
  <method name="Get"><arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="v" direction="out"/></method>
  <method name="GetAll"><arg type="s" direction="in"/><arg type="a{sv}" direction="out"/></method>
 </interface>
 <interface name="org.freedesktop.DBus.Introspectable">
  <method name="Introspect"><arg type="s" direction="out"/></method>
 </interface>
</node>'''


def atomic_json(path, value):
    candidate = path.with_suffix(path.suffix + '.new')
    candidate.write_text(json.dumps(value, ensure_ascii=False, separators=(',', ':')))
    os.chmod(candidate, 0o600)
    candidate.replace(path)


def proc_bundle(pid):
    """Read the semantic identity inherited from the admitted app process."""
    for _ in range(32):
        try:
            raw = Path('/proc/%d/environ' % pid).read_bytes().split(b'\0')
            for entry in raw:
                if entry.startswith(b'L410_ANCO_BUNDLE='):
                    value = entry.split(b'=', 1)[1].decode('ascii')
                    if re.fullmatch(r'org\.l410\.linux\.[a-z0-9_.]+', value):
                        return value
            stat = Path('/proc/%d/stat' % pid).read_text()
            end = stat.rfind(')')
            pid = int(stat[end + 2:].split()[1])
            if pid <= 1:
                break
        except (OSError, ValueError, UnicodeError):
            break
    return ''


def plain(value, default=None):
    return value.value if isinstance(value, Variant) else (default if value is None else value)


def native_tray_bundle(semantic_bundle, directory=None):
    """Map the admitted Linux identity to its installed native alias.

    The mapping is produced by the existing desktop/AnCo entry builder.  Do
    not derive an identity from a process name: desktop IDs and native bundle
    names are stable package metadata and may intentionally differ.
    """
    sources = ([Path(directory) / 'tray-hosts.json'] if directory else [])
    sources.extend((TRAY_HOSTS, ENTRY_BUILDS))
    for source in sources:
        try:
            builds = json.loads(source.read_text())
        except (OSError, ValueError):
            continue
        if source.name == 'tray-hosts.json':
            record = builds.get(semantic_bundle, {})
            matches = [record.get('bundle', '')] if record.get('state') == 'ready' else []
        else:
            matches = [record.get('bundle', '') for record in builds.values()
                       if isinstance(record, dict) and
                       record.get('targetBundle') == semantic_bundle]
        matches = [value for value in matches if
                   re.fullmatch(r'com\.l410\.linux\.(?:app\.)?[a-z][a-z0-9_]{0,80}', value)]
        if len(set(matches)) == 1:
            return matches[0]
    return ''


class Bridge:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.state_path = self.directory / 'tray-sni.json'
        self.action_path = self.directory / 'tray-sni-action.json'
        self.bus = None
        self.items = {}
        self.hosts = set()
        self.generation = 0
        self.last_action = 0
        self.native_services = set()

    async def set_native_service(self, semantic_bundle, enabled):
        native_bundle = native_tray_bundle(semantic_bundle, self.directory)
        if not native_bundle:
            print('SNI_NATIVE_SERVICE_UNMAPPED bundle=%s' % semantic_bundle, flush=True)
            return
        key = (semantic_bundle, native_bundle)
        if not enabled:
            # TrayServiceAbility observes the now-empty existing SNI snapshot
            # and stops itself. Keep the service private instead of exporting
            # it merely so an external process can issue stop-service.
            self.native_services.discard(key)
            print('SNI_NATIVE_SERVICE_RELEASE semantic=%s native=%s' %
                  (semantic_bundle, native_bundle), flush=True)
            return
        if enabled and key in self.native_services:
            return
        command = 'start'
        try:
            process = await asyncio.create_subprocess_exec(
                '/system/bin/aa', command, '-b', native_bundle,
                '-a', 'TrayServiceAbility', '-m', 'entry',
                stdout=asyncio.subprocess.PIPE, stderr=asyncio.subprocess.STDOUT)
            output, _ = await asyncio.wait_for(process.communicate(), timeout=8)
            message = output.decode('utf-8', 'replace').strip().replace('\n', ' ')[:512]
            if process.returncode == 0:
                self.native_services.add(key)
                print('SNI_NATIVE_SERVICE action=%s semantic=%s native=%s result=%s' %
                      (command, semantic_bundle, native_bundle, message), flush=True)
            else:
                print('SNI_NATIVE_SERVICE_ERROR action=%s semantic=%s native=%s rc=%d result=%s' %
                      (command, semantic_bundle, native_bundle, process.returncode, message), flush=True)
        except Exception as error:
            print('SNI_NATIVE_SERVICE_ERROR action=%s semantic=%s native=%s error=%r' %
                  (command, semantic_bundle, native_bundle, error), flush=True)

    async def dbus(self, member, signature='', body=None):
        return await self.bus.call(Message(destination='org.freedesktop.DBus',
            path='/org/freedesktop/DBus', interface='org.freedesktop.DBus',
            member=member, signature=signature, body=body or []))

    async def owner_pid(self, service):
        reply = await self.dbus('GetConnectionUnixProcessID', 's', [service])
        return int(reply.body[0]) if reply.message_type == MessageType.METHOD_RETURN else 0

    def watcher_properties(self):
        return {
            'RegisteredStatusNotifierItems': Variant('as', sorted(self.items)),
            'IsStatusNotifierHostRegistered': Variant('b', True),
            'ProtocolVersion': Variant('i', 0),
        }

    def reply_handler(self, message):
        if message.message_type == MessageType.SIGNAL:
            if message.interface == 'org.freedesktop.DBus' and message.member == 'NameOwnerChanged':
                name, old, new = message.body
                if old and not new:
                    asyncio.create_task(self.remove_service(name))
            elif message.sender and message.member in (
                    'NewTitle', 'NewIcon', 'NewAttentionIcon', 'NewOverlayIcon',
                    'NewToolTip', 'NewStatus', 'NewMenu', 'LayoutUpdated',
                    'ItemsPropertiesUpdated'):
                for key, item in list(self.items.items()):
                    if item.get('owner') in (message.sender, message.destination):
                        asyncio.create_task(self.refresh(key))
            return None
        if message.message_type != MessageType.METHOD_CALL or message.path != WATCHER_PATH:
            return None
        if message.interface == 'org.freedesktop.DBus.Introspectable' and message.member == 'Introspect':
            return Message.new_method_return(message, 's', [WATCHER_XML])
        if message.interface == 'org.freedesktop.DBus.Properties':
            props = self.watcher_properties()
            if message.member == 'GetAll':
                return Message.new_method_return(message, 'a{sv}', [props])
            if message.member == 'Get' and len(message.body) == 2 and message.body[1] in props:
                return Message.new_method_return(message, 'v', [props[message.body[1]]])
        if message.interface == WATCHER and message.member == 'RegisterStatusNotifierItem':
            asyncio.create_task(self.register(message.sender, message.body[0]))
            return Message.new_method_return(message)
        if message.interface == WATCHER and message.member == 'RegisterStatusNotifierHost':
            service = message.body[0] or message.sender
            self.hosts.add(service)
            self.signal('StatusNotifierHostRegistered')
            return Message.new_method_return(message)
        return Message.new_error(message, 'org.freedesktop.DBus.Error.UnknownMethod', 'Unknown watcher request')

    def signal(self, member, signature='', body=None):
        self.bus.send(Message.new_signal(WATCHER_PATH, WATCHER, member, signature, body or []))

    async def register(self, sender, value):
        service = sender if value.startswith('/') else value
        path = value if value.startswith('/') else '/StatusNotifierItem'
        if not service or not path.startswith('/'):
            return
        key = service + path
        try:
            owner_reply = await self.dbus('GetNameOwner', 's', [service])
            owner = str(owner_reply.body[0]) if owner_reply.message_type == MessageType.METHOD_RETURN else service
            pid = await self.owner_pid(service)
            bundle = proc_bundle(pid)
            if not bundle:
                print('SNI_REJECT_UNOWNED service=%s pid=%d' % (service, pid), flush=True)
                return
            self.items[key] = {'key': key, 'service': service, 'owner': owner, 'path': path,
                'pid': pid, 'bundle': bundle, 'interface': '', 'title': bundle,
                'status': 'Active', 'tooltip': '', 'iconName': '', 'itemIsMenu': False,
                'menuPath': '', 'menu': []}
            await self.refresh(key)
            self.signal('StatusNotifierItemRegistered', 's', [key])
            await self.set_native_service(bundle, True)
            print('SNI_REGISTER bundle=%s service=%s path=%s pid=%d' %
                  (bundle, service, path, pid), flush=True)
        except Exception as error:
            self.items.pop(key, None)
            print('SNI_REGISTER_ERROR service=%s error=%r' % (service, error), flush=True)

    async def remove_service(self, service):
        removed = [key for key, item in self.items.items()
                   if item['service'] == service or item.get('owner') == service]
        removed_bundles = {self.items[key]['bundle'] for key in removed}
        for key in removed:
            self.items.pop(key, None)
            self.signal('StatusNotifierItemUnregistered', 's', [key])
        if removed:
            self.publish()
            for bundle in removed_bundles:
                if not any(item.get('bundle') == bundle for item in self.items.values()):
                    await self.set_native_service(bundle, False)
            print('SNI_UNREGISTER service=%s count=%d' % (service, len(removed)), flush=True)

    async def call(self, item, interface, member, signature='', body=None, path=None):
        return await self.bus.call(Message(destination=item['service'],
            path=path or item['path'], interface=interface, member=member,
            signature=signature, body=body or []))

    async def properties(self, item, interface):
        reply = await self.call(item, 'org.freedesktop.DBus.Properties', 'GetAll',
                                's', [interface])
        if reply.message_type != MessageType.METHOD_RETURN:
            raise RuntimeError(reply.error_name or 'GetAll failed')
        return reply.body[0]

    def menu_node(self, node, depth=0):
        item_id, raw_props, raw_children = node
        props = {name: plain(value) for name, value in raw_props.items()}
        result = {'id': int(item_id), 'label': str(props.get('label', '')),
                  'enabled': bool(props.get('enabled', True)),
                  'visible': bool(props.get('visible', True)),
                  'separator': props.get('type') == 'separator', 'children': []}
        if props.get('toggle-type'):
            result['toggleType'] = str(props.get('toggle-type'))
            result['toggleState'] = int(props.get('toggle-state', 0))
        if depth < 1:
            for child in raw_children:
                child_value = plain(child)
                if isinstance(child_value, (list, tuple)) and len(child_value) == 3:
                    result['children'].append(self.menu_node(child_value, depth + 1))
        return result

    async def refresh_menu(self, item):
        path = item.get('menuPath')
        if not path or path == '/':
            item['menu'] = []
            return
        reply = await self.call(item, 'com.canonical.dbusmenu', 'GetLayout',
            'iias', [0, 2, ['label', 'enabled', 'visible', 'type', 'children-display',
                            'toggle-type', 'toggle-state']], path=path)
        if reply.message_type != MessageType.METHOD_RETURN or len(reply.body) < 2:
            item['menu'] = []
            return
        root = self.menu_node(reply.body[1])
        item['menu'] = root['children']

    async def refresh(self, key):
        item = self.items.get(key)
        if not item:
            return
        try:
            props = None
            for interface in ('org.kde.StatusNotifierItem', 'org.freedesktop.StatusNotifierItem'):
                try:
                    props = await self.properties(item, interface)
                    item['interface'] = interface
                    break
                except Exception:
                    pass
            if props is None:
                raise RuntimeError('StatusNotifierItem properties unavailable')
            item['title'] = str(plain(props.get('Title'), item['bundle']))[:256]
            item['status'] = str(plain(props.get('Status'), 'Active'))
            item['iconName'] = str(plain(props.get('IconName'), ''))[:256]
            item['itemIsMenu'] = bool(plain(props.get('ItemIsMenu'), False))
            item['menuPath'] = str(plain(props.get('Menu'), ''))
            tooltip = plain(props.get('ToolTip'))
            if isinstance(tooltip, (list, tuple)) and len(tooltip) >= 4:
                item['tooltip'] = (str(tooltip[2]) + (' — ' if tooltip[2] and tooltip[3] else '') +
                                   str(tooltip[3]))[:512]
            else:
                item['tooltip'] = item['title']
            try:
                await self.refresh_menu(item)
            except Exception as error:
                item['menu'] = []
                print('SNI_MENU_ERROR key=%s error=%r' % (key, error), flush=True)
            self.publish()
        except Exception as error:
            print('SNI_REFRESH_ERROR key=%s error=%r' % (key, error), flush=True)

    def publish(self):
        self.generation += 1
        public = []
        for item in self.items.values():
            public.append({name: item.get(name) for name in (
                'key', 'bundle', 'title', 'status', 'tooltip', 'iconName',
                'itemIsMenu', 'menu')})
        atomic_json(self.state_path, {'version': 1, 'generation': self.generation,
                    'updated': time.time(), 'items': public})

    async def action_loop(self):
        native_reconcile = 0.0
        while True:
            await asyncio.sleep(.2)
            if time.monotonic() >= native_reconcile:
                native_reconcile = time.monotonic() + 1.0
                for bundle in {item.get('bundle') for item in self.items.values()}:
                    if bundle and not any(key[0] == bundle for key in self.native_services):
                        await self.set_native_service(bundle, True)
            try:
                data = json.loads(self.action_path.read_text())
                self.action_path.unlink(missing_ok=True)
                serial = int(data.get('serial', 0))
                if serial <= self.last_action:
                    continue
                self.last_action = serial
                item = next((entry for entry in self.items.values()
                    if entry['key'] == data.get('key') and entry['bundle'] == data.get('bundle')), None)
                if not item:
                    continue
                action = data.get('action')
                if action == 'activate':
                    member = 'ContextMenu' if item.get('itemIsMenu') else 'Activate'
                    await self.call(item, item['interface'], member, 'ii', [0, 0])
                elif action == 'menu':
                    menu_id = int(data.get('menuId'))
                    await self.call(item, 'com.canonical.dbusmenu', 'Event', 'isvu',
                        [menu_id, 'clicked', Variant('s', ''), int(time.time() * 1000) & 0xffffffff],
                        path=item['menuPath'])
                print('SNI_ACTION bundle=%s action=%s' % (item['bundle'], action), flush=True)
            except FileNotFoundError:
                pass
            except Exception as error:
                print('SNI_ACTION_ERROR %r' % (error,), flush=True)

    async def run(self):
        self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(self.directory, 0o700)
        self.bus = await MessageBus(bus_address=os.environ['DBUS_SESSION_BUS_ADDRESS']).connect()
        self.bus.add_message_handler(self.reply_handler)
        await self.dbus('AddMatch', 's', ["type='signal',sender='org.freedesktop.DBus',member='NameOwnerChanged'"])
        await self.dbus('AddMatch', 's', ["type='signal',interface='org.kde.StatusNotifierItem'"])
        await self.dbus('AddMatch', 's', ["type='signal',interface='org.freedesktop.StatusNotifierItem'"])
        await self.dbus('AddMatch', 's', ["type='signal',interface='com.canonical.dbusmenu'"])
        await self.bus.request_name(WATCHER)
        await self.bus.request_name('org.kde.StatusNotifierHost-l410')
        self.publish()
        self.signal('StatusNotifierHostRegistered')
        print('SNI_BRIDGE_READY watcher=1 host=1 directory=%s' % self.directory, flush=True)
        await self.action_loop()


if __name__ == '__main__':
    if len(sys.argv) != 2 or not os.path.isabs(sys.argv[1]):
        raise SystemExit('usage: sni_bridge.py ABSOLUTE_STATE_DIRECTORY')
    asyncio.run(Bridge(sys.argv[1]).run())
