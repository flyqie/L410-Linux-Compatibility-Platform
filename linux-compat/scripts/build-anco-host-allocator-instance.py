"""Build private original named-instance selection. No VM execution."""
from pathlib import Path
import json,subprocess,sys
root=Path(__file__).resolve().parents[2]
out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=False)
baseline=root/'linux-compat/build-support/native-owner'
def wsl(path):
    s=str(path.resolve()).replace('\\','/');return '/mnt/'+s[0].lower()+s[2:]
source=root/'linux-compat/src/anco_host_allocator_instance.cpp'
compile=json.loads((baseline/'commands.json').read_text())['compile'][:]
compile[compile.index(wsl(baseline/'binding.cpp'))]=wsl(source)
compile[compile.index(wsl(baseline/'binding.bc'))]=wsl(out/'instance.bc')
compile=[x for x in compile if not x.startswith('-fsanitize=') and x!='-fsanitize-cfi-cross-dso']
compile+=['-I',wsl(root/'.tmp-openharmony-drivers-hdf-core/interfaces/inner_api/hdi')]
host=root/'had-w32-6.0-analysis/extracted/system/system/lib64'
donor=root/'had-w32-analysis/extracted/system/p111-system/system/lib64/chipset-sdk-sp'
support=root/'analysis_sp68_sceneboard_lifecycle/original-host-service-checker-inputs-20261002-r1'
link=['wsl','-d','Ubuntu','--','clang++-18','--target=aarch64-linux-ohos','-shared','-nostdlib','-fuse-ld=lld','-flto','-Wl,--build-id=none','-Wl,--no-undefined','-Wl,--allow-shlib-undefined','-Wl,-soname,libanco_host_allocator_instance.so',wsl(out/'instance.bc')]
link += [wsl(donor/name) for name in ['libdisplay_buffer_proxy_1.0.z.so','libdisplay_buffer_proxy_1.4.z.so']]
link += [wsl(host/'platformsdk/libutils.z.so'),wsl(support/'libc++.so'),wsl(support/'libc.so'),'-o',wsl(out/'libanco_host_allocator_instance.so')]
(out/'commands.json').write_text(json.dumps({'compile':compile,'link':link},indent=2));(out/source.name).write_bytes(source.read_bytes());(out/Path(__file__).name).write_bytes(Path(__file__).read_bytes())
for name,command in [('compile',compile),('link',link)]:
    with (out/(name+'.log')).open('wb') as log:result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT)
    if result.returncode:print((out/(name+'.log')).read_text(errors='replace'));raise SystemExit(result.returncode)
print(out/'libanco_host_allocator_instance.so')
