"""Build original RS query ABI bridge and L410-only native check. No VM execution."""
from pathlib import Path
import json
import subprocess
import sys

root=Path(__file__).resolve().parents[2]
out=Path(sys.argv[1]).resolve()
out.mkdir(parents=True,exist_ok=False)
baseline=root/'linux-compat/build-support/native-owner'
commands=json.loads((baseline/'commands.json').read_text())
def wsl(path):
    p=str(path.resolve()).replace('\\','/')
    return '/mnt/'+p[0].lower()+p[2:]
host=root/'had-w32-6.0-analysis/extracted/system/system/lib64'
support=root/'analysis_sp68_sceneboard_lifecycle/original-host-service-checker-inputs-20261002-r1'
crt=root/'toolchains/aarch64-linux-musl-cross/aarch64-linux-musl/lib'
jobs=[]
for name,source in [('bridge',root/'linux-compat/src/anco_host_rog_query_bridge.cpp'),
                    ('check',root/'linux-compat/tools/anco_host_rog_query_check.cpp')]:
    command=commands['compile'][:]
    command[command.index(wsl(baseline/'binding.cpp'))]=wsl(source)
    command[command.index(wsl(baseline/'binding.bc'))]=wsl(out/(name+'.bc'))
    command=[x for x in command if not x.startswith('-fsanitize=') and x!='-fsanitize-cfi-cross-dso']
    jobs.append((name+'-compile',command))
    (out/source.name).write_bytes(source.read_bytes())
common=['wsl','-d','Ubuntu','--','clang++-18','--target=aarch64-linux-ohos',
        '-nostdlib','-fuse-ld=lld','-flto','-Wl,--build-id=none','-Wl,--no-undefined',
        '-Wl,--allow-shlib-undefined']
bridge=out/'libanco_host_rog_query.so'
jobs.append(('bridge-link',common+['-shared','-Wl,-soname,libanco_host_rog_query.so',wsl(out/'bridge.bc'),
    wsl(host/'librender_service_base.z.so'),wsl(host/'chipset-sdk-sp/libipc_single.z.so'),
    wsl(host/'platformsdk/libutils.z.so'),wsl(support/'libc++.so'),wsl(support/'libc.so'),
    '-o',wsl(bridge)]))
jobs.append(('check-link',common+['-pie','-Wl,--dynamic-linker,/lib/ld-musl-aarch64.so.1',
    wsl(crt/'Scrt1.o'),wsl(crt/'crti.o'),wsl(out/'check.bc'),wsl(bridge),
    wsl(support/'libc++.so'),wsl(support/'libc.so'),wsl(crt/'crtn.o'),
    '-o',wsl(out/'anco-host-rog-query-check')]))
(out/'commands.json').write_text(json.dumps(dict(jobs),indent=2))
(out/Path(__file__).name).write_bytes(Path(__file__).read_bytes())
for name,command in jobs:
    with (out/(name+'.log')).open('wb') as log:
        result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT)
    if result.returncode:
        print((out/(name+'.log')).read_text(errors='replace'))
        raise SystemExit(result.returncode)
print(bridge)
