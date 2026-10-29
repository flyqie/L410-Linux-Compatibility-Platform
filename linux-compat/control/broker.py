"""Development broker. Loopback, per-install credential, fixed root and commands."""
import configparser
import fcntl
import io
import json
import os
import pty
from pathlib import Path
import re
import select
import signal
import socket
import secrets
import shutil
import struct
import subprocess
import termios
import threading
import time
from desktop_entries import scan as scan_desktop_entries
from desktop_icons import sync_anco_icons
import entry_builds
import render_profiles
import file_exchange
import managed_jobs
import protocol_manager
import hap_installer
import ssh_manager
import tray_hosts

native_build=None
native_build_busy=False

BASE=Path('/data/vendor/l410-linux')
CODE=Path('/vendor/bin/l410-linux-control')
BUILDER_PUBLIC=Path('/vendor/l410-linux/entry-builder')
ROOT=BASE/'rootfs-bookworm'
# The host uses hidepid=2. Join only its declared read group in this broker;
# application processes clear supplementary groups in their private PID namespace.
for mount_line in Path('/proc/mounts').read_text().splitlines():
    fields=mount_line.split()
    if len(fields)>3 and fields[1]=='/proc' and fields[2]=='proc':
        options=dict(part.split('=',1) for part in fields[3].split(',') if '=' in part)
        if options.get('hidepid','0')!='0' and options.get('gid','').isdigit():
            read_group=int(options['gid'])
            if read_group not in os.getgroups():os.setgroups(os.getgroups()+[read_group])
        break
TOKEN=(BASE/'config/token').read_text().strip()
ENTER=[str(ROOT/'lib/ld-linux-aarch64.so.1'),'--library-path',
       str(ROOT/'lib/aarch64-linux-gnu')+':'+str(ROOT/'usr/lib/aarch64-linux-gnu'),
       str(CODE/'enter'),str(ROOT)]
