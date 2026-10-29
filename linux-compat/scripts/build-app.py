from pathlib import Path
import json, subprocess, zipfile, secrets, sys, shutil, argparse, re

b=Path(__file__).resolve().parents[1]; root=b.parent
parser=argparse.ArgumentParser();parser.add_argument('--launcher',action='store_true');parser.add_argument('--descriptor',type=Path);parser.add_argument('--unbundled-records',action='store_true',help='Experimental reusable bytecode record naming');parser.add_argument('--native-display',action='store_true');parser.add_argument('--window-probe',action='store_true');parser.add_argument('--rootless',action='store_true');parser.add_argument('--protocol',action='store_true')
parser.add_argument('--console',action='store_true')
parser.add_argument('--hap-installer',action='store_true',help='Standalone Harmony HAP package installer')
parser.add_argument('--entry',action='store_true',help='Menu entry that starts a Linux app in its own native window')
parser.add_argument('--window-service',action='store_true',help='Native window: one XComponent is one Linux window')
parser.add_argument('--ui-probe',action='store_true',help='Native controls: ArkUI component tree built from C')
parser.add_argument('--ui-host',action='store_true',help='Control host: widget protocol turned into ArkUI controls')
parser.add_argument('--ohos-window',action='store_true',help='Window host: one XComponent is one Linux window surface')
parser.add_argument('--engine-host',action='store_true',help='Compat engine host: host broker creating one HarmonyOS window per Linux window')
parser.add_argument('--typed-lib',type=Path,help='Private typed Surface adapter candidate for an engine-host build')
args=parser.parse_args()
if args.typed_lib and not args.engine_host:parser.error('--typed-lib requires --engine-host')
if args.protocol:
    args.window_probe=True
    if args.rootless:parser.error("protocol and rootless are separate backends")
if args.entry and not args.descriptor:parser.error('--entry requires --descriptor')
if args.entry and (args.console or args.native_display or args.launcher):parser.error('--entry is a separate launcher kind')
if args.hap_installer and (args.console or args.entry or args.native_display or args.launcher or args.window_service or args.ui_probe or args.ui_host or args.ohos_window or args.engine_host):parser.error('--hap-installer is a separate application kind')
if args.window_service and (args.console or args.entry or args.native_display or args.launcher or args.protocol or args.rootless):
    parser.error('--window-service is a separate kind')
if args.ui_probe and (args.console or args.entry or args.native_display or args.launcher or args.protocol or args.rootless or args.window_service or args.ui_host):
    parser.error('--ui-probe is a separate kind')
if args.ui_host and (args.console or args.entry or args.native_display or args.launcher or args.protocol or args.rootless or args.window_service):
    parser.error('--ui-host is a separate kind')
if args.ohos_window and (args.console or args.entry or args.native_display or args.launcher or args.protocol or args.rootless or args.window_service or args.ui_probe or args.ui_host):
    parser.error('--ohos-window is a separate kind')
if args.engine_host and (args.console or args.entry or args.native_display or args.launcher or args.protocol or args.rootless or args.window_service or args.ui_probe or args.ui_host or args.ohos_window):
    parser.error('--engine-host is a separate kind')
if args.rootless:args.window_probe=True
if args.window_probe and (not args.native_display or args.launcher or args.descriptor):parser.error('--window-probe requires standalone --native-display')
descriptor=json.loads(args.descriptor.read_text(encoding='utf8')) if args.descriptor else None
launcher=args.launcher or (descriptor is not None and not args.entry)
bundle=descriptor['bundle'] if descriptor else ('com.l410.linux.mousepad' if launcher else 'com.l410.linux')
if descriptor and not re.fullmatch(r'com\.l410\.linux\.(?:app\.)?[a-z][a-z0-9_]{0,80}',bundle):raise ValueError('invalid launcher bundle')
if descriptor and not re.fullmatch(r'[A-Za-z0-9_.+-]{1,160}\.desktop',descriptor['id']):raise ValueError('invalid desktop id')
if args.entry:
    target_bundle=descriptor.get('targetBundle','');target_ability=descriptor.get('targetAbility','')
    if not re.fullmatch(r'org\.l410\.linux\.[a-z0-9_.]+',target_bundle):raise ValueError('invalid AnCo target bundle')
    if not target_ability.startswith(target_bundle+'.') or not re.fullmatch(r'[A-Za-z0-9_.]+',target_ability):raise ValueError('invalid AnCo target ability')
