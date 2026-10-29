"""Cross-build original native host start binding. Execute only on real L410."""
from pathlib import Path
import json, subprocess

root = Path(__file__).resolve().parents[2]
out = root / 'linux-compat/out/original-host-start-binding-r1-20261006'
out.mkdir(parents=True, exist_ok=True)
base = root / 'linux-compat/out/bms-native-decode-20261005'
commands = json.loads((base / 'commands.json').read_text())
def wsl(path):
    p = str(path.resolve()).replace('\\', '/')
    return '/mnt/' + p[0].lower() + p[2:]
compile_cmd = commands['compile'][:]
compile_cmd[compile_cmd.index(wsl(root / 'linux-compat/src/anco_bundle_decode.cpp'))] = wsl(root / 'linux-compat/src/anco_host_start.cpp')
compile_cmd[compile_cmd.index(wsl(base / 'binding.bc'))] = wsl(out / 'binding.bc')
compile_cmd[5:5] = ['-I', wsl(root / 'analysis_upstream_c_utils_v501/base/include')]
link_cmd = commands['link'][:]
link_cmd[link_cmd.index(wsl(base / 'binding.bc'))] = wsl(out / 'binding.bc')
link_cmd[link_cmd.index(wsl(base / 'anco-bundle-decode'))] = wsl(out / 'anco-host-start')
host = root / 'had-w32-6.0-analysis/extracted/system/system/lib64'
native = [host / 'platformsdk' / name for name in (
    'libability_manager.z.so', 'libability_start_options.z.so', 'libwant.z.so')]
index = link_cmd.index(wsl(host / 'platformsdk/libappexecfwk_base.z.so'))
link_cmd[index:index] = [wsl(path) for path in native]
(out / 'commands.json').write_text(json.dumps(dict(compile=compile_cmd, link=link_cmd), indent=2)+'\n', encoding='utf-8')
for name, cmd in [('compile', compile_cmd), ('link', link_cmd)]:
    with (out / (name + '.log')).open('wb') as log:
        result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        print((out / (name + '.log')).read_text(errors='replace'))
        raise SystemExit(result.returncode)
print(out / 'anco-host-start')
