"""Run only in enter --builder: resource compile and ordinary HAP signing."""
import base64,json,re,shutil,subprocess,time,zipfile
from pathlib import Path
tools=Path('/run/l410-builder');work=Path('/run/l410-build')
job=json.loads((work/'job.json').read_text());tray_only=job.get('trayOnly') is True
bundle=job['bundle'];target_bundle=job['targetBundle'];target_ability=job['targetAbility']
assert re.fullmatch(r'com\.l410\.linux\.(?:app\.)?[a-z][a-z0-9_]{0,80}',bundle)
assert re.fullmatch(r'org\.l410\.linux\.[a-z0-9_.]+',target_bundle)
assert target_ability.startswith(target_bundle+'.') and re.fullmatch(r'[A-Za-z0-9_.]+',target_ability)
template=tools/'templates'/bundle/'modules.abc'
template_data=None
if template.is_file():
    template_data=template.read_bytes()
elif tray_only:
    prefix='com.l410.linux.tray_'
    assert bundle.startswith(prefix),'隐藏托盘宿主必须使用正式 tray 身份'
    slug=bundle[len(prefix):]
    assert re.fullmatch(r'[a-z][a-z0-9_]{0,80}',slug)
    template=tools/'templates/tray-length-bank'/str(len(slug))/'modules.abc'
    assert template.is_file(),'缺少该名称长度的托盘模板'
    marker=(prefix+'x'*len(slug)).encode()
    template_data=template.read_bytes()
    occurrences=template_data.count(marker)
    assert occurrences>=6,'托盘模板不包含完整 Ark bundle 记录'
    template_data=template_data.replace(marker,bundle.encode())
    assert len(template_data)==template.stat().st_size,'托盘模板替换必须等长'
else:
    raise AssertionError('入口模板未预装，请使用工作站助手扩充模板')
folder=work/(bundle+'-'+str(time.time_ns()));folder.mkdir(mode=0o700)
resources=folder/'resources/base'
for name in ('element','media','profile'):(resources/name).mkdir(parents=True)
(folder/'resources/rawfile').mkdir(parents=True)
out=folder/'out';out.mkdir()
icon=job.get('icon');extension='.svg';data=(tools/'generic-entry.svg').read_bytes()
if icon:
    extension=icon['extension'];assert extension in ('.png','.svg','.xpm')
    data=base64.b64decode(icon['data'],validate=True);assert len(data)<=2*1024*1024
if extension=='.xpm':
    from PIL import Image
    import io
    output=io.BytesIO();Image.open(io.BytesIO(data)).convert('RGBA').save(output,format='PNG');data=output.getvalue();extension='.png'
(resources/'element/string.json').write_text(json.dumps({'string':[
    {'name':'app_name','value':job['name'][:128]},
    {'name':'desktop_id','value':job['id']},
    {'name':'target_bundle','value':target_bundle},
    {'name':'target_ability','value':target_ability},
    {'name':'tray_icon_file','value':'tray_icon'+extension},
]},ensure_ascii=False))
(resources/'profile/main_pages.json').write_text('{"src":["pages/LinuxEntry"]}')
(resources/'media'/('app_icon'+extension)).write_bytes(data)
(folder/'resources/rawfile'/('tray_icon'+extension)).write_bytes(data)
manifest=json.loads((tools/'templates/module.json').read_text());manifest['app']['bundleName']=bundle
manifest['app']['icon']='$media:app_icon'
manifest['module']['extensionAbilities']=[{
    'name':'TrayServiceAbility','srcEntry':'./ets/entryability/TrayServiceAbility.js',
    'type':'service','exported':False,'visible':False,
    'description':'Linux StatusNotifierItem background host'}]
if tray_only:
    # The SP68 bundle parser rejects an entry module whose mainElement is a
    # ServiceExtensionAbility.  Keep the precompiled EntryAbility as the
    # bundle's inert application context, but remove every launch surface so
    # it cannot appear in Launcher or create a mission/window.  Only the
    # TrayServiceAbility is started by the SNI bridge.
    ability=manifest['module']['abilities'][0]
    ability['exported']=False
    ability['visible']=False
    ability['skills']=[]
    ability['excludeFromMissions']=True
    ability['removeMissionAfterTerminate']=True
    manifest['module']['mainElement']='EntryAbility'
    # The hidden tray context has its own implementation version.  Do not
    # expose the generic launcher-template development version in Settings.
    manifest['app']['versionName']='1.0.0'