if args.native_display and not launcher:bundle='com.l410.linux.displaylab'
if args.console:bundle='com.l410.linux'
if args.hap_installer:bundle='com.l410.linux.hapinstaller'
if args.window_service:bundle='com.l410.linux.window'
if args.ui_probe:bundle='com.l410.linux.uiprobe'
if args.ui_host:bundle='com.l410.linux.uihost'
if args.ohos_window:bundle='com.l410.linux.ohoswindow'
if args.engine_host:bundle='com.l410.linux.engine'
label=descriptor['name'] if descriptor else ('Mousepad' if launcher else 'Linux 应用')
if args.console:label='Linux 兼容平台'
if args.hap_installer:label='HAP 安装器'
if args.window_service:label='Linux 窗口服务'
if args.ui_probe:label='原生控件验证'
if args.ui_host:label='Linux 控件宿主'
if args.ohos_window:label='鸿蒙窗口宿主'
if args.engine_host:label='Linux 兼容引擎'
app=b/('launcher-'+bundle.rsplit('.',1)[1] if launcher else ('entry-'+bundle.rsplit('.',1)[1] if args.entry else ('window-'+bundle.rsplit('.',1)[1] if args.window_service else ('ui-'+bundle.rsplit('.',1)[1] if (args.ui_probe or args.ui_host) else 'app')))); out=app/'out'
if args.ohos_window:
    app=b/'ohos-window-host';out=app/'out'
if args.engine_host:
    app=b/'engine-host';out=app/'out'
if args.hap_installer:
    app=b/'hap-installer';out=app/'out'
if args.native_display:
    app=b/('native-launcher-'+bundle.rsplit('.',1)[1] if launcher else 'display-lab');out=app/'out'
    if not launcher:label='Linux 共享缓冲区'
    if args.protocol:label='Linux 窗口服务'
r=app/'resources/base'
for p in (r/'element',r/'media',r/'profile',out/'ets'): p.mkdir(parents=True,exist_ok=True)
strings=[{'name':'app_name','value':label},{'name':'hardware_reason','value':'按已允许的配置，为 Linux 应用连接麦克风、摄像头或剪贴板'}]
if args.entry:strings.extend([{'name':'target_bundle','value':target_bundle},{'name':'target_ability','value':target_ability},{'name':'desktop_id','value':descriptor['id']}])
(r/'element/string.json').write_text(json.dumps({'string':strings},ensure_ascii=False),encoding='utf8')
(r/'media/linux.svg').write_text('<svg xmlns="http://www.w3.org/2000/svg" width="96" height="96"><rect width="96" height="96" rx="22" fill="#205BC7"/><path d="M22 28l20 20-20 20M48 68h28" fill="none" stroke="white" stroke-width="8"/></svg>')
(r/'profile/main_pages.json').write_text(json.dumps({'src':(['pages/HapInstallerPage'] if args.hap_installer else ['pages/LinuxWindow'] if args.window_service else ['pages/UiHost'] if args.ui_host else ['pages/UiProbe'] if args.ui_probe else ['pages/EngineWindow','pages/AncoWindow','pages/AncoTaskPage'] if args.engine_host else ['pages/OhosWindow'] if args.ohos_window else ['pages/LinuxEntry'] if args.entry else ['pages/Linux','pages/ProbeChild'] if args.window_probe else ['pages/Linux'])}))
ability='HapInstallerAbility' if args.hap_installer else ('LinuxWindowAbility' if args.window_service else ('UiHostAbility' if args.ui_host else ('UiProbeAbility' if args.ui_probe else ('EngineHostAbility' if args.engine_host else ('OhosWindowAbility' if args.ohos_window else ('EntryAbility' if args.entry else 'LinuxAbility'))))))
manifest={'app':{'bundleName':'com.l410.linux','vendor':'L410','versionCode':1,'versionName':'0.1.0','minAPIVersion':20,'targetAPIVersion':20,'apiReleaseType':'Release','debug':True,'icon':'$media:linux','label':'$string:app_name'},'module':{'name':'entry','type':'entry','deviceTypes':['2in1','tablet'],'deliveryWithInstall':True,'installationFree':False,'pages':'$profile:main_pages','compileMode':'esmodule','mainElement':ability,'requestPermissions':[{'name':'ohos.permission.INTERNET'}],'abilities':[{'name':ability,'srcEntry':'./ets/entryability/'+ability+'.js','exported':True,'icon':'$media:linux','label':'$string:app_name','launchType':'singleton','skills':[{'entities':['entity.system.home'],'actions':['ohos.want.action.home']}]}]}}
manifest['app']['bundleType']='app'
manifest['app']['bundleName']=bundle
if args.entry:manifest['module']['requestPermissions']=[]
if args.entry:
    # Huawei's StatusBarExtensionKit is available only through the ArkTS lazy
    # module factory used by Qt OHOS.  The small AbilityStage record is built by
    # DevEco while the proven launcher lifecycle remains ordinary JS.
    manifest['module']['srcEntry']='./ets/entrystage/EntryStage.ets'
    manifest['module']['extensionAbilities']=[{
        'name':'TrayServiceAbility',
        'srcEntry':'./ets/entryability/TrayServiceAbility.js',
        'type':'service','exported':False,'visible':False,
        'description':'Linux StatusNotifierItem background host'}]
