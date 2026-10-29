"""Cross-build private original-OHOS envelope support; never execute target code."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('output', type=Path)
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
deps = root / 'analysis_sp68_sceneboard_lifecycle/wms-no-parent-arm64-20260930'
sdk = root / 'linux-compat/out/wayland-arm64-20261005/sysroot'
out = a.output.resolve()
out.mkdir(parents=True, exist_ok=True)
source = (deps / 'gbinder/src/gbinder_io.c').read_text()
include = '#include <errno.h>'
needle = '        tx->size = tr->data_size;'
assert source.count(include) == source.count(needle) == 1
source = source.replace(include, include + '\n#include "anco_binder_hitrace.h"')
source = source.replace(needle, '        tx->size = anco_binder_business_size(tx->data,\n'
                        '            tr->data_size, tr->flags);')
(out / 'gbinder_io.original.c').write_bytes((deps / 'gbinder/src/gbinder_io.c').read_bytes())
(out / 'gbinder_io.c').write_text(source)
shutil.copyfile(root / 'linux-compat/src/anco_binder_hitrace.h', out / 'anco_binder_hitrace.h')
compiler = 'aarch64-linux-gnu-gcc'
headers = subprocess.check_output([compiler, '-print-file-name=include'], text=True).strip()
flags = ['-std=gnu11', '-O2', '-fPIC', '-pthread', '-Wall', '-Wextra', '-Werror',
         '--sysroot=' + str(sdk), '-nostdinc', '-isystem', headers,
         '-isystem', str(sdk / 'usr/include/aarch64-linux-gnu'),
         '-isystem', str(sdk / 'usr/include'),
         '-I' + str(sdk / 'usr/include/glib-2.0'),
         '-I' + str(sdk / 'usr/lib/aarch64-linux-gnu/glib-2.0/include'),
         '-I' + str(deps / 'gbinder/include'), '-I' + str(deps / 'gbinder/src'),
         '-I' + str(deps / 'glibutil/include')]
commands = []
for bits in [32, 64]:
    name = f'gbinder_io_{bits}'
    shutil.copyfile(deps / f'gbinder/src/{name}.c', out / f'{name}.c')
    cmd = [compiler, *flags, '-c', str(out / f'{name}.c'), '-o', str(out / f'{name}.o')]
    commands.append(cmd)
    subprocess.run(cmd, check=True)
shutil.copyfile(deps / 'gbinder/build/release/libgbinder.a', out / 'libgbinder.a')
cmd = ['aarch64-linux-gnu-ar', 'r', str(out / 'libgbinder.a'),
       str(out / 'gbinder_io_32.o'), str(out / 'gbinder_io_64.o')]
commands.append(cmd)
subprocess.run(cmd, check=True)
(out / 'commands.json').write_text(json.dumps(commands, indent=2))
print('PRIVATE_OHOS_BINDER_ENVELOPE_ARCHIVE', out / 'libgbinder.a')
