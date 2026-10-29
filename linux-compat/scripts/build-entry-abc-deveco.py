#!/usr/bin/env python3
"""Compile one Linux launcher ABC with Huawei's ArkTS lazy-kit frontend.

The ordinary launcher logic remains JavaScript.  A tiny AbilityStage supplies
the StatusBarExtensionKit factory in the same form used by Qt OHOS.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

BASE = Path(__file__).resolve().parents[1]


def write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")


def deveco_root() -> Path:
    candidates = []
    if os.environ.get("DEVECO_ROOT"):
        candidates.append(Path(os.environ["DEVECO_ROOT"]))
    candidates.extend([
        Path(r"D:\Program Files\Huawei\DevEco Studio"),
        Path(r"E:\Huawei\DevEco_Studio"),
    ])
    for root in candidates:
        if (root / "tools/hvigor/bin/hvigorw.bat").is_file():
            return root
    raise FileNotFoundError("DevEco Studio with hvigor was not found")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bundle", required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--entry-js", type=Path, required=True)
    parser.add_argument("--tray-service-js", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--project", type=Path, required=True)
    args = parser.parse_args()
    if not re.fullmatch(r"com\.l410\.linux\.(?:app\.)?[a-z][a-z0-9_]{0,80}", args.bundle):
        raise ValueError("invalid Linux launcher bundle")

    project = args.project.resolve()
    project.mkdir(parents=True, exist_ok=True)
    entry = project / "entry"
    source = entry / "src/main"
    for folder in (
        project / "AppScope",
        source / "ets/entryability",
        source / "ets/entrystage",
        source / "ets/pages",
        source / "resources/base/element",
        source / "resources/base/media",
        source / "resources/base/profile",
        source / "cpp/types/libl410uihost",
        entry / "libs/arm64-v8a",
        project / "hvigor",
    ):
        folder.mkdir(parents=True, exist_ok=True)

    write(project / "AppScope/app.json5", json.dumps({"app": {
        "bundleName": args.bundle, "vendor": "L410", "versionCode": 1,
        "versionName": "0.1.0", "icon": "$media:app_icon", "label": "$string:app_name",
    }}, ensure_ascii=False))
    write(project / "build-profile.json5", json.dumps({
        "app": {
            "signingConfigs": [],
            "products": [{"name": "default", "signingConfig": "default",
                          "compatibleSdkVersion": "6.0.0(20)",
                          "targetSdkVersion": "6.0.0(20)", "runtimeOS": "HarmonyOS"}],
            "buildModeSet": [{"name": "debug"}, {"name": "release"}],
        },
        "modules": [{"name": "entry", "srcPath": "./entry",
                     "targets": [{"name": "default", "applyToProducts": ["default"]}]}],
    }, ensure_ascii=False))
    write(project / "hvigorfile.ts",
          "import { appTasks } from '@ohos/hvigor-ohos-plugin';\n"
          "export default { system: appTasks, plugins: [] }\n")
    write(project / "hvigor/hvigor-config.json5", '{"modelVersion":"5.0.0","dependencies":{}}')
    write(project / "oh-package.json5",
          '{"modelVersion":"5.0.0","name":"l410-linux-entry","version":"0.1.0",'
          '"dependencies":{},"devDependencies":{}}')
    write(entry / "hvigorfile.ts",
          "import { hapTasks } from '@ohos/hvigor-ohos-plugin';\n"
          "export default { system: hapTasks, plugins: [] }\n")
    write(entry / "build-profile.json5",
          '{"apiType":"stageMode","buildOption":{},"targets":[{"name":"default"}]}')
    write(entry / "oh-package.json5",
          '{"modelVersion":"5.0.0","name":"entry","version":"0.1.0",'
          '"dependencies":{},"devDependencies":{"@types/libl410uihost.so":'
          '"file:./src/main/cpp/types/libl410uihost"}}')
    write(source / "module.json5", json.dumps({"module": {
        "name": "entry", "type": "entry",
        "srcEntry": "./ets/entrystage/EntryStage.ets",
        "deviceTypes": ["2in1", "tablet"], "deliveryWithInstall": True,
        "installationFree": False, "pages": "$profile:main_pages",
        "mainElement": "EntryAbility",
        "abilities": [{
            "name": "EntryAbility", "srcEntry": "./ets/entryability/EntryAbility.js",
            "exported": True, "icon": "$media:app_icon", "label": "$string:app_name",
            "startWindowIcon": "$media:app_icon",
            "startWindowBackground": "$color:start_window_background",
            "launchType": "singleton",
            "skills": [{"entities": ["entity.system.home"],
                        "actions": ["ohos.want.action.home"]}],
        }],
        "extensionAbilities": [{
            "name": "TrayServiceAbility",
            "srcEntry": "./ets/entryability/TrayServiceAbility.js",
            "type": "service", "exported": False, "visible": False,
            "description": "Linux StatusNotifierItem background host",
        }],
    }}, ensure_ascii=False))

    shutil.copyfile(args.entry_js, source / "ets/entryability/EntryAbility.js")
    shutil.copyfile(args.tray_service_js, source / "ets/entryability/TrayServiceAbility.js")
    shutil.copyfile(BASE / "src/EntryStage.ets", source / "ets/entrystage/EntryStage.ets")
    shutil.copyfile(BASE / "src/StatusBarFactories.ts", source / "ets/entrystage/StatusBarFactories.ts")
    shutil.copyfile(BASE / "src/libl410uihost.d.ts",
                    source / "cpp/types/libl410uihost/index.d.ts")
    write(source / "cpp/types/libl410uihost/oh-package.json5",
          '{"name":"libl410uihost.so","types":"./index.d.ts","version":"0.1.0",'
          '"description":"L410 Linux entry native bridge"}')
    write(source / "ets/pages/LinuxEntry.ets", """@Entry
@Component
struct LinuxEntry {
  build() {
    Column() {
      Text('Linux 应用启动失败').fontSize(24)
    }.width('100%').height('100%').justifyContent(FlexAlign.Center)
  }
}
""")
    write(source / "resources/base/element/string.json", json.dumps({"string": [
        {"name": "app_name", "value": args.label},
    ]}, ensure_ascii=False))
    write(source / "resources/base/element/color.json",
          '{"color":[{"name":"start_window_background","value":"#F2F4F7"}]}')
    write(source / "resources/base/profile/main_pages.json", '{"src":["pages/LinuxEntry"]}')
    shutil.copyfile(BASE / "assets/generic-entry.svg",
                    source / "resources/base/media/app_icon.svg")
    shutil.copyfile(BASE / "out/libl410uihost.so", entry / "libs/arm64-v8a/libl410uihost.so")

    root = deveco_root()
    env = os.environ.copy()
    env["DEVECO_SDK_HOME"] = str(root / "sdk")
    env["NODE_HOME"] = str(root / "tools/node")
    env["JAVA_HOME"] = str(root / "jbr")
    env["PATH"] = os.pathsep.join((str(root / "tools/node"), str(root / "jbr/bin"), env["PATH"]))
    command = [str(root / "tools/hvigor/bin/hvigorw.bat"), "--mode", "module",
               "-p", "module=entry@default", "-p", "product=default",
               "assembleHap", "--no-daemon"]
    subprocess.run(command, cwd=project, env=env, check=True)
    hap = entry / "build/default/outputs/default/entry-default-unsigned.hap"
    with zipfile.ZipFile(hap) as archive:
        data = archive.read("ets/modules.abc")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    print("DEVECO_ENTRY_ABC", args.bundle, len(data), flush=True)


if __name__ == "__main__":
    main()
