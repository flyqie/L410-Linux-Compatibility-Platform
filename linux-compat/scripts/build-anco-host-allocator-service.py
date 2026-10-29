"""Build native original HDF allocator owner; execute only on actual L410."""
from pathlib import Path
import json,subprocess,sys
root=Path(__file__).resolve().parents[2]
out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=False)
baseline=root/'linux-compat/build-support/native-owner'
compile=json.loads((baseline/'commands.json').read_text())['compile'][:]
def wsl(path):
 s=str(path.resolve()).replace('\\','/');return '/mnt/'+s[0].lower()+s[2:]
source=root/'linux-compat/tools/anco_host_allocator_service.cpp'
compile[compile.index(wsl(baseline/'binding.cpp'))]=wsl(source)
compile[compile.index(wsl(baseline/'binding.bc'))]=wsl(out/'service.bc')
compile=[x for x in compile if not x.startswith('-fsanitize=') and x!='-fsanitize-cfi-cross-dso']
repo=root/'.tmp-openharmony-drivers-hdf-core'
headers=out/'headers';headers.mkdir()
for name in ['interfaces/inner_api/ipc/hdf_remote_service.h','interfaces/inner_api/core/hdf_object.h']:
 data=subprocess.check_output(['git','-C',str(repo),'show','HEAD:'+name]);(headers/Path(name).name).write_bytes(data)
compile+=['-I',wsl(headers),'-I',wsl(repo/'interfaces/inner_api/hdi'),'-I',wsl(repo/'interfaces/inner_api/utils'),'-I',wsl(repo/'interfaces/inner_api/osal/uhdf')]
host=root/'had-w32-6.0-analysis/extracted/system/system/lib64'
support=root/'analysis_sp68_sceneboard_lifecycle/original-host-service-checker-inputs-20261002-r1'
crt=root/'toolchains/aarch64-linux-musl-cross/aarch64-linux-musl/lib'
link=['wsl','-d','Ubuntu','--','clang++-18','--target=aarch64-linux-ohos','-pie','-nostdlib','-fuse-ld=lld','-flto','-Wl,--build-id=none','-Wl,--no-undefined','-Wl,--allow-shlib-undefined','-Wl,--dynamic-linker,/lib/ld-musl-aarch64.so.1',wsl(crt/'Scrt1.o'),wsl(crt/'crti.o'),wsl(out/'service.bc')]
link += [wsl(host/'chipset-sdk-sp'/name) for name in ['libhdi.z.so','libhdf_ipc_adapter.z.so','libhdf_utils.z.so','libipc_single.z.so']]
link += [wsl(host/'platformsdk/libutils.z.so'),wsl(support/'libc++.so'),wsl(support/'libc.so'),wsl(crt/'crtn.o'),'-o',wsl(out/'anco-host-allocator-service')]
(out/'commands.json').write_text(json.dumps({'compile':compile,'link':link},indent=2));(out/source.name).write_bytes(source.read_bytes());(out/Path(__file__).name).write_bytes(Path(__file__).read_bytes())
for name,command in [('compile',compile),('link',link)]:
 with (out/(name+'.log')).open('wb') as log:result=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT)
 if result.returncode:print((out/(name+'.log')).read_text(errors='replace'));raise SystemExit(result.returncode)
print(out/'anco-host-allocator-service')