if args.hap_installer:
    manifest['module']['requestPermissions']=[]
    manifest['module']['libIsolation']=False
if args.protocol:
    # The display ability requires a prepared session; launch it through the console.
    manifest['module']['abilities'][0].pop('skills',None)
if args.native_display or args.window_service or args.ohos_window or args.engine_host:
    # Keep native libraries with this module so updates use the module's
    # replacement path, rather than the bundle-wide libs_tmp rename path.
    manifest['module']['libIsolation']=True
if args.engine_host:
    # The broker module must also be importable with requireNapi from a
    # windowless/service-style context.  Per-module isolation made it visible
    # only to XComponent and forced a manager page to exist just to load it.
    # Bundle-wide native registration is the proven path used by ui-host.
    manifest['module']['libIsolation']=False
    # The broker shell is an implementation detail.  Do not publish a launcher
    # icon or a visible ability entry for it; Linux applications have their own
    # generated menu entries and real windows.
    manifest['module']['abilities'][0].pop('skills',None)
    manifest['module']['abilities'][0]['visible']=False
    # 容器里的应用和鸿蒙剪贴板互通需要读剪贴板权限（系统应用可预先允许）。
    manifest['module']['requestPermissions'].append({'name':'ohos.permission.READ_PASTEBOARD','reason':'$string:hardware_reason','usedScene':{'abilities':[ability],'when':'inuse'}})
    # 宿主侧窗口级输入：键盘事件由宿主直接取（不依赖控件焦点），再转给容器里的应用。
    manifest['module']['requestPermissions'].append({'name':'ohos.permission.INPUT_MONITORING','reason':'$string:hardware_reason','usedScene':{'abilities':[ability],'when':'inuse'}})
    # 真实应用窗口：每个应用窗口一个独立的能力实例（multiton），它的主窗口就是应用窗口，
    # 这样才有系统自带的最小化/关闭和独立任务条目。
    manifest['module']['abilities'].append({'name':'EngineWindowAbility',
        'srcEntry':'./ets/entryability/EngineWindowAbility.js','exported':False,
        'icon':'$media:linux','label':'$string:app_name','launchType':'multiton'})
    manifest['module']['abilities'].append({'name':'AncoWindowAbility',
        'srcEntry':'./ets/entryability/AncoWindowAbility.js','exported':False,
        'icon':'$media:linux','label':'$string:app_name','launchType':'multiton',
        'visible':False})
    manifest['module']['abilities'].append({'name':'AncoTaskAbility',
        'srcEntry':'./ets/entryability/AncoTaskAbility.js','exported':False,
        'icon':'$media:linux','label':'$string:app_name','launchType':'standard',
        'visible':False})
    manifest['module']['abilities'].append({'name':'AncoDialogAbility',
        'srcEntry':'./ets/entryability/AncoDialogAbility.js','exported':False,
        'icon':'$media:linux','label':'$string:app_name','launchType':'multiton',
        'visible':False})
    manifest['module']['requestPermissions'].append({'name':'ohos.permission.START_ABILITIES_FROM_BACKGROUND'})
    manifest['module']['requestPermissions'].append({'name':'ohos.permission.START_INVISIBLE_ABILITY'})
    # Match the stock ShellAssistant AnCo shape: the broker listener lives in
    # an invisible background service and creates application windows only in
    # response to real guest lifecycle events.  The UI abilities remain during
    # migration so the last known-good path can still be rolled back.
    manifest['module']['extensionAbilities']=[{
        'name':'AncoServiceAbility',
        'srcEntry':'./ets/entryability/AncoServiceAbility.js',
        'type':'service','exported':False,'visible':False,
        'description':'Linux AnCo background window coordinator'}]