instance_lock=(BASE/'run/broker.lock').open('a')
fcntl.flock(instance_lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
file_exchange.initialize()
bridge=None
current_app=None
current_owner=None
instances={}
native_sessions={}
resource_sample=(time.monotonic(),{})
job=managed_jobs.recover("apt")
lock=threading.RLock()
entry_lock=threading.Lock()
ENTRY=BASE/'config/mousepad-entry.json'
LAUNCHERS=BASE/'config/launchers.json'
ENTRY_STATES=BASE/'config/entry-states.json'
SETTINGS=BASE/'config/runtime.json'
DEFAULT_SETTINGS={'memoryMiB':2048,'pidsLimit':512,'sharedMode':'readwrite','playback':True,'microphone':False,'camera':False,'gpu':True}
ACTIVE_ANCO_ICONS=BASE/'config/active-anco-icons.json'
ANCO_CATALOG=ROOT/'opt/l410/anco-host-record/catalog-window-capabilities-r2/packages.json'
ANCO_RUNTIME_CATALOG=ROOT/'opt/l410/anco-host-record/catalog-resource-icons-r136'
ANCO_TRAY_DIR=ANCO_RUNTIME_CATALOG/'tray-bridge'
ANCO_TRAY_STATE=ANCO_TRAY_DIR/'tray-sni.json'
ANCO_TRAY_ACTION=ANCO_TRAY_DIR/'tray-sni-action.json'
ANCO_LAUNCH_LOG=BASE/'log/anco-launch.log'
ANCO_BMS_MANAGER=Path('/data/local/tmp/l410-anco-bms-cli-20261005/anco-bms-manager')
ANCO_SERVICE='l410_anco_r90'
ANCO_STOP_MARKER=Path('/data/local/tmp/l410-anco-graphic-r2/original-wms14-ime-state-r1.stop')
ANCO_CONTROL_LOG=BASE/'log/service-control.log'
ANCO_RUNNER_ROOTS=(str(BASE/'shared')+'/', '/vendor/l410-linux/anco-runtime-assets/')
ANCO_RUNTIME_ROOT='/opt/l410/anco-runtime/'
ANCO_RUNTIME_PREFIX='anco-linux-runtime-'
ANCO_READY_MARKER='ORIGINAL_ENVIRONMENT_RESIDENT_READY'
LOCAL_DEB_SOURCE=Path('/data/app/el2/100/base/com.l410.linux/haps/entry/files/local-install.deb')
LOCAL_DEB_DIR=ROOT/'var/cache/l410-local-deb'
HISHELL_BUNDLE='com.huawei.hmos.hishell'
HISHELL_CLIENT_SOURCE=CODE/'l410-hishell-client'
HISHELL_SOCKET='l410-hishell'
PTY_CHILD="import os,signal;signal.signal(signal.SIGTTOU,signal.SIG_IGN);signal.signal(signal.SIGTTIN,signal.SIG_IGN);os.tcsetpgrp(0,os.getpgrp());os.chdir('/home/linux');os.environ.update(HOME='/home/linux',USER='linux',LOGNAME='linux',SHELL='/bin/bash',TERM='xterm-256color',PS1='linux@l410:\\w$ ');os.execv('/bin/bash',['/bin/bash','--noprofile','--norc','-i'])"
anco_operation={'state':'idle','action':'','message':'尚未执行服务操作','started':0,'finished':0}
anco_operation_lock=threading.Lock()
shell_connection=threading.Condition()
shell_connection_count=0
terminal_lock=threading.RLock()
terminal_sessions={}

def tray_snapshot(bundle):
    if not re.fullmatch(r'org\.l410\.linux\.[a-z0-9_.]+',bundle or ''):
        raise ValueError('托盘应用身份无效')
    try:
        state=json.loads(ANCO_TRAY_STATE.read_text())
    except (OSError,ValueError):
        state={'version':1,'generation':0,'items':[]}
    if state.get('version')!=1 or not isinstance(state.get('items'),list):
        raise ValueError('托盘状态格式无效')
    items=[]
    for item in state['items']:
        if not isinstance(item,dict) or item.get('bundle')!=bundle:continue
        if not isinstance(item.get('key'),str) or len(item['key'])>512:continue
        items.append(item)
    # The native status-bar API owns one item per application context, exactly
    # like Qt OHOS. Prefer a visible active item if an application registered
    # more than one SNI object.
    items.sort(key=lambda item:(item.get('status')=='Passive',item.get('key','')))
    return {'generation':int(state.get('generation',0)),'active':bool(items),
            'item':items[0] if items else None}

def tray_action(body):
    bundle=body.get('bundle');current=tray_snapshot(bundle)
    item=current['item'];action=body.get('action')
    if not item or body.get('key')!=item.get('key'):
        raise ValueError('托盘项已经失效')
    if action not in ('activate','menu'):raise ValueError('托盘动作无效')
    request={'serial':time.time_ns(),'bundle':bundle,'key':item['key'],'action':action}
    if action=='menu':
        menu_id=body.get('menuId')
        if type(menu_id) is not int or menu_id<0 or menu_id>2147483647:
            raise ValueError('托盘菜单项无效')
        request['menuId']=menu_id
    ANCO_TRAY_DIR.mkdir(parents=True,exist_ok=True)
    candidate=ANCO_TRAY_ACTION.with_suffix('.new')
    candidate.write_text(json.dumps(request,separators=(',',':')))
    os.chmod(candidate,0o644);candidate.replace(ANCO_TRAY_ACTION)
    return {'queued':True,'serial':request['serial']}

def finish_anco_launch(connection,serial,desktop_id,bundle,ability):
    """Collect the resident runtime result after the menu entry has exited."""
    status=-1;pid=-1;message='runtime returned no response'
    try:
        with connection:
            answer=json.loads(connection.recv(4096))
        status=answer.get('status');pid=answer.get('pid')
        if type(status) is not int or type(pid) is not int:
            raise ValueError('runtime response fields are invalid')
        message='started' if status==0 and pid>0 else 'runtime rejected launch'
    except Exception as error:
        message=str(error)
    line=('%s serial=%d desktop=%s bundle=%s ability=%s status=%s pid=%s message=%s\n' %
          (time.strftime('%Y-%m-%dT%H:%M:%S%z'),serial,desktop_id,bundle,ability,
           status,pid,message.replace('\n',' ')[:512]))
    try:
        ANCO_LAUNCH_LOG.parent.mkdir(parents=True,exist_ok=True)
        with ANCO_LAUNCH_LOG.open('a') as log:log.write(line)
    except OSError:pass
    print('ANCO_LAUNCH_COMPLETE '+line.rstrip(),flush=True)

def active_anco_application_pids():
    """Return guest application PIDs admitted by the existing AnCo catalog."""
    result=[]
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():continue
        try:environment=(process/'environ').read_bytes().split(b'\0')
        except OSError:continue
        if any(value.startswith(b'L410_ANCO_BUNDLE=org.l410.linux.') for value in environment):
            result.append(int(process.name))
    return sorted(result)

def send_anco_launch(desktop_id,bundle,ability,serial=None):
    """Send one process-only request through the resident original runtime."""
    request=json.dumps({'bundle':bundle,'ability':ability},separators=(',',':')).encode()
    connection=socket.socket(socket.AF_UNIX,socket.SOCK_SEQPACKET)
    try:
        connection.settimeout(3)
        connection.connect('\0l410-anco-launch-r1')
        connection.sendall(request)
    except Exception:
        connection.close()
        raise
    serial=serial or time.time_ns()
    threading.Thread(target=finish_anco_launch,
        args=(connection,serial,desktop_id,bundle,ability),daemon=True).start()
    return serial

def prepare_anco_launch(serial,desktop_id):
    """Prepare the internal window identity, then use runtime lazy admission."""
    bundle=ability=''
    try:
        mapping=ensure_anco_target(desktop_id)
        bundle,ability=mapping['bundle'],mapping['ability']
        state=original_anco_runtime_state()
        if not state['running']:
            run_anco_operation('start')
        state=original_anco_runtime_state()
        if not state['running']:raise RuntimeError('Linux 兼容服务未就绪：'+state['operation'].get('message',''))
        send_anco_launch(desktop_id,bundle,ability,serial)
    except Exception as error:
        line=('%s serial=%d desktop=%s bundle=%s ability=%s status=-1 pid=-1 message=%s\n' %
              (time.strftime('%Y-%m-%dT%H:%M:%S%z'),serial,desktop_id,bundle,ability,str(error).replace('\n',' ')[:512]))
        try:
            ANCO_LAUNCH_LOG.parent.mkdir(parents=True,exist_ok=True)
            with ANCO_LAUNCH_LOG.open('a') as log:log.write(line)
        except OSError:pass
        print('ANCO_LAUNCH_COMPLETE '+line.rstrip(),flush=True)

def anco_launch(desktop_id):
    """Hand a desktop launch to the resident runtime without owning its window.

    The menu UIAbility ends once this resident broker has delivered the packet.
    The broker keeps the connected socket and records the eventual runtime
    result, so the one-shot launcher process does not need to stay visible.
    """
    if not isinstance(desktop_id,str):raise ValueError('desktop id 无效')
    app=next((item for item in scan_desktop_entries(ROOT) if item['id']==desktop_id),None)
    if not app or not app['supported']:raise ValueError((app or {}).get('reason','应用不存在或尚不支持启动'))
    try:
        bundle,ability=entry_builds.anco_target(desktop_id)
    except ValueError:
        serial=time.time_ns()
        threading.Thread(target=prepare_anco_launch,args=(serial,desktop_id),daemon=True).start()
        return {'accepted':True,'queued':True,'started':False,'preparing':True,'serial':serial,
                'desktopId':desktop_id,'taskCreated':False,'windowPending':True}
    state=original_anco_runtime_state()
    if not state['running']:
        serial=time.time_ns()
        threading.Thread(target=prepare_anco_launch,args=(serial,desktop_id),daemon=True).start()
        return {'accepted':True,'queued':True,'started':False,'preparing':True,'serial':serial,
                'desktopId':desktop_id,'bundle':bundle,'ability':ability,
                'taskCreated':False,'windowPending':True}
    serial=send_anco_launch(desktop_id,bundle,ability)
    return {'accepted':True,'queued':True,'started':False,'serial':serial,
            'desktopId':desktop_id,'bundle':bundle,'ability':ability,
            'taskCreated':False,'windowPending':True}

def settings():
    if not SETTINGS.exists():return dict(DEFAULT_SETTINGS)
    value={**DEFAULT_SETTINGS,**json.loads(SETTINGS.read_text())};validate_settings(value);return value

def validate_settings(value):
    if set(value)!=set(DEFAULT_SETTINGS):raise ValueError('unexpected settings fields')
    if type(value['memoryMiB']) is not int or not 512<=value['memoryMiB']<=4096:raise ValueError('memoryMiB must be 512..4096')
    if type(value['pidsLimit']) is not int or not 64<=value['pidsLimit']<=1024:raise ValueError('pidsLimit must be 64..1024')
    if value['sharedMode'] not in ('readwrite','readonly','off'):raise ValueError('invalid shared mode')
    if any(type(value[k]) is not bool for k in ('playback','microphone','camera','gpu')):raise ValueError('hardware permissions must be boolean')

def launch_environment(owned=False):
    value=settings()
    env=dict(os.environ,L410_MEMORY_MIB=str(value['memoryMiB']),L410_PIDS_LIMIT=str(value['pidsLimit']),L410_SHARED_MODE=value['sharedMode'])
    if owned:
        env.update(L410_OWNER_PID=str(os.getpid()),L410_OWNER_START=Path('/proc/self/stat').read_text().rsplit(')',1)[1].split()[19])
    return env

def launchers():
    value=json.loads(LAUNCHERS.read_text()) if LAUNCHERS.exists() else {}
    for desktop_id,item in entry_builds.read().items():
        if item.get('bundle') and item.get('name'):
            value[desktop_id]={'name':item['name'],'bundle':item['bundle']}
    for key,entry in value.items():
        if not re.fullmatch(r'[A-Za-z0-9_.+-]{1,160}\.desktop',key) or not re.fullmatch(r'com\.l410\.linux\.(?:app\.)?[a-z][a-z0-9_]{0,80}',entry['bundle']):raise ValueError('invalid launcher registry')
    if len({entry['bundle'] for entry in value.values()})!=len(value):raise ValueError('duplicate launcher bundle')
    return value

def entry_hap(entry):
    return BASE/'launchers'/(entry['bundle']+'.hap')

def entry_states():
    if ENTRY_STATES.exists():return json.loads(ENTRY_STATES.read_text())
    try:return {'org.xfce.mousepad.desktop':json.loads(ENTRY.read_text()).get('enabled',False)}
    except (OSError,ValueError):return {}

def manage_entry(enabled,desktop_id='org.xfce.mousepad.desktop'):
    with entry_lock:
        if enabled and not (ROOT/'usr/share/applications'/desktop_id).is_file():raise ValueError('Linux application is not installed')
        catalog=json.loads(ANCO_CATALOG.read_text()) if ANCO_CATALOG.exists() else {}
        matches=[record for record in catalog.values()
                 if isinstance(record,dict) and record.get('desktop')==desktop_id]
        if len(matches)!=1:raise ValueError('此应用尚未登记到原生 AnCo 目录')
        record=matches[0];bundle=record.get('bundle','');ability=record.get('ability','')
        if (not re.fullmatch(r'org\.l410\.linux\.[a-z0-9_.]+',bundle)
                or ability!=bundle+'.Main'):
            raise ValueError('AnCo 应用身份无效')
        previous=record.get('menu_visible',False)
        if type(previous) is not bool:raise ValueError('AnCo 菜单状态无效')
        record['menu_visible']=enabled
        candidate=ANCO_CATALOG.with_suffix('.new')
        candidate.write_text(json.dumps(catalog,ensure_ascii=False,indent=2))
        candidate.replace(ANCO_CATALOG)
        try:
            if enabled:
                sync_anco_runtime_resources(bundle)
            else:
                sync_anco_runtime_catalog()
            if previous != enabled:
                replace_anco_runtime_bundle(bundle)
            else:
                register_anco_runtime_bundle(bundle)
        except Exception:
            record['menu_visible']=previous
            candidate.write_text(json.dumps(catalog,ensure_ascii=False,indent=2))
            candidate.replace(ANCO_CATALOG)
            if previous:
                sync_anco_runtime_resources(bundle)
            else:
                sync_anco_runtime_catalog()
            # A failed replacement may already have removed the new record.
            # Re-register the restored authoritative record directly.
            register_anco_runtime_bundle(bundle)
            raise
        # The original AnCo virtual package is the launcher identity.  The
        # system BMS extension merges SA 66816 records into LauncherAbility and
        # SceneBoard projects those records into the normal app grid.  A formal
        # per-app HAP would create a second identity and a disposable white
        # launcher window, so it must not participate in this path.
        states=entry_states();states[desktop_id]=enabled
        candidate=ENTRY_STATES.with_suffix('.new');candidate.write_text(json.dumps(states));candidate.replace(ENTRY_STATES)
        if desktop_id=='org.xfce.mousepad.desktop':ENTRY.write_text(json.dumps({'enabled':enabled}))
        return {'enabled':enabled,'name':record.get('label',desktop_id),'bundle':bundle,
                'ability':ability,'launcherBundle':bundle,'nativeAncoEntry':True}

def entry_health():
    """What each entry looks like on the device: installed, or only a leftover."""
    states=entry_states();result=[]
    for desktop_id,entry in launchers().items():
        bundle=entry.get('bundle','')
        directory=Path('/data/app/el1/bundle/public')/bundle if bundle else None
        haps=list(directory.glob('*.hap')) if (directory and directory.is_dir()) else []
        result.append({'id':desktop_id,'name':entry.get('name',''),'bundle':bundle,
                       'enabled':states.get(desktop_id,False),
                       'bundleDir':bool(directory and directory.exists()),
                       'bundleHap':bool(haps),
                       'bundleHapBytes':sum(hap.stat().st_size for hap in haps)})
    return result

def entry_enabled(desktop_id='org.xfce.mousepad.desktop'):
    return entry_states().get(desktop_id,False)

def forget_entry(desktop_id):
    """Remove only the launcher artifacts owned by one uninstalled desktop file."""
    entry_builds.remove(desktop_id)
    if LAUNCHERS.exists():
        registry=json.loads(LAUNCHERS.read_text());registry.pop(desktop_id,None)
        candidate=LAUNCHERS.with_suffix('.new');candidate.write_text(json.dumps(registry,ensure_ascii=False));candidate.replace(LAUNCHERS)
    states=entry_states();states.pop(desktop_id,None)
    candidate=ENTRY_STATES.with_suffix('.new');candidate.write_text(json.dumps(states));candidate.replace(ENTRY_STATES)
    if desktop_id=='org.xfce.mousepad.desktop':ENTRY.write_text(json.dumps({'enabled':False}))

def finish_apt(process):
    process.wait()
    try:
        with lock:
            if not (BASE/'config/disabled').exists() and process.returncode==0:
                for desktop_id in launchers():
                    if not (ROOT/'usr/share/applications'/desktop_id).exists():
                        try:
                            if entry_enabled(desktop_id):manage_entry(False,desktop_id)
                            forget_entry(desktop_id)
                        except Exception as e:
                            with (BASE/'log/apt.log').open('a') as log:log.write('\nENTRY_CLEANUP_ERROR '+str(e)+'\n')
                    elif entry_enabled(desktop_id):
                        try:
                            ensure_anco_target(desktop_id)
                            manage_entry(True,desktop_id)
                        except Exception as e:
                            with (BASE/'log/apt.log').open('a') as log:log.write('\nENTRY_REFRESH_ERROR '+str(e)+'\n')
                try:refresh_active_anco_icons()
                except Exception as e:
                    with (BASE/'log/apt.log').open('a') as log:log.write('\nANCO_ICON_REFRESH_ERROR '+str(e)+'\n')
    finally:
        if process.metadata.get('localDeb'):
            for staged in (LOCAL_DEB_DIR/'selected.deb',LOCAL_DEB_SOURCE):
                try:staged.unlink(missing_ok=True)
                except OSError:pass
        process.handled()

def refresh_active_anco_icons():
    if not ACTIVE_ANCO_ICONS.is_file():return None
    value=json.loads(ACTIVE_ANCO_ICONS.read_text())
    if set(value)!= {'version','root','catalog','titleDir'} or value['version']!=1:raise ValueError('invalid active AnCo icon state')
    root=Path(value['root']).resolve(strict=True);catalog=Path(value['catalog']).resolve(strict=True);title=Path(value['titleDir']).resolve(strict=True)
    if root!=ROOT.resolve(strict=True) or not catalog.is_relative_to((ROOT/'opt/l410/anco-host-record').resolve(strict=True)):
        raise ValueError('active AnCo icon paths are outside this rootfs')
    allowed=Path('/data/app/el2').resolve(strict=True)
    if not title.is_relative_to(allowed) or 'com.huawei.shell_assistant' not in title.parts or title.name!='linux-title':
        raise ValueError('active AnCo title path is not ShellAssistant linux-title')
    converter=[str(ROOT/'lib/ld-linux-aarch64.so.1'),'--library-path',str(ROOT/'usr/lib/aarch64-linux-gnu')+':'+str(ROOT/'lib/aarch64-linux-gnu'),str(ROOT/'usr/bin/rsvg-convert')]
    return sync_anco_icons(ROOT,catalog,title,converter)

def apps():
    states=entry_states();result=scan_desktop_entries(ROOT);profiles=render_profiles.read()
    catalog=json.loads(ANCO_CATALOG.read_text()) if ANCO_CATALOG.exists() else {}
    targets={record.get('desktop'):record for record in catalog.values()
             if isinstance(record,dict) and record.get('desktop')}
    for app in result:
        target=targets.get(app['id'],{})
        bundle=target.get('bundle','');ability=target.get('ability','')
        launch_ready=(re.fullmatch(r'org\.l410\.linux\.[a-z0-9_.]+',bundle) is not None
                      and re.fullmatch(r'[A-Za-z0-9_.]+',ability) is not None
                      and ability.startswith(bundle+'.'))
        menu_visible=target.get('menu_visible',states.get(app['id'],False))
        app.update(entryReady=launch_ready,
                   entryEnabled=launch_ready and menu_visible is True,
                   entryInstalled=launch_ready,
                   entryBundle=bundle if launch_ready else '',
                   renderProfile=profiles.get(app['id'],'software'),
                   launchReady=launch_ready,
                   catalogState='registered' if launch_ready else 'unregistered',
                   catalogReason='' if launch_ready else '首次直接打开时会自动准备内部 AnCo 窗口身份；无需加入菜单',
                   buildState='',
                   buildError='',
                   ancoBundle=bundle if launch_ready else '',
                   ancoAbility=ability if launch_ready else '')
    return result


def anco_bundle_for(desktop_id):
    if not isinstance(desktop_id,str) or not re.fullmatch(r'[A-Za-z0-9_.+-]{1,160}\.desktop',desktop_id):
        raise ValueError('desktop id 无效')
    slug=re.sub(r'[^a-z0-9]+','_',desktop_id[:-8].lower()).strip('_')
    if not slug:raise ValueError('desktop id 不能生成 AnCo 身份')
    if slug[0].isdigit():slug='app_'+slug
    if len(slug)>80:raise ValueError('desktop id 过长，不能生成无歧义的 AnCo 身份')
    bundle='org.l410.linux.'+slug
    catalog=json.loads(ANCO_CATALOG.read_text()) if ANCO_CATALOG.exists() else {}
    for existing,record in catalog.items():
        if isinstance(record,dict) and record.get('desktop')==desktop_id:return existing
    if bundle in catalog:raise ValueError('AnCo 身份已属于另一 Linux 应用')
    return bundle


def desktop_package(desktop_id):
    path='/usr/share/applications/'+desktop_id
    result=subprocess.run(ENTER+['/usr/bin/dpkg-query','-S',path],capture_output=True,text=True,timeout=15)
    if result.returncode:raise ValueError('无法从 dpkg 确认此 .desktop 所属的软件包')
    packages={line.split(':',1)[0].split(':',1)[0].strip() for line in result.stdout.splitlines() if ': ' in line}
    packages={package for package in packages if re.fullmatch(r'[a-z0-9][a-z0-9+.-]{0,100}',package)}
    if len(packages)!=1:raise ValueError('此 .desktop 的软件包归属不唯一')
    return packages.pop()


def ensure_anco_target(desktop_id):
    app=next((item for item in scan_desktop_entries(ROOT) if item['id']==desktop_id),None)
    if not app or not app['supported']:raise ValueError('应用不存在或尚不支持启动')
    catalog=json.loads(ANCO_CATALOG.read_text()) if ANCO_CATALOG.exists() else {}
    matches=[record for record in catalog.values() if isinstance(record,dict) and record.get('desktop')==desktop_id]
    if len(matches)==1:
        try:runtime=json.loads((ANCO_RUNTIME_CATALOG/'packages.json').read_text())
        except (OSError,ValueError):runtime={}
        runtime_record=runtime.get(matches[0]['bundle'])
        changed=not isinstance(runtime_record,dict) or runtime_record.get('desktop')!=desktop_id
        sync_anco_runtime_resources(matches[0]['bundle'])
        register_anco_runtime_bundle(matches[0]['bundle'])
        return {'changed':changed,'bundle':matches[0]['bundle'],'ability':matches[0]['ability']}
    if matches:raise ValueError('此应用存在重复的 AnCo 目录记录')
    try:runtime=json.loads((ANCO_RUNTIME_CATALOG/'packages.json').read_text())
    except (OSError,ValueError):runtime={}
    runtime_matches=[record for record in runtime.values()
                     if isinstance(record,dict) and record.get('desktop')==desktop_id]
    if len(runtime_matches)==1:
        record=runtime_matches[0]
        register_anco_runtime_bundle(record['bundle'])
        return {'changed':False,'bundle':record['bundle'],'ability':record['ability'],
                'package':record.get('package','')}
    if runtime_matches:raise ValueError('此应用存在重复的 AnCo 运行时目录记录')
    bundle=anco_bundle_for(desktop_id);package=desktop_package(desktop_id)
    loader=str(ROOT/'lib/ld-linux-aarch64.so.1')
    library_path=str(ROOT/'usr/lib/aarch64-linux-gnu')+':'+str(ROOT/'lib/aarch64-linux-gnu')
    command=[loader,'--library-path',library_path,str(ROOT/'usr/bin/python3'),
             str(CODE/'prepare-anco-package.py'),'--rootfs',str(ROOT),
             '--catalog',str(ANCO_CATALOG.parent),'--desktop',desktop_id,
             '--package',package,'--bundle',bundle]
    result=subprocess.run(command,capture_output=True,text=True,timeout=30)
    if result.returncode:raise ValueError('建立 AnCo 启动映射失败：'+((result.stdout or '')+(result.stderr or '')).strip())
    record=json.loads(result.stdout.strip().splitlines()[-1])
    sync_anco_runtime_resources(record['bundle'])
    register_anco_runtime_bundle(record['bundle'])
    return {'changed':True,'bundle':record['bundle'],'ability':record['bundle']+'.Main','package':package}


def register_anco_runtime_bundle(bundle):
    if not re.fullmatch(r'org\.l410\.linux\.[a-z0-9_.]+',bundle):raise ValueError('AnCo bundle 无效')
    record=ANCO_RUNTIME_CATALOG/(bundle+'.json')
    if not ANCO_BMS_MANAGER.is_file() or not record.is_file():raise ValueError('原生 AnCo 登记工具或元数据缺失')
    loader=str(ROOT/'lib/aarch64-linux-gnu/ld-linux-aarch64.so.1')
    library_path=str(ROOT/'usr/lib/aarch64-linux-gnu')+':'+str(ROOT/'lib/aarch64-linux-gnu')
    result=subprocess.run([loader,'--library-path',library_path,str(ANCO_BMS_MANAGER),
                           '--manager-register',str(record)],capture_output=True,text=True,timeout=30)
    if result.returncode:raise ValueError('原生 AnCo 身份登记失败：'+((result.stdout or '')+(result.stderr or '')).strip())
    notify=subprocess.run([loader,'--library-path',library_path,str(ANCO_BMS_MANAGER),
                           '--manager-notify',bundle,'0','0'],capture_output=True,text=True,timeout=30)
    if notify.returncode:raise ValueError('原生 AnCo 桌面状态通知失败：'+((notify.stdout or '')+(notify.stderr or '')).strip())
    return {'bundle':bundle,'registered':True,'notified':True,
            'output':((result.stdout or '')+(result.stderr or '')+(notify.stdout or '')+(notify.stderr or '')).strip()}


def replace_anco_runtime_bundle(bundle):
    """Replace one original AnCo record after its desktop visibility changes."""
    if not re.fullmatch(r'org\.l410\.linux\.[a-z0-9_.]+',bundle):raise ValueError('AnCo bundle 无效')
    if not ANCO_BMS_MANAGER.is_file():raise ValueError('原生 AnCo 登记工具缺失')
    loader=str(ROOT/'lib/aarch64-linux-gnu/ld-linux-aarch64.so.1')
    library_path=str(ROOT/'usr/lib/aarch64-linux-gnu')+':'+str(ROOT/'lib/aarch64-linux-gnu')
    removed=subprocess.run([loader,'--library-path',library_path,str(ANCO_BMS_MANAGER),
                            '--manager-unregister',bundle],capture_output=True,text=True,timeout=30)
    if removed.returncode:raise ValueError('原生 AnCo 旧登记移除失败：'+((removed.stdout or '')+(removed.stderr or '')).strip())
    registered=register_anco_runtime_bundle(bundle)
    registered['replaced']=True
    registered['output']=((removed.stdout or '')+(removed.stderr or '')+registered['output']).strip()
    return registered


def sync_anco_runtime_catalog():
    loader=str(ROOT/'lib/ld-linux-aarch64.so.1')
    library_path=str(ROOT/'usr/lib/aarch64-linux-gnu')+':'+str(ROOT/'lib/aarch64-linux-gnu')
    command=[loader,'--library-path',library_path,str(ROOT/'usr/bin/python3'),
             str(CODE/'sync-anco-runtime-catalog.py'),
             '--source',str(ANCO_CATALOG.parent),'--output',str(ANCO_RUNTIME_CATALOG)]
    result=subprocess.run(command,capture_output=True,text=True,timeout=30)
    if result.returncode:raise ValueError('同步 AnCo 运行目录失败：'+((result.stdout or '')+(result.stderr or '')).strip())
    return json.loads(result.stdout.strip().splitlines()[-1])


def sync_anco_runtime_resources(required_bundle=None):
    catalog=sync_anco_runtime_catalog()
    icons=refresh_active_anco_icons()
    if required_bundle and required_bundle in icons.get('errors',{}):
        raise ValueError('Linux 应用图标不可用：'+icons['errors'][required_bundle])
    return {'catalog':catalog,'icons':icons}


def anco_runner_command(command):
    return any(any(token.startswith(root) for root in ANCO_RUNNER_ROOTS) and
               Path(token).name.startswith('server-') and token.endswith('.sh')
               for token in command.split())


def anco_runtime_command(command):
    return any(token.startswith(ANCO_RUNTIME_ROOT) and
               (Path(token).name=='anco-linux-runtime' or
                Path(token).name.startswith(ANCO_RUNTIME_PREFIX))
               for token in command.split())


def runner_log_from_process(process,command):
    """Return the log owned by the currently executing compatibility runner.

    Runner revisions and filename conventions are deployment details.  The
    live runner's stdout is the authoritative log, so following fd 1 keeps
    status valid across upgrades without accepting a historical ready marker.
    """
    if not anco_runner_command(command):return None
    try:path=Path(os.readlink(process/'fd/1'))
    except OSError:return None
    trusted_roots=(str(BASE/'shared')+'/',str(BASE/'log')+'/')
    if (path.is_absolute() and any(str(path).startswith(root) for root in trusted_roots)
            and path.name.startswith('runner-') and path.suffix=='.log'):
        return path
    return None


def original_anco_runtime_state():
    pids=[];runner_pids=[];runner_logs=[]
    for process in Path('/proc').iterdir():
        if not process.name.isdigit():continue
        try:command=(process/'cmdline').read_bytes().replace(b'\0',b' ').decode(errors='replace')
        except OSError:continue
        if anco_runtime_command(command):pids.append(int(process.name))
        if anco_runner_command(command):
            runner_pids.append(int(process.name))
            runner_log=runner_log_from_process(process,command)
            if runner_log and runner_log not in runner_logs:runner_logs.append(runner_log)
    try:service=subprocess.run(['/system/bin/param','get','startup.service.ctl.'+ANCO_SERVICE],capture_output=True,text=True,timeout=3).stdout.strip()
    except (OSError,subprocess.SubprocessError):service=''
    ready_marker=False
    for runner_log in runner_logs:
        try:
            if ANCO_READY_MARKER in runner_log.read_text(errors='replace')[-65536:]:ready_marker=True;break
        except OSError:pass
    running=bool(pids and runner_pids and ready_marker)
    with anco_operation_lock:operation=dict(anco_operation)
    if operation['state']=='running':phase=operation['action']
    elif running:phase='running'
    elif runner_pids or pids:phase='starting'
    elif operation['state']=='error':phase='error'
    else:phase='stopped'
    return {'running':running,'pids':sorted(pids),'runnerPids':sorted(runner_pids),'runnerLogs':[str(path) for path in runner_logs],
            'serviceParameter':service,'phase':phase,'operation':operation}


def service_control_log(message):
    ANCO_CONTROL_LOG.parent.mkdir(parents=True,exist_ok=True)
    with ANCO_CONTROL_LOG.open('a') as output:
        output.write(time.strftime('%Y-%m-%d %H:%M:%S ') + message + '\n')


def wait_anco(predicate,timeout):
    deadline=time.monotonic()+timeout
    state=original_anco_runtime_state()
    while time.monotonic()<deadline:
        if predicate(state):return state
        time.sleep(.5);state=original_anco_runtime_state()
    return state


def run_anco_operation(action):
    with anco_operation_lock:
        anco_operation.update(state='running',action=action,message='正在执行',started=time.time(),finished=0)
    service_control_log('BEGIN '+action)
    try:
        if action in ('stop','restart'):
            ANCO_STOP_MARKER.parent.mkdir(parents=True,exist_ok=True)
            ANCO_STOP_MARKER.write_text('requested by Linux compatibility manager\n')
            state=wait_anco(lambda value:not value['pids'] and not value['runnerPids'],45)
            result=subprocess.run(['/system/bin/begetctl','stop_service',ANCO_SERVICE],capture_output=True,text=True,timeout=10)
            service_control_log('STOP command rc=%d %s'%(result.returncode,((result.stdout or '')+(result.stderr or '')).strip()))
            state=wait_anco(lambda value:not value['pids'] and not value['runnerPids'],10)
            if state['pids'] or state['runnerPids']:
                raise RuntimeError('停止超时：仍有兼容服务进程 '+str(state['pids']+state['runnerPids']))
        if action in ('start','restart'):
            ANCO_STOP_MARKER.unlink(missing_ok=True)
            result=subprocess.run(['/system/bin/begetctl','start_service',ANCO_SERVICE],capture_output=True,text=True,timeout=10)
            service_control_log('START command rc=%d %s'%(result.returncode,((result.stdout or '')+(result.stderr or '')).strip()))
            state=wait_anco(lambda value:value['running'],75)
            if not state['running']:
                raise RuntimeError('启动超时：兼容服务未进入就绪状态')
        message={'start':'Linux 兼容服务已启动','stop':'Linux 兼容服务已停止','restart':'Linux 兼容服务已重启'}[action]
        with anco_operation_lock:anco_operation.update(state='success',message=message,finished=time.time())
        service_control_log('SUCCESS '+action)
    except Exception as error:
        with anco_operation_lock:anco_operation.update(state='error',message=str(error),finished=time.time())
        service_control_log('ERROR '+action+' '+str(error))


def request_anco_operation(action):
    if action not in ('start','stop','restart'):raise ValueError('无效服务操作')
    with anco_operation_lock:
        if anco_operation['state']=='running':raise ValueError('已有服务操作正在执行')
    threading.Thread(target=run_anco_operation,args=(action,),daemon=True).start()
    return {'state':'running','action':action,'message':'操作已开始'}

# --- control host (native ArkUI controls, one window per Linux application) ---
UI_HOST_BUNDLE='com.l410.linux.uihost'
UI_STATE=Path('/data/app/el2/100/base')/UI_HOST_BUNDLE/'files/ui-sessions'
UI_LAUNCH=str(CODE/'l410-ui-launch')
audio_relay_thread=None
audio_relay_bytes=[0]
audio_capture_relay_thread=None
UI_PROGRAMS={'demo':'/data/local/tmp/l410ui-demo'}

def ui_host_pid():
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():continue
        try:cmd=(proc/'cmdline').read_bytes().replace(b'\0',b' ').decode(errors='replace')
        except OSError:continue
        if UI_HOST_BUNDLE in cmd:return int(proc.name)
    return 0

def ui_sessions():
    listening=Path('/proc/net/unix').read_text(errors='replace')
    result=[]
    if UI_STATE.is_file():
        for line in UI_STATE.read_text(errors='replace').splitlines():
            parts=line.split()
            if len(parts)<2 or not parts[0].isdigit():continue
            index=int(parts[0])
            result.append({'session':index,'app':parts[1],'listening':('@l410-native-ui-%d'%index) in listening})
    return result

def ui_clients():
    result=[]
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():continue
        try:cmd=(proc/'cmdline').read_bytes().replace(b'\0',b' ').decode(errors='replace')
        except OSError:continue
        if 'l410ui-' in cmd:result.append({'pid':int(proc.name),'command':cmd.strip()})
    return result

def ui_state():
    return {'hostPid':ui_host_pid(),'sessions':ui_sessions(),'clients':ui_clients(),'programs':sorted(UI_PROGRAMS)}

def ui_launch(app):
    # A menu entry names the desktop file; the program table is keyed by program.
    if isinstance(app,str) and app.startswith('desktop:'):
        app=app.split(':',1)[1]
    if isinstance(app,str) and app.endswith('.desktop'):
        app=app[:-len('.desktop')]
    if not isinstance(app,str) or not re.fullmatch(r'[a-z0-9][a-z0-9_-]{0,40}',app):
        raise ValueError('应用名无效')
    program=UI_PROGRAMS.get(app)
    if program is None:raise ValueError('该应用还没有原生控件界面')
    if not Path(program).is_file():raise ValueError('程序不存在：'+program)
    subprocess.Popen([UI_LAUNCH,program,app],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,start_new_session=True)
    return {'state':'launching','app':app}

def stop_linux_programs():
    """Stop the Linux side of a window as well: closing the window must not leave
    the program (or its container session) running in the background."""
    killed=[]
    markers=('/opt/l410/l410ui-','/data/local/tmp/l410ui-demo',str(CODE/'enter'))
    for proc in Path('/proc').iterdir():
        if not proc.name.isdigit():continue
        try:cmd=(proc/'cmdline').read_bytes().replace(b'\0',b' ').decode(errors='replace')
        except OSError:continue
        if not any(marker in cmd for marker in markers):continue
        try:
            os.kill(int(proc.name),15);killed.append(int(proc.name))
        except OSError:pass
    return killed

clipboard_relay_thread=None
clipboard_state={'toContainer':0,'toHarmony':0,'sensors':0}

def clipboard_relay_start():
    """Carry clipboard text between the application and the container.

    The application writes what the HarmonyOS clipboard holds into its own files
    directory; the container reads and writes files in the shared directory; this
    service is the only process that can see both.
    """
    global clipboard_relay_thread
    if clipboard_relay_thread and clipboard_relay_thread.is_alive():
        return {'state': 'running', **clipboard_state}
    app_out = Path('/data/app/el2/100/base/com.l410.linux.uihost/files/clipboard-out.txt')
    app_in = Path('/data/app/el2/100/base/com.l410.linux.uihost/files/clipboard-in.txt')
    shared_out = Path('/data/vendor/l410-linux/shared/clipboard-from-harmony.txt')
    shared_in = Path('/data/vendor/l410-linux/shared/clipboard-to-harmony.txt')

    def copy(source, target, key):
        try:
            data = source.read_bytes()
        except OSError:
            return
        try:
            if not target.exists() or target.read_bytes() != data:
                target.write_bytes(data)
                clipboard_state[key] += 1
        except OSError:
            pass

    def pump():
        while True:
            try:
                copy(app_out, shared_out, 'toContainer')
                copy(shared_in, app_in, 'toHarmony')
                copy(Path('/data/app/el2/100/base/com.l410.linux.uihost/files/sensors.json'),
                     Path('/data/vendor/l410-linux/shared/sensors.json'), 'sensors')
            except Exception as error:
                with (BASE / 'log/apt.log').open('a') as log:
                    log.write('\nCLIPBOARD_RELAY_ERROR ' + str(error) + '\n')
            time.sleep(0.5)

    clipboard_relay_thread = threading.Thread(target=pump, daemon=True)
    clipboard_relay_thread.start()
    return {'state': 'started', **clipboard_state}

audio_capture_bytes=[0]

def audio_capture_relay_start():
    """The microphone direction: the application writes captured PCM into its own
    files directory, the container reads it from the shared directory, and this
    service carries it between the two."""
    global audio_capture_relay_thread
    if audio_capture_relay_thread and audio_capture_relay_thread.is_alive():
        return {'state': 'running', 'bytesForwarded': audio_capture_bytes[0]}
    source = Path('/data/app/el2/100/base/com.l410.linux.uihost/files/audio-capture.fifo')
    target = Path('/data/vendor/l410-linux/shared/audio-capture.fifo')

    def pump():
        while True:
            try:
                if not source.exists():
                    time.sleep(0.5)
                    continue
                with source.open('rb', buffering=0) as stream:
                    if not target.exists():
                        os.mkfifo(str(target), 0o666)
                    with target.open('wb', buffering=0) as sink:
                        while True:
                            chunk = stream.read(4096)
                            if not chunk:
                                break
                            sink.write(chunk)
                            audio_capture_bytes[0] += len(chunk)
            except Exception as error:
                with (BASE / 'log/apt.log').open('a') as log:
                    log.write('\nAUDIO_CAPTURE_RELAY_ERROR ' + str(error) + '\n')
                time.sleep(1)

    audio_capture_relay_thread = threading.Thread(target=pump, daemon=True)
    audio_capture_relay_thread.start()
    return {'state': 'started'}

def audio_relay_start():
    """Pump playback PCM between the two places that can see each one.

    The container can only write into the shared directory, the application can
    only read from its own files directory, and this service is the one process
    that can reach both. The relay is what makes the audio path work without any
    network protocol in between.
    """
    global audio_relay_thread
    if audio_relay_thread and audio_relay_thread.is_alive():
        return {'state': 'running', 'bytesForwarded': audio_relay_bytes[0]}
    shared = Path('/data/vendor/l410-linux/shared/audio-playback.fifo')
    target = Path('/data/app/el2/100/base/com.l410.linux.uihost/files/audio-playback.fifo')

    def pump():
        while True:
            try:
                if not shared.exists() or not target.exists():
                    time.sleep(0.5)
                    continue
                with shared.open('rb', buffering=0) as source:
                    with target.open('wb', buffering=0) as sink:
                        while True:
                            chunk = source.read(4096)
                            if not chunk:
                                break
                            sink.write(chunk)
                            audio_relay_bytes[0] += len(chunk)
            except Exception as error:
                with (BASE / 'log/apt.log').open('a') as log:
                    log.write('\nAUDIO_RELAY_ERROR ' + str(error) + '\n')
                time.sleep(1)

    audio_relay_thread = threading.Thread(target=pump, daemon=True)
    audio_relay_thread.start()
    return {'state': 'started'}

ui_type_pending={'change':None}

def ui_type_set(node_id, text):
    """Queue a text change for the window: this is what a native input control
    sends when the user types in it (including through the Chinese IME)."""
    ui_type_pending['change'] = {'id': int(node_id), 'text': str(text)[:1024]}
    return ui_type_pending['change']

def ui_type_take():
    # Flat on purpose: the host parses this reply with a tiny hand written
    # scanner, so the fields sit at the top level.
    change = ui_type_pending['change']
    ui_type_pending['change'] = None
    if not change:
        return {'id': 0, 'text': ''}
    return {'id': change['id'], 'text': change['text']}

ui_tap_pending={'tap':None}

def ui_tap_set(x, y):
    """Queue a tap for the page: the page applies it through the native side,
    which runs the same hit test a real touch would."""
    ui_tap_pending['tap'] = {'x': float(x), 'y': float(y)}
    return ui_tap_pending['tap']

def ui_tap_take():
    tap = ui_tap_pending['tap']
    ui_tap_pending['tap'] = None
    return {'tap': tap}

window_watchdog_thread=None
window_watchdog_state={'had_client': False, 'idle_since': 0.0}

def window_watchdog_tick():
    """Close the control window when the Linux application has gone.

    The host notices a client leaving (its session goes idle) but the empty
    window would stay on screen, so the window is closed from here once it has
    been idle for a while.
    """
    state = window_watchdog_state
    host = ui_host_pid()
    if not host:
        state['had_client'] = False
        state['idle_since'] = 0.0
        return
    # A session names the client that owns it while one is connected, which is a
    # more reliable sign of life than the client list.
    clients = 0
    try:
        state_view = ui_state()
        sessions = state_view.get('sessions') if isinstance(state_view, dict) else state_view
        for session in sessions or []:
            if isinstance(session, dict) and session.get('app'):
                clients += 1
    except Exception:
        clients = 0
    if clients:
        state['had_client'] = True
        state['idle_since'] = 0.0
        return
    if not state['had_client']:
        return
    now = time.time()
    if not state['idle_since']:
        state['idle_since'] = now
        return
    if now - state['idle_since'] < 20.0:
        return
    subprocess.run(['/system/bin/aa', 'force-stop', UI_HOST_BUNDLE],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        (BASE / 'run/ui-window.owner').unlink()
    except OSError:
        pass
    with (BASE / 'log/apt.log').open('a') as log:
        log.write('\nWINDOW_CLOSED_AFTER_APP_EXIT\n')
    state['had_client'] = False
    state['idle_since'] = 0.0

def window_watchdog_start():
    global window_watchdog_thread
    if window_watchdog_thread and window_watchdog_thread.is_alive():
        return {'state': 'running'}
    window_watchdog_thread = threading.Thread(target=window_watchdog_loop, daemon=True)
    window_watchdog_thread.start()
    return {'state': 'started'}

def window_watchdog_loop():
    while True:
        time.sleep(5)
        if (BASE / 'config/disabled').exists():
            continue
        try:
            window_watchdog_tick()
        except Exception as error:
            with (BASE / 'log/apt.log').open('a') as log:
                log.write('\nWINDOW_WATCHDOG_ERROR ' + str(error) + '\n')

def ui_stop():
    subprocess.run(['/system/bin/aa','force-stop',UI_HOST_BUNDLE],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    for client in ui_clients():
        try:os.kill(client['pid'],15)
        except OSError:pass
    time.sleep(0.5)
    stopped=stop_linux_programs()
    return {'state':'stopped','hostPid':ui_host_pid(),'stoppedPids':stopped}

def resources():
    global resource_sample
    config=settings();data={'memoryLimitBytes':config['memoryMiB']*1048576,'pidsLimit':config['pidsLimit'],'memoryBytes':0,'pids':0}
    for path in Path('/dev/memcg/l410-linux').glob('run-*/memory.usage_in_bytes'):
        try:data['memoryBytes']+=int(path.read_text())
        except OSError:pass
    for path in Path('/dev/pids/l410-linux').glob('run-*/pids.current'):
        try:data['pids']+=int(path.read_text())
        except OSError:pass
    samples={}
    for path in Path('/dev/pids/l410-linux').glob('run-*/cgroup.procs'):
        try:pids=path.read_text().split()
        except OSError:continue
        for pid in pids:
            try:
                fields=(Path('/proc')/pid/'stat').read_text().rsplit(')',1)[1].split()
                samples[(pid,fields[19])]=int(fields[11])+int(fields[12])
            except (OSError,ValueError,IndexError):pass
    now=time.monotonic();previous_time,previous=resource_sample
    ticks=sum(max(0,count-previous.get(key,count)) for key,count in samples.items())
    data['cpuPercent']=round(100*ticks/os.sysconf('SC_CLK_TCK')/max(.001,now-previous_time),1)
    resource_sample=(now,samples)
    return data

def sensors():
    result=[]
    for path in Path('/sys/class/thermal').glob('thermal_zone*'):
        try:
            name=(path/'type').read_text().strip()
            if name not in ('soc_thermal','gpu','Battery'):continue
            raw=int((path/'temp').read_text())
            result.append({'name':name,'celsius':raw/1000,'source':str(path)})
        except (OSError,ValueError):pass
    return result

def prepare_native_surface(owner,endpoint):
    identity=native_identity(owner)
    if identity is None or endpoint!='l410-native-display-'+identity[0]:
        raise ValueError('原生显示端点与当前鸿蒙窗口不匹配')
    if any(i['owner']==owner for i in live_instances().values()):
        raise ValueError('请先停止当前应用再绑定显示会话')
    for key in list(native_sessions):
        if time.monotonic()-native_sessions[key]['created']>60:native_sessions.pop(key)
    session={'endpoint':endpoint,'key':secrets.token_urlsafe(24),'identity':identity,'created':time.monotonic()}
    native_sessions[owner]=session
    return {'endpoint':endpoint,'key':session['key']}

def start(app_name="mousepad",owner=None,native=False):
    global bridge,current_app,current_owner
    with lock:
        if app_name=='registered-launcher':
            desktop_id=next((key for key,value in launchers().items() if value['bundle']==owner),None)
            if not desktop_id:raise ValueError('此入口尚未关联 Linux 应用')
            app_name='desktop:'+desktop_id
        if app_name=='desktop:org.xfce.mousepad.desktop':app_name='mousepad'
        launch=None
        if app_name.startswith('desktop:'):
            launch=next((a for a in apps() if a['id']==app_name[8:]),None)
            if not launch or not launch['supported']:raise ValueError(launch['reason'] if launch else 'desktop entry missing')
        elif app_name not in ('mousepad','glxgears','vulkantriangle','vkcube'):raise ValueError('unsupported application')
        if app_name=='mousepad':launch=next((a for a in apps() if a['id']=='org.xfce.mousepad.desktop'),None)
        render_profile=render_profiles.selected(app_name,launch)
        if render_profile!='software' and not settings()['gpu']:raise ValueError('GPU 访问已关闭；请开启 GPU 或为此应用选择默认软件显示')
        active=live_instances()
        identity=native_identity(owner)
        surface=None
        if isinstance(owner,str) and owner.startswith('com.l410.linux') and identity is None:
            raise ValueError('无法跟踪鸿蒙窗口进程；检查宿主 /proc 读取组')
        if app_name in active and active[app_name]['owner']==owner and active[app_name]['nativeIdentity']!=identity:
            stop(owner);active=live_instances()
        if app_name in active:
            instance=active[app_name]
            if instance['owner']!=owner:raise ValueError('应用已在另一鸿蒙窗口运行，请激活已有窗口或先停止该应用')
            if bool(native)!=instance.get('nativeDisplay',False):raise ValueError('应用已使用另一显示路径运行')
            bridge=instance['process'];current_app=app_name;current_owner=owner
            return instance_reply(app_name,instance,'running')
        if native:
            surface=native_sessions.get(owner)
            if render_profile not in ('gl4es','software') or surface is None or surface['identity']!=identity or time.monotonic()-surface['created']>60:
                raise ValueError('原生显示会话已失效或当前渲染方式不支持')
        if owner and any(i['owner']==owner for i in active.values()):raise ValueError('当前鸿蒙窗口已运行另一应用，请先停止；独立菜单入口可同时运行')
        if len(active)>=4:raise ValueError('当前最多同时运行四个 Linux 应用')
        if job and job.poll() is None:raise ValueError('apt is running')
        used={i['port'] for i in active.values()}
        port=None
        for candidate in [18410]+list(range(18412,18440)):
            if candidate in used:continue
            with socket.socket() as probe:
                probe.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
                try:probe.bind(('127.0.0.1',candidate));port=candidate;break
                except OSError:continue
        if port is None:raise ValueError('no free application bridge port')
        log_path=BASE/'log'/('app-'+re.sub(r'[^a-zA-Z0-9_.-]','_',app_name)+'.log')
        log=log_path.open('wb',buffering=0)
        key=secrets.token_urlsafe(24)
        args=ENTER+(['--gpu'] if render_profile!='software' else [])+['/usr/bin/python3','/root/window_bridge.py',app_name,json.dumps(settings()),json.dumps(launch),str(port),key]
        if surface:args.append(json.dumps({'endpoint':surface['endpoint'],'key':surface['key']}))
        bridge=subprocess.Popen(args,env=launch_environment(owned=True),
            stdin=subprocess.DEVNULL,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
        log.close()
        current_app=app_name
        current_owner=owner
        instance={'process':bridge,'owner':owner,'port':port,'key':key,'log':log_path,'created':time.monotonic(),'nativeIdentity':identity,'name':launch['name'] if launch else {'mousepad':'Mousepad','glxgears':'OpenGL 齿轮','vulkantriangle':'Vulkan 三角形','vkcube':'Vulkan 立方体'}[app_name]}
        instances[app_name]=instance
        instance['renderProfile']=render_profile
        instance['nativeDisplay']=bool(surface)
        if surface:native_sessions.pop(owner,None)
        return instance_reply(app_name,instance,'starting')

def live_instances():
    return {name:instance for name,instance in instances.copy().items() if instance['process'].poll() is None}

def native_builder_enabled():
    return (BASE/'entry-builder/enabled').is_file()

def native_builder_worker():
    global native_build,native_build_busy
    while True:
        time.sleep(1)
        if not native_builder_enabled():continue
        request=None
        try:
            with lock:
                if (BASE/'config/disabled').exists():continue
                if job and job.poll() is None:continue
                recovered=managed_jobs.recover('build')
                if recovered and not recovered.data.get('handled'):
                    request=recovered.metadata;native_build=recovered;native_build_busy=True
                else:
                    request=entry_builds.claim('device')['job']
                    if not request:continue
                    native_build_busy=True
                    native_build=None
                work=BASE/'entry-build-work'
                work.mkdir(parents=True,exist_ok=True)
                if native_build is None:
                    (work/'job.json').write_text(json.dumps(request))
                    (work/'result.json').unlink(missing_ok=True)
                    env=launch_environment();env['L410_SHARED_MODE']='off'
                    native_build=managed_jobs.launch('build',ENTER+['--builder','/usr/bin/python3','/run/l410-builder/device_entry_build.py'],env,BASE/'log/entry-build.log',request)
            try:code=native_build.wait(timeout=180)
            except subprocess.TimeoutExpired:
                native_build.terminate();native_build.wait(timeout=10);raise ValueError('设备入口构建超时')
            if code:raise ValueError('设备构建失败，查看 log/entry-build.log')
            result=json.loads((work/'result.json').read_text())
            source=(work/result['hap']).resolve()
            if not source.is_relative_to(work.resolve()) or result['bundle']!=request['bundle']:raise ValueError('invalid build result')
            with entry_lock:
                target=BASE/'launchers'/(request['bundle']+'.hap')
                shutil.copyfile(source,target);target.chmod(0o644)
                registry=launchers();registry[request['id']]={key:request[key] for key in ('name','bundle')}
                path=BASE/'config/launchers.json';candidate=path.with_suffix('.new');candidate.write_text(json.dumps(registry,ensure_ascii=False));candidate.replace(path)
            entry_builds.finish(request['id'],None,manage_entry)
        except Exception as error:
            print('NATIVE_BUILD_ERROR',str(error),flush=True)
            if request:entry_builds.finish(request['id'],str(error),manage_entry)
        finally:
            with lock:
                if native_build:native_build.handled()
                native_build_busy=False;native_build=None

def tray_host_worker():
    """Build hidden native StatusBar owners from the existing SNI snapshot."""
    global native_build_busy
    seen_generation=-1
    while True:
        time.sleep(1)
        request=None
        try:
            try:
                state=json.loads(ANCO_TRAY_STATE.read_text())
            except FileNotFoundError:
                # The SNI bridge creates its v1 snapshot on the first publish.
                # Before any Linux application registers a tray item, absence
                # is the same valid empty state used by tray_snapshot().
                state={'version':1,'generation':0,'items':[]}
            generation=int(state.get('generation',0))
            if generation!=seen_generation:
                seen_generation=generation
                for item in state.get('items',[]):
                    if isinstance(item,dict) and item.get('status')!='Passive':
                        tray_hosts.queue(item.get('bundle'))
            with lock:
                if native_build_busy or (job and job.poll() is None):continue
                request=tray_hosts.claim()
                if not request:continue
                native_build_busy=True
                work=BASE/'entry-build-work'
                work.mkdir(parents=True,exist_ok=True)
                (work/'job.json').write_text(json.dumps(request))
                (work/'result.json').unlink(missing_ok=True)
            env=launch_environment();env['L410_SHARED_MODE']='off'
            result=subprocess.run(ENTER+['--builder','/usr/bin/python3',
                '/run/l410-builder/device_entry_build.py'],env=env,
                stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=180)
            if result.returncode:raise ValueError('设备托盘宿主构建失败：'+result.stdout[-1000:])
            built=json.loads((work/'result.json').read_text())
            source=(work/built['hap']).resolve()
            if not source.is_relative_to(work.resolve()) or built['bundle']!=request['bundle']:
                raise ValueError('托盘宿主构建结果无效')
            record=tray_hosts.finish(request['targetBundle'],source)
            install=subprocess.run(['/system/bin/sh',str(CODE/'manage-entry.sh'),
                'enable',record['bundle'],record['hap']],capture_output=True,text=True,timeout=60)
            if install.returncode:
                raise ValueError('托盘宿主安装失败：'+((install.stdout or '')+(install.stderr or ''))[-1000:])
            if not tray_hosts.installed(record['bundle']):
                raise ValueError('托盘宿主安装后不可见')
            print('TRAY_HOST_READY semantic=%s native=%s' %
                  (request['targetBundle'],record['bundle']),flush=True)
        except (OSError,ValueError,subprocess.SubprocessError) as error:
            print('TRAY_HOST_ERROR '+str(error),flush=True)
            if request:
                try:tray_hosts.finish(request['targetBundle'],'',error)
                except Exception:pass
        finally:
            if request:
                with lock:native_build_busy=False

def native_identity(owner):
    if not isinstance(owner,str) or not re.fullmatch(r'com\.l410\.linux(?:\.[a-z][a-z0-9_]*)?',owner):return None
    for path in Path('/proc').glob('[0-9]*/cmdline'):
        try:
            if path.read_bytes().split(b'\0',1)[0].decode(errors='replace')==owner:
                fields=(path.parent/'stat').read_text().rsplit(')',1)[1].split()
                return (path.parent.name,fields[19])
        except (OSError,IndexError):pass
    return None

def watch_native_owners():
    while True:
        time.sleep(1)
        with lock:
            file_exchange.clean()
            for instance in list(live_instances().values()):
                owner=instance['owner']
                if not isinstance(owner,str) or not owner.startswith('com.l410.linux'):continue
                identity=instance['nativeIdentity']
                if identity is None:continue
                try:
                    fields=Path('/proc',identity[0],'stat').read_text().rsplit(')',1)[1].split()
                    missing=fields[19]!=identity[1] or fields[0]=='Z'
                except (OSError,IndexError):missing=True
                if missing:
                    print('NATIVE_OWNER_EXIT_CLEANUP',owner,flush=True)
                    stop(owner)

def instance_reply(name,instance,state='running'):
    return {'state':state,'pid':instance['process'].pid,'app':name,'owner':instance['owner'],'port':instance['port'],'key':instance['key'],'name':instance['name'],'settings':settings(),'renderProfile':instance['renderProfile']}

def owned_status(owner):
    with lock:
        found=max(((n,i) for n,i in instances.items() if i['owner']==owner),key=lambda pair:(pair[1]['process'].poll() is None,pair[1]['created']),default=None)
        if not found:return {'running':False,'exit':None,'output':'此窗口没有运行的 Linux 应用'}
        name,instance=found;code=instance['process'].poll()
        output=instance['log'].read_text(errors='replace')[-4000:] if code is not None else ''
        return {'running':code is None,'app':name,'exit':code,'output':output}

def console_state():
    """State for the manager UI, sourced from the original AnCo runtime path."""
    runtime=original_anco_runtime_state()
    builds=entry_builds.status()
    return {'engineRunning':runtime['running'],'enginePids':runtime['pids'],
            'enginePhase':runtime['phase'],'engineOperation':runtime['operation'],
            'runnerPids':runtime['runnerPids'],'serviceParameter':runtime['serviceParameter'],
            'apps':apps(),'aptRunning':bool(job and job.poll() is None),
            'aptExit':job.poll() if job else None,
            'entryBuildRunning':native_build_busy or any(
                item.get('state') in ('queued','building') for item in builds['builds']),
            'entryBuilds':builds['builds'],'workerOnline':builds['workerOnline'],
            'workerMode':builds['workerMode'],'profiles':render_profiles.PROFILES,
             'settings':settings(),'resources':resources(),'sensors':sensors(),
             'ssh':ssh_manager.status(),
            'root':str(ROOT),'sharedDirectory':'/data/service/el2/100/hmdfs/account/files/Docs/Huawei Share/Linux 兼容共享'}


def console_log():
    sections=[]
    runtime=original_anco_runtime_state()
    for path in runtime['runnerLogs']:
        try:text=Path(path).read_text(errors='replace')[-18000:]
        except OSError:text='（暂无输出）'
        sections.append('===== 兼容服务运行日志 =====\n'+text)
    if not runtime['runnerLogs']:sections.append('===== 兼容服务运行日志 =====\n（当前没有运行中的兼容服务 runner）')
    for title,path in (('服务操作',ANCO_CONTROL_LOG),('管理服务',BASE/'log/broker.log')):
        try:text=path.read_text(errors='replace')[-18000:]
        except OSError:text='（暂无输出）'
        sections.append('===== '+title+' =====\n'+text)
    return '\n\n'.join(sections)[-48000:]


def hishell_pid():
    for proc in Path('/proc').glob('[0-9]*'):
        try:command=(proc/'cmdline').read_bytes().split(b'\0',1)[0].decode(errors='replace')
        except OSError:continue
        if command==HISHELL_BUNDLE:return int(proc.name)
    return 0


def wait_hishell(timeout=8):
    deadline=time.monotonic()+timeout
    pid=hishell_pid()
    while not pid and time.monotonic()<deadline:
        time.sleep(.2);pid=hishell_pid()
    return pid


def hishell_files_directory():
    matches=[path for path in Path('/data/app/el2').glob('*/base/'+HISHELL_BUNDLE+'/files') if path.is_dir()]
    if len(matches)!=1:raise RuntimeError('无法确定 HiShell 数据目录')
    return matches[0]


def shell_peer_allowed(conn):
    try:
        pid,uid,_=struct.unpack('3i',conn.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12))
        domain=Path('/proc/%d/attr/current'%pid).read_text(errors='replace').strip()
        command=Path('/proc/%d/cmdline'%pid).read_bytes().split(b'\0',1)[0].decode(errors='replace')
    except (OSError,struct.error):return False
    return uid>0 and domain.startswith('u:r:hishell_hap:') and command=='/bin/zsh'


def shell_session(conn):
    global shell_connection_count
    child=0;master=-1
    try:
        if not shell_peer_allowed(conn):return
        conn.settimeout(3)
        if conn.recv(64)!=b'L410_HISHELL_V1\n':return
        conn.settimeout(None)
        child,master=pty.fork()
        if child==0:
            environment=launch_environment()
            environment.update(HOME='/home/linux',USER='linux',LOGNAME='linux',TERM='xterm-256color',SHELL='/bin/bash')
            os.execve(ENTER[0],ENTER+['/usr/bin/python3','-c',PTY_CHILD],environment)
        fcntl.ioctl(master,termios.TIOCSWINSZ,struct.pack('HHHH',36,120,0,0))
        with shell_connection:
            shell_connection_count+=1;shell_connection.notify_all()
        while True:
            ready,_,_=select.select([conn,master],[],[])
            if conn in ready:
                data=conn.recv(8192)
                if not data:break
                os.write(master,data)
            if master in ready:
                try:data=os.read(master,8192)
                except OSError:break
                if not data:break
                conn.sendall(data)
    except OSError:
        pass
    finally:
        try:conn.close()
        except OSError:pass
        if master>=0:
            try:os.close(master)
            except OSError:pass
        if child>0:
            try:os.kill(child,signal.SIGHUP)
            except OSError:pass
            try:os.waitpid(child,0)
            except OSError:pass


def serve_hishell():
    server=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
    server.bind('\0'+HISHELL_SOCKET);server.listen(8)
    while True:
        try:connection,_=server.accept()
        except OSError:continue
        threading.Thread(target=shell_session,args=(connection,),daemon=True).start()


def open_linux_shell():
    runtime=original_anco_runtime_state()
    if not runtime['running']:raise ValueError('Linux 兼容服务尚未就绪，请先启动服务')
    if not HISHELL_CLIENT_SOURCE.is_file():raise ValueError('Linux Shell 客户端缺失')
    pid=hishell_pid()
    if not pid:
        subprocess.run(['/system/bin/aa','start','-b',HISHELL_BUNDLE,'-a','EntryAbility'],capture_output=True,text=True,timeout=10,check=True)
        pid=wait_hishell()
    if not pid:raise RuntimeError('HiShell 未能启动')
    files=hishell_files_directory();client=files/'l410-hishell-client'
    shutil.copyfile(HISHELL_CLIENT_SOURCE,client)
    owner=Path('/proc/%d'%pid).stat().st_uid
    os.chown(client,owner,owner);os.chmod(client,0o750)
    inside='/data/storage/el2/base/files/l410-hishell-client'
    nsenter=['/system/bin/nsenter','-t',str(pid),'-m','--']
    subprocess.run(nsenter+['/system/bin/umount','/bin/zsh'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    before=shell_connection_count
    try:
        mounted=subprocess.run(nsenter+['/system/bin/mount','--bind',inside,'/bin/zsh'],capture_output=True,text=True,timeout=10)
        if mounted.returncode:raise RuntimeError('HiShell Shell 接线失败：'+((mounted.stdout or '')+(mounted.stderr or '')).strip())
        started=subprocess.run(['/system/bin/aa','start','-b',HISHELL_BUNDLE,'-a','EntryAbility','--pb','openInNewWindow','true'],capture_output=True,text=True,timeout=10)
        if started.returncode:raise RuntimeError('HiShell 窗口启动失败：'+((started.stdout or '')+(started.stderr or '')).strip())
        deadline=time.monotonic()+6
        with shell_connection:
            while shell_connection_count==before and time.monotonic()<deadline:
                shell_connection.wait(deadline-time.monotonic())
        if shell_connection_count==before:raise RuntimeError('HiShell 已启动，但 Linux PTY 未连接')
    finally:
        subprocess.run(nsenter+['/system/bin/umount','/bin/zsh'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    return {'state':'opened','terminal':'HiShell','transport':'PTY','message':'Linux Shell 已在 HiShell 新窗口中打开'}


def terminal_reader(session_id):
    with terminal_lock:session=terminal_sessions.get(session_id)
    if not session:return
    descriptor=session['fd']
    try:
        while True:
            try:data=os.read(descriptor,8192)
            except OSError:break
            if not data:break
            with terminal_lock:
                current=terminal_sessions.get(session_id)
                if not current:break
                current['buffer'].extend(data)
                if len(current['buffer'])>262144:
                    removed=len(current['buffer'])-196608
                    del current['buffer'][:removed];current['base']+=removed
    finally:
        with terminal_lock:
            current=terminal_sessions.get(session_id)
            if current:current['active']=False
        try:os.close(descriptor)
        except OSError:pass
        try:os.waitpid(session['pid'],0)
        except OSError:pass


def process_children(pid):
    try:return [int(value) for value in Path('/proc/%d/task/%d/children'%(pid,pid)).read_text().split()]
    except OSError:return []


def terminal_foreground(session_id):
    deadline=time.monotonic()+5
    while time.monotonic()<deadline:
        with terminal_lock:session=terminal_sessions.get(session_id)
        if not session or not session['active']:return
        pending=[session['pid']];seen=set();groups=[]
        while pending:
            pid=pending.pop(0)
            if pid in seen:continue
            seen.add(pid);pending.extend(process_children(pid))
            try:
                fields=Path('/proc/%d/stat'%pid).read_text().rsplit(')',1)[1].split()
                group=int(fields[2]);sid=int(fields[3])
                if sid==session['pid'] and group!=session['pid']:groups.append(group)
            except (OSError,IndexError,ValueError):pass
        if groups:
            group=groups[-1]
            try:
                os.tcsetpgrp(session['fd'],group);os.killpg(group,signal.SIGCONT)
                return
            except OSError:pass
        time.sleep(.05)


def terminal_create():
    runtime=original_anco_runtime_state()
    if not runtime['running']:raise ValueError('Linux 兼容服务尚未就绪，请先启动服务')
    session_id=secrets.token_hex(12)
    child,master=pty.fork()
    if child==0:
        environment=launch_environment()
        environment.update(HOME='/home/linux',USER='linux',LOGNAME='linux',TERM='xterm-256color',SHELL='/bin/bash',PS1='linux@l410:\\w$ ')
        os.execve(ENTER[0],ENTER+['/usr/bin/python3','-c',PTY_CHILD],environment)
    fcntl.ioctl(master,termios.TIOCSWINSZ,struct.pack('HHHH',32,110,0,0))
    with terminal_lock:
        terminal_sessions[session_id]={'pid':child,'fd':master,'buffer':bytearray(),'base':0,'active':True,'created':time.time()}
    threading.Thread(target=terminal_reader,args=(session_id,),daemon=True).start()
    threading.Thread(target=terminal_foreground,args=(session_id,),daemon=True).start()
    return {'id':session_id,'pid':child,'active':True,'offset':0,'output':''}


def terminal_read(session_id,offset):
    if not isinstance(session_id,str) or not re.fullmatch(r'[0-9a-f]{24}',session_id):raise ValueError('Shell 会话无效')
    if type(offset) is not int or offset<0:raise ValueError('Shell 输出位置无效')
    with terminal_lock:
        session=terminal_sessions.get(session_id)
        if not session:raise ValueError('Shell 会话不存在或已关闭')
        start=max(offset,session['base']);index=start-session['base'];data=bytes(session['buffer'][index:])
        next_offset=session['base']+len(session['buffer']);active=session['active'];pid=session['pid']
    text=data.decode('utf-8',errors='replace').replace('\r','')
    text=re.sub(r'\x1b\][^\x07]*(?:\x07|\x1b\\)','',text)
    text=re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]','',text)
    return {'id':session_id,'pid':pid,'active':active,'offset':next_offset,'output':text,'truncated':offset<start}


def terminal_write(session_id,data):
    if not isinstance(data,str) or len(data.encode('utf-8'))>2048:raise ValueError('Shell 输入过长')
    with terminal_lock:
        session=terminal_sessions.get(session_id)
        if not session or not session['active']:raise ValueError('Shell 会话已经结束')
        descriptor=session['fd']
    os.write(descriptor,data.encode('utf-8'))
    return {'id':session_id,'written':len(data),'active':True}


def terminal_close(session_id):
    with terminal_lock:session=terminal_sessions.pop(session_id,None)
    if not session:return {'id':session_id,'active':False}
    try:os.kill(session['pid'],signal.SIGHUP)
    except OSError:pass
    try:os.close(session['fd'])
    except OSError:pass
    return {'id':session_id,'active':False}


def terminal_close_all():
    with terminal_lock:ids=list(terminal_sessions)
    for session_id in ids:terminal_close(session_id)


def close_window(owner=None):
    """Close the application window.

    The control host owns the window now, so closing it means stopping that host
    and the Linux program behind it; the old single window service used to be
    asked over a TCP port, which is exactly what this project no longer does.
    """
    subprocess.run(['/system/bin/aa', 'force-stop', UI_HOST_BUNDLE],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    stopped = stop_linux_programs()
    return {'closed': True, 'stoppedPids': stopped}


def stop(owner=None):
    global bridge
    with lock:
        for instance in live_instances().values():
            if owner is not None and owner!=instance['owner']:continue
            process=instance['process']
            process.terminate()
            try:process.wait(timeout=8)
            except subprocess.TimeoutExpired:
                # Direct child is the private PID namespace init. Verify ns differs.
                children=Path(f'/proc/{process.pid}/task/{process.pid}/children').read_text().split()
                for child in children:
                    if os.readlink(f'/proc/{child}/ns/pid')!=os.readlink('/proc/1/ns/pid'):
                        os.kill(int(child),signal.SIGKILL)
                process.wait(timeout=3)
        return {'state':'stopped'}

class ManagerCall:
    """One management request. Routes are unchanged; the transport is a line of
    JSON on an abstract socket, not HTTP, and the caller is identified by the
    kernel's peer credentials instead of a token in a header."""
    def __init__(self,path,method,raw):
        self.path=path
        self.command=method
        self.headers={'Content-Length':str(len(raw))}
        self.rfile=io.BytesIO(raw)
        self.captured=None
    def authorized(self):
        return True
    def log_message(self,format,*args):
        return
    def reply(self,data,status=200):
        if status>=400:
            message=data.get('error') if isinstance(data,dict) else str(data)
            self.captured={'ok':False,'error':message or ('status %d'%status),'status':status}
        else:
            self.captured={'ok':True,'data':data,'status':status}
    def send_error(self,code,message=None):
        self.captured={'ok':False,'error':message or ('status %d'%code),'status':code}
    def do_GET(self):
        if not self.authorized():return
        if self.path=='/console/status':window_watchdog_start();self.reply(console_state())
        elif self.path=='/console/log':self.reply({'output':console_log()})
        elif self.path=='/status':
            active=live_instances();environment=original_anco_runtime_state()
            self.reply({'running':environment['running'],'instances':[instance_reply(n,i) for n,i in active.items()],
                'pid':environment['pids'][0] if environment['pids'] else None,
                'aptRunning':bool(job and job.poll() is None),'aptExit':job.poll() if job else None,'entryBuildRunning':native_build_busy,
                'root':str(ROOT),'currentApp':current_app,'apps':apps(),'resources':resources(),
                'sharedDirectory':'/data/service/el2/100/hmdfs/account/files/Docs/Huawei Share/Linux 兼容共享','sensors':sensors(),'mousepadEntry':entry_enabled()})
        elif self.path=='/apps':self.reply({'apps':apps()})
        elif self.path=='/ui/sessions':self.reply(ui_state())
        elif self.path=='/console/resources':self.reply({'resources':resources(),'sensors':sensors()})
        elif self.path=='/ssh/status':self.reply(ssh_manager.status())
        elif self.path=='/ssh/log':self.reply({'output':ssh_manager.LOG_FILE.read_text(errors='replace')[-24000:] if ssh_manager.LOG_FILE.exists() else ''})
        elif self.path=='/ssh/client-key/export':self.reply(ssh_manager.export_client_private_key())
        elif self.path=='/settings':self.reply({'settings':settings()})
        elif self.path=='/entry/build-status':self.reply(entry_builds.status())
        elif self.path=='/entry/health':self.reply(entry_health())
        elif self.path=='/ui/stop':self.reply(ui_stop())
        elif self.path=='/stop':self.reply(stop(None))
        elif self.path=='/ui/tap-pending':self.reply(ui_tap_take())
        elif self.path=='/ui/tap':self.reply(ui_tap_set(0,0))
        elif self.path=='/audio/relay':self.reply(dict(audio_relay_start(),bytesForwarded=audio_relay_bytes[0]))
        elif self.path=='/apt/log':self.reply({'output':(BASE/'log/apt.log').read_text(errors='replace')[-24000:] if (BASE/'log/apt.log').exists() else ''})
        elif self.path=='/hap/status':self.reply(hap_installer.status())
        else:self.send_error(404)
    def do_POST(self):
        # Serialize mutations with builder claims and maintenance transitions.
        # Frame/audio traffic uses separate per-application bridge servers.
        # A desktop launch only validates metadata and hands one packet to the
        # resident runtime.  Do not keep its visible one-shot UIAbility waiting
        # behind unrelated apt, build or maintenance work.
        if self.path=='/anco/launch':self.post_locked()
        else:
            with lock:self.post_locked()
    def post_locked(self):
        global job
        if not self.authorized():return
        try:
            n=int(self.headers.get('Content-Length','0'))
            if n>(90000 if self.path.startswith('/files/') else 4096):raise ValueError('request too large')
            body=json.loads(self.rfile.read(n) or b'{}')
            if (BASE/'config/disabled').exists() and self.path not in ('/stop','/console/stop','/close','/owned-status','/maintenance'):
                raise ValueError('平台正在维护或已卸载；恢复平台后再试')
            if self.path=='/console/start':
                self.reply(request_anco_operation('start'))
            elif self.path=='/console/stop':self.reply(request_anco_operation('stop'))
            elif self.path=='/console/restart':self.reply(request_anco_operation('restart'))
            elif self.path=='/ssh/enabled':self.reply(ssh_manager.set_enabled(body.get('enabled')))
            elif self.path=='/ssh/port':self.reply(ssh_manager.change_port(body.get('port')))
            elif self.path=='/ssh/regenerate-host-key':self.reply(ssh_manager.regenerate_host_key())
            elif self.path=='/ssh/client-key/generate':self.reply(ssh_manager.generate_client_key())
            elif self.path=='/ssh/self-test':self.reply(ssh_manager.self_test())
            elif self.path=='/shell/open':self.reply(open_linux_shell())
            elif self.path=='/shell/create':self.reply(terminal_create())
            elif self.path=='/shell/read':self.reply(terminal_read(body.get('id'),body.get('offset',0)))
            elif self.path=='/shell/write':self.reply(terminal_write(body.get('id'),body.get('data','')))
            elif self.path=='/shell/close':self.reply(terminal_close(body.get('id')))
            elif self.path=='/tray/status':self.reply(tray_snapshot(body.get('bundle')))
            elif self.path=='/tray/action':self.reply(tray_action(body))
            elif self.path=='/anco/launch':self.reply(anco_launch(body.get('id')))
            elif self.path=='/audio/relay':self.reply(dict(audio_relay_start(),bytesForwarded=audio_relay_bytes[0]))
            elif self.path=='/audio/capture':self.reply(dict(audio_capture_relay_start(),bytesForwarded=audio_capture_bytes[0]))
            elif self.path=='/clipboard/relay':self.reply(clipboard_relay_start())
            elif self.path=='/ui/tap':self.reply(ui_tap_set(body.get('x',0),body.get('y',0)))
            elif self.path=='/ui/tap-pending':self.reply(ui_tap_take())
            elif self.path=='/ui/type':self.reply(ui_type_set(body.get('id',0),body.get('text','')))
            elif self.path=='/ui/type-pending':self.reply(ui_type_take())
            elif self.path.startswith('/files/'):
                self.reply(file_exchange.dispatch(self.path[len('/files/'):],body,settings()['sharedMode']))
            elif self.path=='/maintenance':
                if (job and job.poll() is None) or native_build_busy or any(i['state']=='building' for i in entry_builds.status()['builds']):
                    raise ValueError('请等待 apt 和入口构建结束后再卸载')
                (BASE/'config/disabled').write_text('Maintenance: integration uninstall in progress; user data retained.\n')
                protocol_manager.stop();file_exchange.clean(all=True);stop();self.reply({'state':'maintenance','running':False})
            elif self.path=='/ui/launch':self.reply(ui_launch(body.get('app')))
            elif self.path=='/ui/stop':self.reply(ui_stop())
            elif self.path=='/native-surface':self.reply(prepare_native_surface(body.get('owner'),body.get('endpoint')))
            elif self.path=='/start':self.reply(start(body.get('app','mousepad'),body.get('owner'),body.get('nativeDisplay') is True))
            elif self.path=='/stop':self.reply(stop(body.get('owner')))
            elif self.path=='/close':self.reply(close_window(body.get('owner')))
            elif self.path=='/owned-status':self.reply(owned_status(body.get('owner')))
            elif self.path=='/app/render-profile':
                desktop_id=body.get('id');profile=body.get('profile')
                app=next((a for a in apps() if a['id']==desktop_id),None)
                if not app or not app['supported']:raise ValueError('应用不存在或暂不支持')
                app_name='mousepad' if desktop_id=='org.xfce.mousepad.desktop' else 'desktop:'+desktop_id
                if app_name in live_instances():raise ValueError('请先停止这个应用，再修改渲染方式')
                if job and job.poll() is None:raise ValueError('请等待 apt 完成')
                render_profiles.save(desktop_id,profile)
                self.reply({'apps':apps(),'output':app['name']+' 已选择 '+render_profiles.PROFILES[profile]+'，下次启动生效；实际兼容性以应用运行结果为准'})
            elif self.path=='/settings':
                with lock:
                    if protocol_manager.running() or live_instances() or (job and job.poll() is None) or native_build_busy:raise ValueError('请先停止 Linux 应用并等待 apt/入口构建完成')
                    validate_settings(body)
                    candidate=SETTINGS.with_suffix('.new');candidate.write_text(json.dumps(body));candidate.replace(SETTINGS)
                self.reply({'settings':settings(),'output':'配置已保存，下次启动生效'})
            elif self.path=='/entry':
                if not isinstance(body.get('enabled'),bool):raise ValueError('enabled must be boolean')
                desktop_id=body.get('id','org.xfce.mousepad.desktop')
                mapping=ensure_anco_target(desktop_id) if body['enabled'] else None
                restart_queued=False
                if mapping and mapping['changed'] and original_anco_runtime_state()['running']:
                    request_anco_operation('restart');restart_queued=True
                entry_builds.cancel(desktop_id)
                response=manage_entry(body['enabled'],desktop_id)
                response['output']=('卓易通虚拟入口已加入鸿蒙应用菜单' if body['enabled'] and response.get('enabled')
                    else '卓易通虚拟入口已从鸿蒙应用菜单移除')
                if restart_queued:response['output']+='；兼容服务正在重启以加载新应用'
                self.reply(response)
            elif self.path=='/entry/build-next':self.reply(entry_builds.claim('workstation'))
            elif self.path=='/entry/rebuild':
                desktop_id=body.get('id');registry=launchers();entry=registry.get(desktop_id)
                if not entry or not (BUILDER_PUBLIC/'templates'/entry['bundle']/'modules.abc').is_file():raise ValueError('此入口没有设备模板，需使用工作站构建脚本更新')
                if any(i['owner']==entry['bundle'] for i in live_instances().values()):raise ValueError('请先关闭此应用窗口')
                if not native_builder_enabled():raise ValueError('设备入口构建器未启用')
                self.reply(entry_builds.queue_app(desktop_id,registry,force=True))
            elif self.path=='/entry/build-result':self.reply(entry_builds.finish(body.get('id'),body.get('error'),manage_entry))
            elif self.path=='/apt':
                with lock:
                    if protocol_manager.running() or live_instances() or (job and job.poll() is None) or native_build_busy:raise ValueError('请先关闭 Linux 应用并等待其他 apt 操作完成')
                    action=body.get('action'); package=body.get('package','')
                    if action=='update':args=['update']
                    elif action in ('install','purge') and re.fullmatch(r'[a-z0-9][a-z0-9+.-]{0,100}',package):args=[action,'-y','--',package]
                    elif action=='install-local':
                        if not LOCAL_DEB_SOURCE.is_file() or LOCAL_DEB_SOURCE.is_symlink():raise ValueError('请选择有效的本地 deb 文件')
                        size=LOCAL_DEB_SOURCE.stat().st_size
                        if not 128<=size<=1073741824:raise ValueError('deb 文件大小无效')
                        with LOCAL_DEB_SOURCE.open('rb') as source:
                            if source.read(8)!=b'!<arch>\n':raise ValueError('所选文件不是 Debian 软件包')
                        LOCAL_DEB_DIR.mkdir(parents=True,exist_ok=True)
                        target=LOCAL_DEB_DIR/'selected.deb';shutil.copyfile(LOCAL_DEB_SOURCE,target);os.chmod(target,0o644)
                        args=['install','-y','--','/var/cache/l410-local-deb/selected.deb']
                    else:raise ValueError('unsupported apt request')
                    job=managed_jobs.launch('apt',ENTER+['/usr/bin/apt-get']+args,launch_environment(),BASE/'log/apt.log',{'action':action,'package':package,'localDeb':action=='install-local'})
                    threading.Thread(target=finish_apt,args=(job,),daemon=True).start()
                self.reply({'pid':job.pid},202)
            elif self.path=='/hap/inspect':self.reply(hap_installer.inspect())
            elif self.path=='/hap/install':self.reply(hap_installer.start(),202)
            else:self.send_error(404)
        except (ValueError,OSError,subprocess.SubprocessError) as e:self.reply({'error':str(e)},409)

# --- management transport: abstract AF_UNIX socket, one JSON line per request ---
MANAGER_SOCKET='l410-manager'

def manager_dispatch(path,method,raw):
    call=ManagerCall(path,method,raw)
    try:
        if method=='POST':
            if path=='/anco/launch':call.post_locked()
            else:
                with lock:call.post_locked()
        else:
            call.do_GET()
    except Exception as error:
        return {'ok':False,'error':str(error),'status':500}
    return call.captured or {'ok':False,'error':'no reply','status':500}

def peer_allowed(conn):
    """Only our own applications and root may talk to the management service."""
    try:
        credentials=conn.getsockopt(socket.SOL_SOCKET,socket.SO_PEERCRED,12)
    except OSError:
        return False
    pid=int.from_bytes(credentials[0:4],'little')
    uid=int.from_bytes(credentials[4:8],'little')
    if uid==0:return True
    try:
        command=Path('/proc/%d/cmdline'%pid).read_bytes().replace(b'\0',b' ').decode(errors='replace')
    except OSError:
        return False
    return 'com.l410.linux' in command

def manager_client(conn):
    if not peer_allowed(conn):
        with conn:conn.sendall(b'{"ok":false,"error":"permission denied"}\n')
        return
    with conn:
        pending=b''
        while True:
            chunk=conn.recv(65536)
            if not chunk:return
            pending+=chunk
            while b'\n' in pending:
                line,pending=pending.split(b'\n',1)
                if not line.strip():continue
                try:
                    request=json.loads(line)
                    body=request.get('body')
                    method='POST' if (request.get('method')=='POST' or body is not None) else 'GET'
                    raw=json.dumps(body if body is not None else {}).encode()
                    answer=manager_dispatch(request.get('path',''),method,raw)
                except Exception as error:
                    answer={'ok':False,'error':str(error),'status':400}
                try:
                    conn.sendall(json.dumps(answer,ensure_ascii=False).encode()+b'\n')
                except OSError:
                    return

def serve_manager():
    server=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM)
    server.bind('\0'+MANAGER_SOCKET)
    server.listen(8)
    while True:
        try:
            conn,_=server.accept()
        except OSError:
            continue
        threading.Thread(target=manager_client,args=(conn,),daemon=True).start()

def terminate_broker(signum,frame):
    raise SystemExit(0)
signal.signal(signal.SIGTERM,terminate_broker)
signal.signal(signal.SIGINT,terminate_broker)

try:
    recovered_build=managed_jobs.recover('build')
    native_build_busy=bool(recovered_build and not recovered_build.data.get('handled'))
    if job and not job.data.get('handled'):threading.Thread(target=finish_apt,args=(job,),daemon=True).start()
    threading.Thread(target=watch_native_owners,daemon=True).start()
    threading.Thread(target=native_builder_worker,daemon=True).start()
    threading.Thread(target=tray_host_worker,daemon=True).start()
    threading.Thread(target=serve_hishell,daemon=True).start()
    serve_manager()
finally:
    terminal_close_all()
    protocol_manager.stop()
    file_exchange.clean(all=True)
    stop()
