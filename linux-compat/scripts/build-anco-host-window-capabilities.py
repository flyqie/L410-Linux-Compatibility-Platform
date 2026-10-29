"""Cross-build native original-window capability diagnostic; real L410 only."""
from pathlib import Path
import json,subprocess,sys
root=Path(__file__).resolve().parents[2];out=Path(sys.argv[1]).resolve();out.mkdir(parents=True,exist_ok=False)
base=root/'linux-compat/out/original-host-start-binding-r1-20261006';commands=json.loads((base/'commands.json').read_text())
def wsl(path):
 p=str(path.resolve()).replace('\\','/');return '/mnt/'+p[0].lower()+p[2:]
source=root/'linux-compat/src/anco_host_window_capabilities.cpp'
compile_cmd=commands['compile'][:];compile_cmd[compile_cmd.index(wsl(root/'linux-compat/src/anco_host_start.cpp'))]=wsl(source);compile_cmd[compile_cmd.index(wsl(base/'binding.bc'))]=wsl(out/'binding.bc')
host=root/'had-w32-6.0-analysis/extracted/system/system/lib64';support=root/'analysis_sp68_sceneboard_lifecycle/original-host-service-checker-inputs-20261002-r1'
link_cmd=['wsl','-d','Ubuntu','--','clang++-18','--target=aarch64-linux-ohos','-shared','-nostdlib','-fuse-ld=lld','-flto','-Wl,--build-id=none','-Wl,-soname,libl410_anco_window_capabilities.so','-Wl,--no-undefined','-Wl,--allow-shlib-undefined',wsl(out/'binding.bc'),wsl(host/'libwm.z.so'),wsl(host/'platformsdk/libwindow_scene_common.z.so'),wsl(host/'platformsdk/libace_compatible.z.so'),wsl(host/'platformsdk/libutils.z.so'),wsl(support/'libc++.so'),wsl(support/'libc.so'),'-o',wsl(out/'libl410_anco_window_capabilities.so')]
(out/'commands.json').write_bytes((json.dumps(dict(compile=compile_cmd,link=link_cmd),indent=2)+'\n').encode());(out/source.name).write_bytes(source.read_bytes());(out/Path(__file__).name).write_bytes(Path(__file__).read_bytes())
for name,cmd in [('compile',compile_cmd),('link',link_cmd)]:
 with (out/(name+'.log')).open('wb') as log:r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
 if r.returncode:print((out/(name+'.log')).read_text(errors='replace'));raise SystemExit(r.returncode)
print(out/'libl410_anco_window_capabilities.so')
