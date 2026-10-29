"""Precompile explicitly named native launcher templates.

Each Linux desktop file owns a stable Harmony bundle identity. Numeric slot
banks are deliberately unsupported: callers must pass the formal bundle that
will be installed on the device.
"""
from pathlib import Path
import argparse
import json
import re
import shutil
import subprocess
import sys

base = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--bundle', action='append', required=True,
                    help='formal bundle, for example com.l410.linux.app.org_xfce_mousepad')
args = parser.parse_args()
bundles = list(dict.fromkeys(args.bundle))
for bundle in bundles:
    if not re.fullmatch(r'com\.l410\.linux\.(?:app\.)?[a-z][a-z0-9_]{0,80}', bundle):
        parser.error(f'invalid formal Linux bundle: {bundle}')

bank = base / 'out/entry-builder/templates'
bank.mkdir(parents=True, exist_ok=True)
deveco_project = base / 'out/entry-builder/deveco-entry-project'
for bundle in bundles:
    folder = bank / bundle
    if folder.exists():
        shutil.rmtree(folder)
    folder.mkdir()
    records = []
    tray_source = (base / 'src/TrayServiceAbility.js').read_text(encoding='utf8').replace(
        '__L410_BUNDLE__', bundle)
    tray_generated = folder / 'TrayServiceAbility.js'
    tray_generated.write_text(tray_source, encoding='utf8')
    for name, record in (
        ('EntryAbility', 'entry/ets/entryability/EntryAbility'),
        ('LinuxEntry', 'entry/ets/pages/LinuxEntry'),
    ):
        source = (base / 'src' / f'{name}.js').read_text(encoding='utf8').replace(
            '__L410_BUNDLE__', bundle)
        generated = folder / f'{name}.js'
        generated.write_text(source, encoding='utf8')
        record = bundle + '/' + record
        records.append(f'{generated.as_posix()};{record};esm;{record}.js;entry;false;js')
    (folder / 'files.txt').write_text('\n'.join(records) + '\n', encoding='utf8')
    subprocess.run([
        sys.executable, str(base / 'scripts/build-entry-abc-deveco.py'),
        '--bundle', bundle, '--label', 'Linux 应用',
        '--entry-js', str(folder / 'EntryAbility.js'),
        '--tray-service-js', str(tray_generated),
        '--output', str(folder / 'modules.abc'), '--project', str(deveco_project),
    ], check=True)
    print('TEMPLATE_READY', bundle, (folder / 'modules.abc').stat().st_size, flush=True)

manifest = json.loads((base / 'entry-featherpad/module.json').read_text(encoding='utf8'))
manifest['app']['icon'] = '$media:app_icon'
manifest['module']['mainElement'] = 'EntryAbility'
manifest['module']['srcEntry'] = './ets/entrystage/EntryStage.ets'
manifest['module']['requestPermissions'] = []
manifest['module']['abilities'] = [{
    'name': 'EntryAbility', 'srcEntry': './ets/entryability/EntryAbility.js',
    'exported': True, 'icon': '$media:app_icon', 'label': '$string:app_name',
    'launchType': 'singleton',
    'skills': [{'entities': ['entity.system.home'], 'actions': ['ohos.want.action.home']}],
}]
manifest['module']['extensionAbilities'] = [{
    'name': 'TrayServiceAbility', 'srcEntry': './ets/entryability/TrayServiceAbility.js',
    'type': 'service', 'exported': False, 'visible': False,
    'description': 'Linux StatusNotifierItem background host',
}]
(bank / 'module.json').write_text(json.dumps(manifest, ensure_ascii=False), encoding='utf8')
known = sorted(path.name for path in bank.iterdir()
               if path.is_dir() and re.fullmatch(
                   r'com\.l410\.linux\.(?:app\.)?[a-z][a-z0-9_]{0,80}', path.name))
(bank / 'bank.json').write_text(json.dumps({
    'version': 5, 'bundles': known,
    'dispatch': 'anco-ability-alias+sni-host+arkts-statusbar-factory',
}, ensure_ascii=False), encoding='utf8')
