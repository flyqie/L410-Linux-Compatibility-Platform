"""Cross-build one original old/new Surface ABI forwarding symbol. No execution."""
from pathlib import Path
import subprocess
import sys
import json

root = Path(__file__).resolve().parents[2]
out = Path(sys.argv[1]).resolve()
out.mkdir(parents=True, exist_ok=True)

def wsl(path):
    p = str(path.resolve()).replace('\\', '/')
    return '/mnt/' + p[0].lower() + p[2:]

command = ['wsl', '-d', 'Ubuntu', '--', 'clang-18', '--target=aarch64-linux-ohos',
    '-fPIC', '-shared', '-nostdlib', '-fuse-ld=lld', '-Wl,--build-id=none',
    '-Wl,--no-undefined', '-Wl,--allow-shlib-undefined',
    '-Wl,-soname,libanco_host_surface_abi.so',
    wsl(root / 'linux-compat/src/anco_host_surface_rotation_bridge.S'),
    wsl(root / 'had-w32-analysis/extracted/system/p111-system/system/lib64/libdelegator.z.so'),
    '-o', wsl(out / 'libanco_host_surface_abi.so')]
(out / 'command.json').write_text(json.dumps(command, indent=2))
with (out / 'build.log').open('wb') as log:
    result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
if result.returncode:
    print((out / 'build.log').read_text(errors='replace'))
    raise SystemExit(result.returncode)
print(out / 'libanco_host_surface_abi.so')