if args.ui_host:
    # Same reason as the probe: a node type XComponent cannot select a per module
    # library path, so the app's library has to live in the bundle wide lib dir.
    manifest['module']['libIsolation']=False
    # Starting the control host again must bring the existing window forward
    # instead of creating a second one: a background page stops its timer, and
    # a second window would leave the first one unable to apply control commands.
    manifest['module']['abilities'][0]['launchType']='singleton'
if args.ui_probe:
    # A node type XComponent has no soPath (the framework ignores it for node),
    # so the native module is only found through the bundle wide lib path that
    # the app registers as "default". libIsolation keeps the library under
    # <module>/libs/<abi> and registers only a per module path, which a node
    # component cannot select: the load then falls back to /system/lib64/module.
    manifest['module']['libIsolation']=False
if launcher or args.entry:
    icon=Path(descriptor['iconFile']) if descriptor else b/'assets/mousepad.png'
    if icon.suffix.lower() not in ('.png','.svg'):raise ValueError('launcher icon must be PNG or SVG')
    for extension in ('.png','.svg'):
        previous=r/'media'/('app_icon'+extension)
        if previous.exists():previous.unlink()
    shutil.copyfile(icon,r/'media'/('app_icon'+icon.suffix.lower()))
    raw=app/'resources/rawfile';raw.mkdir(parents=True,exist_ok=True)
    for previous in raw.glob('tray_icon.*'):previous.unlink()
    tray_name='tray_icon'+icon.suffix.lower()
    shutil.copyfile(icon,raw/tray_name)
    strings.append({'name':'tray_icon_file','value':tray_name})
    (r/'element/string.json').write_text(json.dumps({'string':strings},ensure_ascii=False),encoding='utf8')
    manifest['app']['icon']='$media:app_icon'
    manifest['module']['abilities'][0]['icon']='$media:app_icon'
if args.console or args.hap_installer:
    icon=b/'assets'/('hap-installer.svg' if args.hap_installer else 'linux-manager.svg')
    shutil.copyfile(icon,r/'media/app_icon.svg')
    manifest['app']['icon']='$media:app_icon'
    manifest['module']['abilities'][0]['icon']='$media:app_icon'
if args.console:
    # Declare the ordinary 2-in-1 window modes used by native desktop apps.
    # Without this, SceneBoard treats the manager as a landscape compatibility
    # app and scales it into a reduced viewport, leaving a wallpaper strip at
    # the top when maximized.
    manifest['module']['abilities'][0]['supportWindowMode']=['floating','fullscreen','split']
for permission in (() if (args.console or args.hap_installer or args.entry or args.ohos_window or args.engine_host) else ('MICROPHONE','CAMERA','READ_PASTEBOARD','ACTIVITY_MOTION')):
    manifest['module']['requestPermissions'].append({'name':'ohos.permission.'+permission,'reason':'$string:hardware_reason','usedScene':{'abilities':[ability],'when':'inuse'}})
manifest['module']['distro']={'moduleName':'entry','moduleType':'entry','deliveryWithInstall':True,'installationFree':False}
(app/'module.json').write_text(json.dumps(manifest,ensure_ascii=False),encoding='utf8')
entries=[]
secret=b/'config/token'
secret.parent.mkdir(exist_ok=True)
if not secret.exists():secret.write_text(secrets.token_urlsafe(32))
(app/'generated').mkdir(exist_ok=True)
if args.hap_installer:
    records=[('HapInstallerAbility','entry/ets/entryability/HapInstallerAbility'),('HapInstallerPage','entry/ets/pages/HapInstallerPage')]
