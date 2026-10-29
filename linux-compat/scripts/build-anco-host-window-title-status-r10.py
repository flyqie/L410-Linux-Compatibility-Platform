"""Build original title/hot-area backport; no activation, execution is L410 only."""
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

source = root / 'linux-compat/src/anco_host_window_title_bridge.cpp'
compile_cmd = commands['compile'][:]
compile_cmd[compile_cmd.index(wsl(baseline / 'binding.cpp'))] = wsl(source)
compile_cmd[compile_cmd.index(wsl(baseline / 'binding.bc'))] = wsl(out / 'bridge.bc')
# Opaque original objects only, no substitute virtual classes. The original
# listener CFI type check is explicitly retained at its original callback.
compile_cmd = [x for x in compile_cmd if not x.startswith('-fsanitize=')
               and x != '-fsanitize-cfi-cross-dso']
compile_cmd += ['-ffp-contract=off'] # donor uses separate FMUL/FADD, no FMA
geometry_source = root / 'linux-compat/src/anco_host_window_geometry.cpp'
geometry_baseline = root / 'linux-compat/build-support/window-geometry'
geometry_commands = json.loads((geometry_baseline / 'commands.json').read_text())
geometry_compile = geometry_commands['geometry_compile'][:]
geometry_compile[geometry_compile.index(wsl(geometry_source))] = wsl(geometry_source)
geometry_compile[geometry_compile.index(wsl(geometry_baseline / 'geometry.bc'))] = wsl(out / 'geometry.bc')
host = root / 'had-w32-6.0-analysis/extracted/system/system/lib64'
support = root / 'analysis_sp68_sceneboard_lifecycle/original-host-service-checker-inputs-20261002-r1'
link_cmd = ['wsl', '-d', 'Ubuntu', '--', 'clang++-18', '--target=aarch64-linux-ohos',
    '-shared', '-nostdlib', '-fuse-ld=lld', '-flto', '-Wl,--build-id=none',
    '-Wl,-soname,libanco_host_window_title.so', '-Wl,--no-undefined',
    '-Wl,--allow-shlib-undefined', '-fsanitize=cfi-vcall',
    '-fsanitize-cfi-cross-dso', '-fno-sanitize-link-runtime',
    '-fvisibility=hidden',
    wsl(out / 'bridge.bc'), wsl(out / 'geometry.bc'),
    wsl(host / 'libwm.z.so'), wsl(host / 'platformsdk/libutils.z.so'),
    wsl(host / 'platformsdk/libimage_native.z.so'),
    wsl(host / 'platformsdk/libwindow_scene_common.z.so'),
    wsl(host / 'platformsdk/libwmutil.z.so'),
    wsl(host / 'platformsdk/libace_compatible.z.so'),
    wsl(host / 'platformsdk/libace_container_scope.z.so'),
    wsl(support / 'libc++.so'), wsl(support / 'libc.so'),
    '-o', wsl(out / 'libanco_host_window_title.so')]
(out / 'commands.json').write_text(json.dumps({'compile': compile_cmd, 'geometry_compile': geometry_compile, 'link': link_cmd}, indent=2))
(out / source.name).write_bytes(source.read_bytes())
(out / geometry_source.name).write_bytes(geometry_source.read_bytes())
(out / Path(__file__).name).write_bytes(Path(__file__).read_bytes())
for name, cmd in [('compile', compile_cmd), ('geometry-compile', geometry_compile), ('link', link_cmd)]:
    with (out / (name+'.log')).open('wb') as log:
        result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        print((out / (name+'.log')).read_text(errors='replace'))
        raise SystemExit(result.returncode)
print(out / 'libanco_host_window_title.so')
