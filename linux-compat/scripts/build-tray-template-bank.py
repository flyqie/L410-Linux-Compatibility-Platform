"""Precompile length-indexed Ark templates for formal hidden tray bundles.

The device replaces only an equal-length placeholder bundle string.  Installed
identities remain ``com.l410.linux.tray_<desktop slug>``; this bank is an
internal build asset and never allocates numeric application identities.
"""
from pathlib import Path
import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import subprocess
import sys

base = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--minimum', type=int, default=1)
parser.add_argument('--maximum', type=int, default=76)
parser.add_argument('--workers', type=int, default=4)
args = parser.parse_args()
if not 1 <= args.minimum <= args.maximum <= 76:
    parser.error('length range must be within 1..76')
if not 1 <= args.workers <= 8:
    parser.error('workers must be within 1..8')

bank = base / 'out/entry-builder/templates/tray-length-bank'
prefix = 'com.l410.linux.tray_'


def build(length):
    bundle = prefix + 'x' * length
    folder = bank / str(length)
    folder.mkdir(parents=True, exist_ok=True)
    output = folder / 'modules.abc'
    if output.is_file() and output.read_bytes().count(bundle.encode()) >= 6:
        return 'TRAY_LENGTH_TEMPLATE_CACHED %d %d' % (length, output.stat().st_size)
    workspace = base / 'out/entry-builder/tray-template-work' / str(length)
    generated = workspace / 'source'
    project = workspace / 'project'
    generated.mkdir(parents=True, exist_ok=True)
    entry_js = generated / 'EntryAbility.js'
    tray_js = generated / 'TrayServiceAbility.js'
    entry_js.write_text((base / 'src/EntryAbility.js').read_text(encoding='utf8').replace(
        '__L410_BUNDLE__', bundle), encoding='utf8')
    tray_js.write_text((base / 'src/TrayServiceAbility.js').read_text(encoding='utf8').replace(
        '__L410_BUNDLE__', bundle), encoding='utf8')
    result = subprocess.run([
        sys.executable, str(base / 'scripts/build-entry-abc-deveco.py'),
        '--bundle', bundle, '--label', 'Linux 托盘服务',
        '--entry-js', str(entry_js), '--tray-service-js', str(tray_js),
        '--output', str(output), '--project', str(project),
    ], capture_output=True, text=True, encoding='utf8', errors='replace')
    if result.returncode:
        raise RuntimeError('template %d failed:\n%s' %
                           (length, (result.stdout + result.stderr)[-3000:]))
    data = output.read_bytes()
    if data.count(bundle.encode()) < 6:
        raise RuntimeError(f'template {length} lacks complete bundle records')
    return 'TRAY_LENGTH_TEMPLATE_READY %d %d' % (length, len(data))


with ThreadPoolExecutor(max_workers=args.workers) as executor:
    futures = {executor.submit(build, length): length
               for length in range(args.minimum, args.maximum + 1)}
    for future in as_completed(futures):
        print(future.result(), flush=True)