elif args.entry:
    records=[('EntryAbility','entry/ets/entryability/EntryAbility'),
             ('TrayServiceAbility','entry/ets/entryability/TrayServiceAbility'),
             ('LinuxEntry','entry/ets/pages/LinuxEntry')]
elif args.window_service:
    records=[('LinuxWindowAbility','entry/ets/entryability/LinuxWindowAbility'),('LinuxWindow','entry/ets/pages/LinuxWindow')]
elif args.ui_probe:
    records=[('UiProbeAbility','entry/ets/entryability/UiProbeAbility'),('UiProbe','entry/ets/pages/UiProbe')]
elif args.ui_host:
    records=[('UiHostAbility','entry/ets/entryability/UiHostAbility'),('UiHost','entry/ets/pages/UiHost')]
elif args.ohos_window:
    records=[('OhosWindowAbility','entry/ets/entryability/OhosWindowAbility'),('OhosWindow','entry/ets/pages/OhosWindow')]
elif args.engine_host:
    records=[('EngineHostAbility','entry/ets/entryability/EngineHostAbility'),('EngineWindow','entry/ets/pages/EngineWindow'),('EngineWindowAbility','entry/ets/entryability/EngineWindowAbility'),('AncoServiceAbility','entry/ets/entryability/AncoServiceAbility'),('AncoWindowAbility','entry/ets/entryability/AncoWindowAbility'),('AncoWindow','entry/ets/pages/AncoWindow'),('AncoTaskManager','entry/ets/entryability/AncoTaskManager'),('AncoSystemManager','entry/ets/entryability/AncoSystemManager'),('AncoTaskAbility','entry/ets/entryability/AncoTaskAbility'),('AncoDialogAbility','entry/ets/entryability/AncoDialogAbility'),('AncoTaskPage','entry/ets/pages/AncoTaskPage')]
else:
    records=[('LinuxAbility','entry/ets/entryability/LinuxAbility'),('Linux','entry/ets/pages/Linux')]+([('ProbeChild','entry/ets/pages/ProbeChild')] if args.window_probe else [])
for name,record in records:
    if not args.unbundled_records:record=bundle+'/'+record
    generated=app/'generated'/(name+'.js')
    if args.hap_installer or args.entry or args.window_service or args.ui_probe or args.ui_host or args.ohos_window or args.engine_host:
        extension='.js'
        generated=app/'generated'/(name+extension)
        source=(b/'src'/(name+extension)).read_text(encoding='utf8').replace('__L410_TOKEN__',secret.read_text().strip()).replace('__L410_BUNDLE__',bundle)
        generated.write_text(source,encoding='utf8')
        entries.append(f'{generated.as_posix()};{record};esm;{record}{extension};entry;false;js')
        continue
    source=(b/'src'/((('LinuxAbility' if name=='LinuxAbility' else 'NativeDisplay') if args.native_display else name)+'.js')).read_text(encoding='utf8').replace('__L410_TOKEN__',secret.read_text().strip()).replace('__L410_BUNDLE__',bundle).replace('__L410_AUTO_START__','true' if launcher else 'false')
    if args.native_display and name=='Linux':source='\n'.join((b/'src'/file).read_text(encoding='utf8') for file in ('native_input.js','native_audio.js'))+'\n'+source
    if args.native_display and name=='Linux':
        native_app=json.dumps('desktop:'+descriptor['id'] if descriptor else 'mousepad') if launcher else "globalThis.l410Test==='native-mousepad'?'mousepad':globalThis.l410Test==='native-calibration'?'desktop:l410-calibration.desktop':'glxgears'"
        source=source.replace('__L410_NATIVE_APP__',native_app)
    if launcher and not args.native_display and name=='Linux':
        start=source.index('    // MANAGER_CONTROLS_BEGIN');end=source.index('    // MANAGER_CONTROLS_END')
        source=source[:start]+source[end:]
        if descriptor:source=source.replace("globalThis.l410Test==='gpu'?'glxgears':'mousepad'",json.dumps('desktop:'+descriptor['id']))
    if args.window_probe:source=(b/'src'/({'LinuxAbility':'WindowProbeAbility','Linux':'WindowProbe'}.get(name,name)+'.js')).read_text(encoding='utf8')
    if args.rootless:source=(b/'src'/('RootlessAbility.js' if name=='LinuxAbility' else 'RootlessPage.js')).read_text(encoding='utf8').replace('__L410_TOKEN__',secret.read_text().strip())
    if args.rootless and name!='LinuxAbility':source=(b/'src/native_input.js').read_text(encoding='utf8')+'\n'+source
    if args.protocol:source=(b/'src'/('ProtocolAbility.js' if name=='LinuxAbility' else 'ProtocolPage.js')).read_text(encoding='utf8').replace('__L410_TOKEN__',secret.read_text().strip())
    if args.console:source=(b/'src'/('ConsoleAbility.js' if name=='LinuxAbility' else 'ConsolePage.js')).read_text(encoding='utf8').replace('__L410_TOKEN__',secret.read_text().strip())
    generated.write_text(source,encoding='utf8')
    entries.append(f'{generated.as_posix()};{record};esm;{record}.js;entry;false;js')