else:
    manifest['module']['abilities'][0]['icon']='$media:app_icon'
(folder/'module.json').write_text(json.dumps(manifest,ensure_ascii=False))
subprocess.run([str(tools/'restool'),'-i',str(folder),'-p',bundle,'-o',str(out),'-r',str(folder/'ResourceTable.txt'),'-j',str(folder/'module.json'),'-f'],check=True,timeout=60)
manifest=json.loads((out/'module.json').read_text())
manifest['app'].update(compileSdkType='OpenHarmony',compileSdkVersion='6.0.0.47')
manifest['module'].pop('distro',None);manifest['module'].update(packageName='entry',virtualMachine='ark13.0.1.0')
(out/'module.json').write_text(json.dumps(manifest,ensure_ascii=False))
(out/'ets').mkdir(exist_ok=True);(out/'ets/modules.abc').write_bytes(template_data)
(out/'libs/arm64-v8a').mkdir(parents=True,exist_ok=True)
shutil.copyfile(tools/'libl410uihost.so',out/'libs/arm64-v8a/libl410uihost.so')
unsigned=folder/'Linux.unsigned.hap'
with zipfile.ZipFile(unsigned,'w',compression=zipfile.ZIP_STORED) as archive:
    for path in sorted(out.rglob('*')):
        if path.is_file() and path.name not in ('config.json','ResourceTable.txt'):archive.write(path,path.relative_to(out).as_posix())
private=tools/'private'
profile=json.loads((private/'profile.json').read_text());profile['bundle-info']['bundle-name']=bundle
# Match the already installed per-application tray hosts: AnCo identity uses
# this app identifier, while ServiceExtensionAbility installation on SP68 is
# rejected unless the profile explicitly carries the existing privilege-
# extension capability.  Keep ACLs empty; no additional system permission is
# granted to the launcher/tray host.
profile['bundle-info']['app-identifier']='5765880207859999999'
profile['acls']['allowed-acls']=[]
capabilities=['AllowAppUsePrivilegeExtension']
if tray_only:
    # This is the original BMS/Launcher contract for an installed application
    # context that must not occupy an application-grid slot.  Ability
    # visible/skills flags alone do not set ApplicationInfo.hideDesktopIcon on
    # SP68; the signed profile capability does.
    capabilities.append('AllowAppDesktopIconHide')
profile['app-privilege-capabilities']=capabilities
(folder/'profile.json').write_text(json.dumps(profile))
password=json.loads((private/'key-password.json').read_text())['password']
def sign(command,arguments):
    result=subprocess.run(['/usr/bin/java','-Xmx256m','-jar',str(tools/'hap-sign-tool.jar'),command,*map(str,arguments)],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True,timeout=90)
    with (folder/'signing.log').open('a') as stream:stream.write(result.stdout.replace(password,'[redacted]'))
    if result.returncode:raise RuntimeError('签名失败，查看该次构建的 signing.log')
sign('sign-profile',['-mode','localSign','-keyAlias','profile','-keyPwd',password,'-profileCertFile',private/'profile-chain.cer','-inFile',folder/'profile.json','-signAlg','SHA384withECDSA','-keystoreFile',private/'profile.p12','-keystorePwd',password,'-outFile',folder/'profile.p7b'])
sign('sign-app',['-mode','localSign','-keyAlias','tile','-keyPwd',password,'-appCertFile',private/'tile-chain.cer','-profileFile',folder/'profile.p7b','-inFile',unsigned,'-signAlg','SHA384withECDSA','-keystoreFile',private/'tile.p12','-keystorePwd',password,'-outFile',folder/'Linux.signed.hap','-compatibleVersion','20'])
(work/'result.json').write_text(json.dumps({'hap':(folder/'Linux.signed.hap').relative_to(work).as_posix(),'bundle':bundle,'bytes':(folder/'Linux.signed.hap').stat().st_size}))
print('DEVICE_ENTRY_BUILD_PASS',bundle,flush=True)
