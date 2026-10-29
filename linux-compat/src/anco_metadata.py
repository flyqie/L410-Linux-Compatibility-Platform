"""Linux package record -> original AnCo ordinary launcher registration JSON.

Wire source: hwServices-v128.jar converter/info constructors and toJson,
Converter, ConstantMapper, InstallerType and OHLauncheMode. This serializer
does not allocate identities, install packages, supply Android resources,
change caller credentials or claim BMS acceptance.
"""
import json


# Original AnCo ResourceManager resolves this media ID in the virtual package.
ICON_RESOURCE_ID = 0x01000001


def ordinary_launcher(record):
    """Serialize a trusted persistent package record; no guessed app identity.

    version_code is the catalog's persistent monotonically increasing package
    revision, not a numeric coercion of a Debian version. Times are milliseconds.
    Android resource IDs/certificates remain empty when the Linux app has none.
    Window modes/orientation must be explicitly projected by the caller.
    """
    def text(key):
        value = record[key]
        if not isinstance(value, str) or not value or '\0' in value:
            raise ValueError(key)
        return value

    def number(key, low, high):
        value = record[key]
        if type(value) is not int or not low <= value <= high:
            raise ValueError(key)
        return value

    bundle, ability = text('bundle'), text('ability')
    label, version = text('label'), text('version')
    app_id = number('app_id', 10000, 19999)
    guest_user = number('guest_user', 0, 21473)
    code_path = text('code_path')
    if not code_path.startswith('/'):
        raise ValueError('code_path must be guest absolute path')
    modes = record['window_modes']
    if (not isinstance(modes, list) or any(type(x) is not int for x in modes) or
            any(x < 0 or x > 3 for x in modes) or len(set(modes)) != len(modes)):
        raise ValueError('window_modes')
    privacy = record['privacy']
    if type(privacy) is not bool:
        raise ValueError('privacy')
    app = {
        'name': label, 'bundleName': bundle, 'iconId': 0, 'labelId': 0,
        'versionCode': number('version_code', 0, 2**63 - 1),
        'versionName': version, 'virtualAppType': 1, 'removable': True,
        # Linux desktop visibility is owned by the resource-bearing launcher
        # alias selected in the manager. The virtual AnCo ability remains an
        # explicit start target but must not create a second iconless tile.
        'label': label, 'hideDesktopIcon': False, 'fingerprint': '',
        'uid': app_id, 'installerType': 0, 'isModuleUpdate': False,
        'systemApp': False,
        'installTime': number('install_time_ms', 0, 2**63 - 1),
        'updateTime': number('update_time_ms', 0, 2**63 - 1),
        'targetSdkVersion': 0, 'codePath': code_path, 'isForceRotate': False,
        'appSourceDirs': [], 'userIds': [guest_user] if privacy else [],
        'signingCertificateHistory': [],
    }
    component = {
        'name': ability, 'iconId': ICON_RESOURCE_ID, 'labelId': 0, 'descriptionId': 0,
        'startWindowIconId': ICON_RESOURCE_ID, 'startWindowBackgroundId': 0,
        'visible': True, 'launchMode': 1,
        'orientation': number('orientation', 0, 2147483647),
        'enabled': True, 'userIdToEnableMap': {}, 'label': label,
        'permission': '', 'metadata': [], 'supportWindowModes': list(modes),
        'preferMultiWindowOrientation': '',
    }
    return {
        'applicationInfo': app,
        'moduleInfos': {'entry': {'moduleName': '.entry',
                                'abilityKeys': [ability], 'metadata': []}},
        'abilityInfos': {ability: component},
        'skillInfos': {ability: [{'actions': ['ohos.want.action.home'],
                                'entities': ['entity.system.home'], 'uris': []}]},
    }


def encode(record):
    return json.dumps(ordinary_launcher(record), ensure_ascii=False,
                      separators=(',', ':'), allow_nan=False).encode('utf-8')


if __name__ == '__main__':
    import argparse
    from pathlib import Path
    import sys
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('record', type=Path)
    args = parser.parse_args()
    sys.stdout.buffer.write(encode(json.loads(args.record.read_text('utf-8'))) + b'\n')