(app/'files.txt').write_text('\n'.join(entries)+'\n')
compiler=root/'audit-alignment-v306/tile-sdk/ets-windows/ets/build-tools/ets-loader/bin/ark/build-win/bin/es2abc.exe'
if args.entry:
    subprocess.run([sys.executable,str(b/'scripts/build-entry-abc-deveco.py'),
        '--bundle',bundle,'--label',label,'--entry-js',str(app/'generated/EntryAbility.js'),
        '--tray-service-js',str(app/'generated/TrayServiceAbility.js'),
        '--output',str(out/'ets/modules.abc'),'--project',str(app/'deveco-entry-project')],check=True)
else:
    subprocess.run([str(compiler),'--module','--merge-abc','--target-api-version=20','--output='+str(out/'ets/modules.abc'),'@'+str(app/'files.txt')],check=True)
restool=root/'tools/oh6-sdk-extract/toolchains-windows/toolchains/restool.exe'
subprocess.run([str(restool),'-i',str(app),'-p',bundle,'-o',str(out),'-r',str(app/'ResourceTable.txt'),'-j',str(app/'module.json'),'-f'],check=True)
config=json.loads((out/'module.json').read_text(encoding='utf8'))
config['app'].update(compileSdkType='OpenHarmony',compileSdkVersion='6.0.0.47')
config['module'].pop('distro',None)
config['module'].update(packageName='entry',virtualMachine='ark13.0.1.0')
(out/'module.json').write_text(json.dumps(config,ensure_ascii=False),encoding='utf8')
if args.window_service:
    native=out/'libs/arm64-v8a';native.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(b/'out/libl410window.so',native/'libl410window.so')
if args.ui_probe:
    native=out/'libs/arm64-v8a';native.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(b/'out/libl410uiprobe.so',native/'libl410uiprobe.so')
if args.ui_host:
    native=out/'libs/arm64-v8a';native.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(b/'out/libl410uihost.so',native/'libl410uihost.so')
if args.entry:
    native=out/'libs/arm64-v8a';native.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(b/'out/libl410uihost.so',native/'libl410uihost.so')
if args.console:
    # The console page talks to the management service through the same
    # native module the control host uses (abstract socket client).
    native=out/'libs/arm64-v8a';native.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(b/'out/libl410uihost.so',native/'libl410uihost.so')
if args.hap_installer:
    native=out/'libs/arm64-v8a';native.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(b/'out/libl410uihost.so',native/'libl410uihost.so')
if args.ohos_window:
    # The XComponent loads the buffer serving module directly.
    native=out/'libs/arm64-v8a';native.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(b/'out/libl410displaymulti.so',native/'libl410displaymulti.so')
if args.engine_host:
    # Two native pieces: the host broker channel (libl410engine) and the per
    # window buffer serving module the XComponent loads (libl410displaymulti).
    native=out/'libs/arm64-v8a';native.mkdir(parents=True,exist_ok=True)
    shutil.copyfile(b/'compat-engine/out/libl410engine.so',native/'libl410engine.so')
    shutil.copyfile(b/'build/host/libl410ancotaskinput.so',
                    native/'libl410ancotaskinput.so')
    # Exact L410 system module used by ShellAssistant to register AnCo task
    # windows with MMI. Keep it inside this candidate HAP; do not replace the
    # system copy or borrow the original RDP/container runtime.
    stock_input_module=b/'build/host/libshell_fusion_manager-device.so'
    if not stock_input_module.is_file():
        raise FileNotFoundError(stock_input_module)
    shutil.copyfile(stock_input_module,native/'libshell_fusion_manager.so')
    typed_lib=args.typed_lib if args.typed_lib else b/'build/host/libl410_anco_surface_typed.z.so'
    shutil.copyfile(typed_lib,native/'libl410_anco_surface_typed.z.so')
    shutil.copyfile(b/'out/libl410displaymulti.so',native/'libl410displaymulti.so')
