"""Cross-build the original host BundleInfo parser binding; never execute here."""
from pathlib import Path
import json
import subprocess

root = Path(__file__).resolve().parents[2]
out = root / 'linux-compat/out/bms-native-decode-20261005'
out.mkdir(parents=True, exist_ok=True)
baseline = root / 'linux-compat/build-support/native-owner'
commands = json.loads((baseline / 'commands.json').read_text())

def wsl(path):
    p = str(path.resolve()).replace('\\', '/')
    return '/mnt/' + p[0].lower() + p[2:]

compile_cmd = commands['compile'][:]
compile_cmd[compile_cmd.index(wsl(baseline / 'binding.cpp'))] = wsl(root / 'linux-compat/src/anco_bundle_decode.cpp')
compile_cmd[compile_cmd.index(wsl(baseline / 'binding.bc'))] = wsl(out / 'binding.bc')
compile_cmd[5:5] = ['-I', wsl(root / 'linux-compat/third_party/nlohmann-3.11.3')]
# No callback/virtual ABI classes defined here. Direct exported native calls.
compile_cmd = [x for x in compile_cmd if not x.startswith('-fsanitize=') and x != '-fsanitize-cfi-cross-dso']
host = root / 'had-w32-6.0-analysis/extracted/system/system/lib64'
crt = root / 'toolchains/aarch64-linux-musl-cross/aarch64-linux-musl/lib'
libc = root / 'analysis_sp68_sceneboard_lifecycle/original-host-service-checker-inputs-20261002-r1'
link_cmd = ['wsl', '-d', 'Ubuntu', '--', 'clang++-18', '--target=aarch64-linux-ohos',
    '-pie', '-nostdlib', '-fuse-ld=lld', '-flto', '-Wl,--build-id=none',
    '-Wl,--dynamic-linker,/lib/ld-musl-aarch64.so.1', '-Wl,--no-undefined',
    '-Wl,--allow-shlib-undefined', wsl(crt / 'Scrt1.o'), wsl(crt / 'crti.o'),
    wsl(out / 'binding.bc'), wsl(host / 'platformsdk/libappexecfwk_base.z.so'),
    wsl(host / 'chipset-sdk-sp/libipc_single.z.so'),
    wsl(host / 'chipset-sdk-sp/libutils.z.so'), wsl(libc / 'libc++.so'),
    wsl(libc / 'libc.so'), wsl(crt / 'crtn.o'), '-o', wsl(out / 'anco-bundle-decode')]
(out / 'commands.json').write_text(json.dumps({'compile': compile_cmd, 'link': link_cmd}, indent=2))
for name, cmd in [('compile', compile_cmd), ('link', link_cmd)]:
    with (out / (name + '.log')).open('wb') as log:
        result = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        print((out / (name + '.log')).read_text(errors='replace'))
        raise SystemExit(result.returncode)
print(out / 'anco-bundle-decode')
