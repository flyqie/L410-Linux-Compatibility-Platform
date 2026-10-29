"""Cross-build the current original native UdsIpc typed callback, L410 only."""
from pathlib import Path
import json,subprocess,sys
root=Path(__file__).resolve().parents[2]
sys.stdout.reconfigure(encoding='utf-8',errors='replace')
out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=False)
def wsl(path):
    s=str(path.resolve()).replace('\\','/');return '/mnt/'+s[0].lower()+s[2:]
baseline=root/'linux-compat/build-support/native-owner'
command=json.loads((baseline/'commands.json').read_text())['compile'][:]
relay='--relay' in sys.argv[2:]
session=relay or '--session' in sys.argv[2:]
source=root/('linux-compat/src/anco_native_input_relay.cpp' if relay else
            'linux-compat/tools/probe_anco_input_session.cpp' if session else
            'linux-compat/tools/probe_anco_uds_typed.cpp')
command[command.index(wsl(baseline/'binding.cpp'))]=wsl(source)
command[command.index(wsl(baseline/'binding.bc'))]=wsl(out/'probe.bc')
command+=['-Xclang','-fdump-record-layouts','-Xclang','-fdump-vtable-layouts']
host=root/'had-w32-6.0-analysis/extracted/system/system/lib64'
support=root/'analysis_sp68_sceneboard_lifecycle/original-host-service-checker-inputs-20261002-r1'
crt=root/'toolchains/aarch64-linux-musl-cross/aarch64-linux-musl/lib'
link=['wsl','-d','Ubuntu','--','clang++-18','--target=aarch64-linux-ohos','-pie','-nostdlib','-fuse-ld=lld','-flto','-fsanitize=cfi-vcall,cfi-icall','-fsanitize-cfi-cross-dso','-fvisibility=hidden','-fno-sanitize-link-runtime','-Wl,--build-id=none','-Wl,--no-undefined','-Wl,--allow-shlib-undefined','-Wl,--dynamic-linker,/system/lib/ld-musl-aarch64.so.1',wsl(crt/'Scrt1.o'),wsl(crt/'crti.o'),wsl(out/'probe.bc'),wsl(host/'chipset-sdk/libuds_channel.z.so'),wsl(host/'libuds_ipc_client.z.so'),wsl(host/'libuds_ipc_core.z.so'),wsl(support/'libc++.so'),wsl(support/'libc.so'),wsl(crt/'crtn.o'),'-o',wsl(out/'probe-anco-uds-typed')]
sources=[source,root/'linux-compat/src/anco_uds_native_abi.h',Path(__file__)]
if relay:sources += [root/'linux-compat/src/anco_native_input_relay.h']
commands={'compile':command,'link':link}
jobs=[('compile',command)]
if session:
    module=root/'linux-compat/src/anco_native_input_session.cpp'
    module_command=command[:]
    module_command[module_command.index(wsl(source))]=wsl(module)
    module_command[module_command.index(wsl(out/'probe.bc'))]=wsl(out/'session.bc')
    link.insert(link.index(wsl(out/'probe.bc'))+1,wsl(out/'session.bc'))
    sources += [module,root/'linux-compat/src/anco_native_input_session.hpp',root/'linux-compat/src/anco_stock_input_events.hpp']
    commands['compile_session']=module_command
    jobs.append(('compile_session',module_command))
    windows=root/'linux-compat/src/anco_native_input_windows.cpp'
    windows_command=module_command[:]
    windows_command[windows_command.index(wsl(module))]=wsl(windows)
    windows_command[windows_command.index(wsl(out/'session.bc'))]=wsl(out/'windows.bc')
    link.insert(link.index(wsl(out/'session.bc'))+1,wsl(out/'windows.bc'))
    commands['compile_windows']=windows_command
    jobs.append(('compile_windows',windows_command))
    sources += [windows,root/'linux-compat/src/anco_native_input_windows.hpp']
for p in sources:
    (out/p.name).write_bytes(p.read_bytes())
(out/'commands.json').write_text(json.dumps(commands,indent=2))
jobs.append(('link',link))
for name,cmd in jobs:
    with (out/(name+'.log')).open('wb') as log:rc=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT).returncode
    if rc:print('\n'.join((out/(name+'.log')).read_text(errors='replace').splitlines()[-70:]));raise SystemExit(rc)
print('STRICT_TYPED_CFI_BUILD_PASS_NOT_DEVICE_PROOF',out)
