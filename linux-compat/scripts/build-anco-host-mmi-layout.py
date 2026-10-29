"""Cross-build private original MMI version adapter; execution is L410 only."""
from pathlib import Path
import json
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=False)
baseline = root / 'linux-compat/build-support/native-owner'
commands = json.loads((baseline / 'commands.json').read_text())
def wsl(path):
    value = str(path.resolve()).replace('\\', '/')
    return '/mnt/' + value[0].lower() + value[2:]
compile_cmd = commands['compile'][:]
compile_cmd[compile_cmd.index(wsl(baseline / 'binding.cpp'))] = wsl(root / 'linux-compat/src/anco_host_mmi_layout_bridge.cpp')
compile_cmd[compile_cmd.index(wsl(baseline / 'binding.bc'))] = wsl(out / 'bridge.bc')
# No fabricated virtual ABI types/callbacks or cross-DSO indirect dispatch.
compile_cmd = [x for x in compile_cmd if not x.startswith('-fsanitize=')
               and x != '-fsanitize-cfi-cross-dso']
host = root / 'had-w32-6.0-analysis/extracted/system/system/lib64'
support = root / 'analysis_sp68_sceneboard_lifecycle/original-host-service-checker-inputs-20261002-r1'
link_cmd = ['wsl', '-d', 'Ubuntu', '--', 'clang++-18', '--target=aarch64-linux-ohos',
    '-shared', '-nostdlib', '-fuse-ld=lld', '-flto', '-Wl,--build-id=none',
    '-Wl,-soname,libanco_host_mmi_layout.so', '-Wl,--no-undefined',
    '-Wl,--allow-shlib-undefined', wsl(out / 'bridge.bc'),
    wsl(host / 'platformsdk/libmmi-client.z.so'), wsl(support / 'libc++.so'),
    wsl(support / 'libc.so'), '-o', wsl(out / 'libanco_host_mmi_layout.so')]
(out / 'commands.json').write_text(json.dumps({'compile': compile_cmd, 'link': link_cmd}, indent=2))
(out / 'anco_host_mmi_layout_bridge.cpp').write_bytes((root / 'linux-compat/src/anco_host_mmi_layout_bridge.cpp').read_bytes())
(out / Path(__file__).name).write_bytes(Path(__file__).read_bytes())
for name, cmd in [('compile', compile_cmd), ('link', link_cmd)]:
    with (out / (name+'.log')).open('wb') as log:
        result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        print((out / (name+'.log')).read_text(errors='replace'))
        raise SystemExit(result.returncode)
print(out / 'libanco_host_mmi_layout.so')