if args.native_display:
    native=out/'libs/arm64-v8a';native.mkdir(parents=True,exist_ok=True);shutil.copyfile(b/'out/libl410display.so',native/'libl410display.so')
    if args.window_probe:shutil.copyfile(b/'out/libl410displaymulti.so',native/'libl410displaymulti.so')
with zipfile.ZipFile(app/'Linux.unsigned.hap','w',compression=zipfile.ZIP_STORED) as z:
    for p in sorted(out.rglob('*')):
        if p.is_file() and p.name not in ('config.json','ResourceTable.txt'):z.write(p,p.relative_to(out).as_posix())
# Reuse the existing locally managed application signing chain, never device identity credentials.
d=root/'audit-alignment-v306/performance-tile/private-trust'
profile=json.loads((d/'profile.json').read_text())
profile['bundle-info']['bundle-name']=bundle
# AnCo（卓易通）白名单是按 bundle + AppIdentifier 登记的；系统只看这个字段。
# 先自报一个 19 位十进制标识，装上去验证系统是否接受（见 logs/）。
profile['bundle-info']['app-identifier']='5765880207859999999'
# （对照实验结论：按普通应用装会因系统权限被拒；保持系统应用身份。）
profile['acls']['allowed-acls']=[]
profile.pop('app-privilege-capabilities',None)
if args.engine_host or args.entry:
    # A ServiceExtensionAbility is a privileged extension on this system.
    # This capability is scoped to the Linux engine profile only and is the
    # same capability already used by the local performance-tile signing flow.
    profile['app-privilege-capabilities']=['AllowAppUsePrivilegeExtension']
    # The invisible AnCo coordinator must be able to create its per-window
    # multiton UI abilities while no foreground host page exists.  Both are
    # system-grant permissions; list only these engine-specific permissions in
    # this profile rather than widening the shared signing profile.
    if args.engine_host:
        profile['acls']['allowed-acls']=[
            'ohos.permission.START_ABILITIES_FROM_BACKGROUND',
            'ohos.permission.START_INVISIBLE_ABILITY'
        ]
(app/'profile.json').write_text(json.dumps(profile),encoding='utf8')
pw=json.loads((d/'key-password.json').read_text())['password']
java=root/'tools/temurin17-jre/jdk-17.0.20.1+1-jre/bin/java.exe'
jar=root/'tools/oh6-sdk-extract/toolchains-windows/toolchains/lib/hap-sign-tool.jar'
def sign(command,args):
    result=subprocess.run([str(java),'-jar',str(jar),command,*map(str,args)],capture_output=True,text=True,encoding='utf8',errors='replace')
    with (app/'signing.log').open('a',encoding='utf8') as f:f.write((result.stdout+result.stderr).replace(pw,'[redacted]'))
    if result.returncode:raise RuntimeError('Signing failed; see redacted signing.log')
sign('sign-profile',['-mode','localSign','-keyAlias','profile','-keyPwd',pw,'-profileCertFile',d/'profile-chain.cer','-inFile',app/'profile.json','-signAlg','SHA384withECDSA','-keystoreFile',d/'profile.p12','-keystorePwd',pw,'-outFile',app/'profile.p7b'])
sign('sign-app',['-mode','localSign','-keyAlias','tile','-keyPwd',pw,'-appCertFile',d/'tile-chain.cer','-profileFile',app/'profile.p7b','-inFile',app/'Linux.unsigned.hap','-signAlg','SHA384withECDSA','-keystoreFile',d/'tile.p12','-keystorePwd',pw,'-outFile',app/'Linux.signed.hap','-compatibleVersion','20'])
print('SIGNED_HAP_BYTES', (app/'Linux.signed.hap').stat().st_size)
