"""Inspect and install one HAP selected by the dedicated Harmony application."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import threading
import time
import zipfile

BASE = Path('/data/vendor/l410-linux')
CODE = Path('/vendor/bin/l410-linux-control')
SOURCE = Path('/data/app/el2/100/base/com.l410.linux.hapinstaller/files/incoming.hap')
STAGING = Path('/data/local/tmp/l410-linux-install/hapinstaller-incoming.hap')
LOG = BASE / 'log/hap-installer.log'
BUILDER_PUBLIC = Path('/vendor/l410-linux/entry-builder')
BUILDER_PRIVATE = BASE / 'entry-builder/private'
BUILD_WORK = BASE / 'entry-build-work'
ROOTFS = BASE / 'rootfs-bookworm'
QT_RUNTIME = Path('/vendor/l410/qt-hap-runtime/api20-qt6.13-r8b')
QT_RUNTIME_PROFILE = 'api20-qt6.13-r8b'
QT_RUNTIME_REPLACEMENTS = {
    'libs/arm64-v8a/libQt6Core.so': 'libQt6Core.so',
    'libs/arm64-v8a/libqohos.so': 'libqohos.so',
}
QT_RUNTIME_ANCHORS = {
    'libs/arm64-v8a/libQt6Gui.so': 'libQt6Gui.so',
    'libs/arm64-v8a/libQt6Widgets.so': 'libQt6Widgets.so',
    'libs/arm64-v8a/libQt6Network.so': 'libQt6Network.so',
    'libs/arm64-v8a/libQt6OpenGL.so': 'libQt6OpenGL.so',
    'libs/arm64-v8a/libQt6Svg.so': 'libQt6Svg.so',
}
ENTER_BUILDER = [
    '/data/local/tmp/boot-tools-island/lib/ld-linux-aarch64.so.1',
    '--library-path', '/data/local/tmp/boot-tools-island/lib',
    str(CODE / 'enter'), str(ROOTFS), '--builder'
]
BUNDLE = re.compile(r'[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+')
lock = threading.Lock()
state = {'state': 'idle', 'output': '', 'package': None}


def _zip_member_equals_file(archive, member, path):
    if not path.is_file() or archive.getinfo(member).file_size != path.stat().st_size:
        return False
    with archive.open(member) as left, path.open('rb') as right:
        while True:
            left_chunk = left.read(1024 * 1024)
            right_chunk = right.read(1024 * 1024)
            if left_chunk != right_chunk:
                return False
            if not left_chunk:
                return True


def _qt_runtime_plan(archive):
    names = set(archive.namelist())
    present = [name for name in QT_RUNTIME_REPLACEMENTS if name in names]
    if not present:
        return {'detected': False, 'eligible': False,
                'profile': QT_RUNTIME_PROFILE, 'reason': '不是 Qt OHOS HAP'}
    if len(present) != len(QT_RUNTIME_REPLACEMENTS):
        return {'detected': True, 'eligible': False,
                'profile': QT_RUNTIME_PROFILE, 'reason': 'Qt 运行库成员不完整'}
    required = 'libs/arm64-v8a/libQt6Gui.so'
    if required not in names:
        return {'detected': True, 'eligible': False,
                'profile': QT_RUNTIME_PROFILE, 'reason': '缺少 Qt GUI ABI 基准'}
    missing_profile = [filename for filename in (*QT_RUNTIME_REPLACEMENTS.values(),
                       *QT_RUNTIME_ANCHORS.values()) if not (QT_RUNTIME / filename).is_file()]
    if missing_profile:
        return {'detected': True, 'eligible': False,
                'profile': QT_RUNTIME_PROFILE, 'reason': '设备 Qt 兼容运行库未就绪'}
    compared = []
    for member, filename in QT_RUNTIME_ANCHORS.items():
        if member not in names:
            continue
        compared.append(filename)
        if not _zip_member_equals_file(archive, member, QT_RUNTIME / filename):
            return {'detected': True, 'eligible': False,
                    'profile': QT_RUNTIME_PROFILE,
                    'reason': filename + ' 与已验证 Qt ABI 不一致'}
    return {'detected': True, 'eligible': True, 'profile': QT_RUNTIME_PROFILE,
            'reason': '将自动应用已验证的 Qt 输入与窗口生命周期运行库',
            'anchors': compared,
            'replacements': list(QT_RUNTIME_REPLACEMENTS.values())}


def _manifest():
    source = SOURCE.resolve(strict=True)
    if source != SOURCE or source.is_symlink() or not source.is_file():
        raise ValueError('未找到由 HAP 安装器选择的文件')
    size = source.stat().st_size
    if not 1024 <= size <= 1024 * 1024 * 1024:
        raise ValueError('HAP 大小必须在 1 KiB 到 1 GiB 之间')
    with zipfile.ZipFile(source) as archive:
        info = archive.getinfo('module.json')
        if info.file_size > 2 * 1024 * 1024:
            raise ValueError('module.json 过大')
        manifest = json.loads(archive.read(info))
        qt_runtime = _qt_runtime_plan(archive)
    app = manifest.get('app', {})
    module = manifest.get('module', {})
    bundle = app.get('bundleName', '')
    if not BUNDLE.fullmatch(bundle):
        raise ValueError('HAP bundleName 无效')
    abilities = [item.get('name', '') for item in module.get('abilities', [])
                 if isinstance(item, dict) and item.get('name')]
    return {'bundle': bundle, 'versionName': str(app.get('versionName', '')),
            'versionCode': app.get('versionCode', 0),
            'minAPIVersion': app.get('minAPIVersion', 0),
            'targetAPIVersion': app.get('targetAPIVersion', 0),
            'deviceTypes': module.get('deviceTypes', []),
            'module': str(module.get('name', '')),
            'abilities': abilities[:16], 'bytes': size,
            'qtRuntime': qt_runtime}


def inspect():
    return {'package': _manifest(), 'state': status()['state']}


def _bms_identity():
    probe = subprocess.run(['/system/bin/timeout', '8',
        '/data/local/tmp/audit-alignment-v306/native-install-token-probe'],
        capture_output=True, text=True, timeout=12)
    output = (probe.stdout or '') + (probe.stderr or '')
    match = re.search(r'^bms_install_tool_acl token=([0-9]+) install_permission=0$',
                      probe.stdout or '', re.MULTILINE)
    if probe.returncode or not match or match.group(1) == '0':
        raise RuntimeError('无法取得已验证的 BMS 安装身份')
    return match.group(1), output


def _install_command(package):
    token, output = _bms_identity()
    identity = ['/data/local/tmp/identity_token_exec_v3', token,
                '2000', '2000', '/system/bin/bm']
    installed_hap = Path('/data/app/el1/bundle/public') / package['bundle'] / 'entry.hap'
    if installed_hap.is_file():
        removed = subprocess.run(['/system/bin/timeout', '60', *identity,
            'uninstall', '-n', package['bundle'], '-k', '-u', '100'],
            capture_output=True, text=True, timeout=65)
        output += (removed.stdout or '') + (removed.stderr or '')
        if removed.returncode or 'uninstall bundle successfully' not in removed.stdout:
            raise RuntimeError('无法保留应用数据并刷新原生运行库')
    command = [*identity, 'install', '-p', str(STAGING), '-u', '100']
    installed = subprocess.run(['/system/bin/timeout', '90', *command],
                               capture_output=True, text=True, timeout=95)
    output += (installed.stdout or '') + (installed.stderr or '')
    return installed, output


def _prepare_staging(package):
    """Copy the selected HAP, applying the verified Qt runtime when eligible."""
    qt_runtime = package.get('qtRuntime', {})
    if not qt_runtime.get('eligible'):
        shutil.copyfile(SOURCE, STAGING)
        os.chmod(STAGING, 0o644)
        return False
    candidate = STAGING.with_suffix('.new')
    candidate.unlink(missing_ok=True)
    replaced = []
    try:
        with zipfile.ZipFile(SOURCE, 'r') as old, zipfile.ZipFile(
                candidate, 'w', compression=zipfile.ZIP_STORED, allowZip64=True) as new:
            for info in old.infolist():
                if info.filename.upper().startswith('META-INF/'):
                    continue
                replacement = QT_RUNTIME_REPLACEMENTS.get(info.filename)
                source = ((QT_RUNTIME / replacement).open('rb') if replacement
                          else old.open(info, 'r'))
                with source, new.open(info, 'w', force_zip64=True) as target:
                    shutil.copyfileobj(source, target, 1024 * 1024)
                if replacement:
                    replaced.append(info.filename)
        if set(replaced) != set(QT_RUNTIME_REPLACEMENTS):
            raise RuntimeError('Qt 运行库替换集合不完整')
        candidate.replace(STAGING)
        os.chmod(STAGING, 0o644)
        return True
    finally:
        candidate.unlink(missing_ok=True)


def _local_development_sign(package):
    """Sign an unsigned HAP with the already deployed local development chain.

    The selected source is never modified.  The signing workspace lives on the
    external project volume and is removed after its signed result is copied to
    the fixed BMS staging path.
    """
    required = [BUILDER_PUBLIC / 'hap-sign-tool.jar', BUILDER_PRIVATE / 'profile.json',
                BUILDER_PRIVATE / 'profile.p12', BUILDER_PRIVATE / 'profile-chain.cer',
                BUILDER_PRIVATE / 'tile.p12', BUILDER_PRIVATE / 'tile-chain.cer',
                BUILDER_PRIVATE / 'key-password.json']
    if not all(path.is_file() for path in required):
        raise RuntimeError('本机开发签名组件不完整')
    BUILD_WORK.mkdir(mode=0o700, parents=True, exist_ok=True)
    folder = BUILD_WORK / ('hap-installer-%d-%d' % (os.getpid(), time.time_ns()))
    folder.mkdir(mode=0o700)
    relative = folder.relative_to(BUILD_WORK).as_posix()
    guest = Path('/run/l410-build') / relative
    private = Path('/run/l410-builder/private')
    try:
        unsigned = folder / 'unsigned.hap'
        shutil.copyfile(STAGING, unsigned)
        profile = json.loads((BUILDER_PRIVATE / 'profile.json').read_text())
        profile['bundle-info']['bundle-name'] = package['bundle']
        profile['bundle-info']['app-identifier'] = '5765880207859999999'
        profile['acls']['allowed-acls'] = []
        profile.pop('app-privilege-capabilities', None)
        (folder / 'profile.json').write_text(json.dumps(profile))
        password = json.loads((BUILDER_PRIVATE / 'key-password.json').read_text())['password']

        def sign(kind, arguments):
            result = subprocess.run([*ENTER_BUILDER, '/usr/bin/java', '-Xmx256m',
                '-jar', '/run/l410-builder/hap-sign-tool.jar', kind,
                *map(str, arguments)], capture_output=True, text=True, timeout=120)
            text = ((result.stdout or '') + (result.stderr or '')).replace(password, '[redacted]')
            if result.returncode:
                raise RuntimeError('本机开发签名失败：' + text[-1200:])
            return text

        signing_output = sign('sign-profile', ['-mode', 'localSign',
            '-keyAlias', 'profile', '-keyPwd', password,
            '-profileCertFile', private / 'profile-chain.cer',
            '-inFile', guest / 'profile.json', '-signAlg', 'SHA384withECDSA',
            '-keystoreFile', private / 'profile.p12', '-keystorePwd', password,
            '-outFile', guest / 'profile.p7b'])
        signing_output += sign('sign-app', ['-mode', 'localSign',
            '-keyAlias', 'tile', '-keyPwd', password,
            '-appCertFile', private / 'tile-chain.cer',
            '-profileFile', guest / 'profile.p7b', '-inFile', guest / 'unsigned.hap',
            '-signAlg', 'SHA384withECDSA', '-keystoreFile', private / 'tile.p12',
            '-keystorePwd', password, '-outFile', guest / 'signed.hap',
            '-compatibleVersion', '20'])
        signed = folder / 'signed.hap'
        if not signed.is_file() or signed.stat().st_size < STAGING.stat().st_size:
            raise RuntimeError('本机开发签名没有生成有效 HAP')
        shutil.copyfile(signed, STAGING)
        os.chmod(STAGING, 0o644)
        return signing_output
    finally:
        shutil.rmtree(folder, ignore_errors=True)


def _run_install(package):
    global state
    output = []
    try:
        STAGING.parent.mkdir(mode=0o755, parents=True, exist_ok=True)
        qt_adapted = _prepare_staging(package)
        locally_signed = qt_adapted
        if qt_adapted:
            output.append('已应用 Qt 兼容运行库 ' + QT_RUNTIME_PROFILE + '。\n')
            output.append(_local_development_sign(package))
            installed, attempt = _install_command(package)
            output.append(attempt)
        else:
            installed, attempt = _install_command(package)
            output.append(attempt)
        if (not qt_adapted and
                (installed.returncode or 'install bundle successfully' not in installed.stdout) and
                ('9568320' in attempt or 'no signature file' in attempt.lower())):
            output.append('\n检测到安装包未签名，正在使用本机开发签名后重试。\n')
            output.append(_local_development_sign(package))
            installed, attempt = _install_command(package)
            output.append(attempt)
            locally_signed = True
        if installed.returncode or 'install bundle successfully' not in installed.stdout:
            raise RuntimeError('系统安装服务拒绝了这个 HAP')
        target = Path('/data/app/el1/bundle/public') / package['bundle'] / 'entry.hap'
        if not target.is_file():
            raise RuntimeError('安装服务返回成功，但未找到已安装的 entry.hap')
        with lock:
            state = {'state': 'installed', 'output': ''.join(output)[-12000:],
                     'package': dict(package, localDevelopmentSigned=locally_signed,
                                     qtRuntimeAdapted=qt_adapted)}
    except Exception as error:
        output.append('\nERROR: ' + str(error))
        with lock:
            state = {'state': 'error', 'output': ''.join(output)[-12000:],
                     'package': package, 'error': str(error)}
    finally:
        STAGING.unlink(missing_ok=True)
        STAGING.with_suffix('.new').unlink(missing_ok=True)
        LOG.parent.mkdir(parents=True, exist_ok=True)
        LOG.write_text(state.get('output', ''), errors='replace')


def start():
    global state
    package = _manifest()
    with lock:
        if state.get('state') == 'installing':
            raise ValueError('已有 HAP 正在安装')
        state = {'state': 'installing', 'output': '已提交系统安装服务',
                 'package': package}
    threading.Thread(target=_run_install, args=(package,), daemon=True).start()
    return status()


def status():
    with lock:
        return json.loads(json.dumps(state, ensure_ascii=False))
