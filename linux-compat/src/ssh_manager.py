"""Transactional management for the device's init-managed Dropbear service.

The Harmony manager talks to this module through broker.py's existing local
credential-checked socket.  No SSH protocol is implemented here: this only
controls the already-installed Dropbear server, its port and its host key.
"""
from pathlib import Path
import base64
import os
import re
import shutil
import socket
import subprocess
import threading
import time


SSH_DIR = Path('/data/service/el1/public/ssh')
CONFIG = SSH_DIR / 'l410-ssh.conf'
HOST_KEY = SSH_DIR / 'dropbear_ecdsa_host_key'
PUBLIC_KEY = SSH_DIR / 'dropbear_ecdsa_host_key.pub'
CLIENT_PRIVATE_KEY = SSH_DIR / 'l410_client_ecdsa'
CLIENT_PUBLIC_KEY = SSH_DIR / 'l410_client_ecdsa.pub'
AUTHORIZED_DIR = Path('/data/local/tmp/ssh-root/.ssh')
AUTHORIZED_KEYS = AUTHORIZED_DIR / 'authorized_keys'
PID_FILE = SSH_DIR / 'dropbear.pid'
LOG_FILE = SSH_DIR / 'dropbear.log'
SERVICE = 'l410_dropbear'
LOADER = '/vendor/lib64/l410-kylin-net/ld-linux-aarch64.so.1'
LIBDIR = '/vendor/lib64/l410-kylin-net'
DROPBEAR = '/vendor/bin/l410-kylin-net/dropbear'
DROPBEARKEY = '/vendor/bin/l410-kylin-net/dropbearkey'
ROOTFS = Path('/data/vendor/l410-linux/rootfs-bookworm')
ROOTFS_LOADER = ROOTFS / 'lib/ld-linux-aarch64.so.1'
ROOTFS_LIBDIR = str(ROOTFS / 'usr/lib/aarch64-linux-gnu') + ':' + str(ROOTFS / 'lib/aarch64-linux-gnu')
ROOTFS_DROPBEARKEY = ROOTFS / 'usr/bin/dropbearkey'
ROOTFS_DROPBEARCONVERT = ROOTFS / 'usr/bin/dropbearconvert'
DEFAULT = {'enabled': False, 'port': 2222}
PORT_MIN = 1024
PORT_MAX = 65535
_lock = threading.RLock()


def _config_text(value):
    return 'ENABLED=%d\nPORT=%d\n' % (1 if value['enabled'] else 0, value['port'])


def _validate_port(value):
    if type(value) is not int or value < PORT_MIN or value > PORT_MAX:
        raise ValueError('SSH 端口必须是 1024–65535 的整数')
    return value


def _read_config():
    result = dict(DEFAULT)
    try:
        text = CONFIG.read_text(errors='strict')
    except FileNotFoundError:
        return result
    except OSError as error:
        raise ValueError('无法读取 SSH 配置：' + str(error))
    enabled = re.search(r'(?m)^ENABLED=([01])$', text)
    port = re.search(r'(?m)^PORT=([0-9]{1,5})$', text)
    if not enabled or not port:
        raise ValueError('SSH 配置格式无效')
    result['enabled'] = enabled.group(1) == '1'
    result['port'] = _validate_port(int(port.group(1)))
    return result


def _write_config(value):
    _validate_port(value['port'])
    SSH_DIR.mkdir(parents=True, exist_ok=True)
    if CONFIG.is_symlink():
        raise ValueError('拒绝写入符号链接形式的 SSH 配置')
    candidate = CONFIG.with_name(CONFIG.name + '.new')
    candidate.write_text(_config_text(value))
    os.chmod(candidate, 0o600)
    os.replace(candidate, CONFIG)


def ensure_config():
    with _lock:
        if not CONFIG.exists():
            _write_config(DEFAULT)
        return _read_config()


def _cmdline(pid):
    try:
        return Path('/proc/%d/cmdline' % pid).read_bytes().split(b'\0')
    except OSError:
        return []


def _master_pid():
    try:
        pid = int(PID_FILE.read_text().strip())
    except (OSError, ValueError):
        return 0
    fields = _cmdline(pid)
    if any(field.decode(errors='replace') == DROPBEAR for field in fields):
        return pid
    return 0


def _actual_port(pid):
    fields = [field.decode(errors='replace') for field in _cmdline(pid)]
    for index, value in enumerate(fields[:-1]):
        if value == '-p':
            spec = fields[index + 1]
            tail = spec.rsplit(':', 1)[-1]
            if tail.isdigit():
                return int(tail)
    return 0


def _listener_present(port):
    encoded = '%04X' % port
    for table in ('/proc/net/tcp', '/proc/net/tcp6'):
        try:
            lines = Path(table).read_text().splitlines()[1:]
        except OSError:
            continue
        for line in lines:
            fields = line.split()
            if len(fields) > 3 and fields[1].rsplit(':', 1)[-1].upper() == encoded and fields[3] == '0A':
                return True
    return False


def _child_connections(master):
    count = 0
    for stat in Path('/proc').glob('[0-9]*/stat'):
        try:
            fields = stat.read_text().rsplit(')', 1)[1].split()
            if int(fields[1]) != master:
                continue
            command = _cmdline(int(stat.parent.name))
            if any(field.decode(errors='replace') == DROPBEAR for field in command):
                count += 1
        except (OSError, ValueError, IndexError):
            continue
    return count


def _public_line(key=HOST_KEY):
    if not key.is_file() or key.is_symlink():
        raise ValueError('Dropbear 主机私钥不存在或类型无效')
    result = subprocess.run(
        [LOADER, '--library-path', LIBDIR, DROPBEARKEY, '-y', '-f', str(key)],
        capture_output=True, text=True, timeout=10)
    if result.returncode:
        raise ValueError('Dropbear 主机私钥无法读取')
    for line in result.stdout.splitlines():
        if line.startswith(('ecdsa-sha2-', 'ssh-ed25519 ', 'ssh-rsa ')):
            return line.strip()
    raise ValueError('Dropbear 没有返回主机公钥')


def _publish_public(key=HOST_KEY, target=PUBLIC_KEY):
    line = _public_line(key)
    candidate = target.with_name(target.name + '.new')
    candidate.write_text(line + '\n')
    os.chmod(candidate, 0o644)
    os.replace(candidate, target)
    return line


def _wait_banner(port, timeout=5.0):
    deadline = time.monotonic() + timeout
    last = ''
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(('127.0.0.1', port), timeout=.5) as connection:
                banner = connection.recv(160).split(b'\n', 1)[0].rstrip(b'\r').decode('ascii', errors='replace')
                if banner.startswith('SSH-2.0-dropbear'):
                    return banner
                last = banner
        except OSError as error:
            last = str(error)
        time.sleep(.15)
    raise RuntimeError('端口 %d 未出现 Dropbear SSH banner：%s' % (port, last or '无响应'))


def _wait_stopped(port, timeout=5.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if not _listener_present(port):
            return
        time.sleep(.1)
    raise RuntimeError('端口 %d 的旧 SSH 监听未停止' % port)


def _service(action):
    result = subprocess.run(['/system/bin/begetctl', action + '_service', SERVICE],
                            capture_output=True, text=True, timeout=12)
    if result.returncode:
        raise RuntimeError('init %s SSH 失败：%s' %
                           ('启动' if action == 'start' else '停止',
                            ((result.stdout or '') + (result.stderr or '')).strip()))
    return result


def _start_official(port):
    _service('start')
    banner = _wait_banner(port, 8.0)
    try:
        _publish_public()
    except (OSError, ValueError, subprocess.SubprocessError):
        pass
    return banner


def _stop_official(port):
    _service('stop')
    _wait_stopped(port, 8.0)


def _unused_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    if port < PORT_MIN:
        raise RuntimeError('无法分配 SSH 候选端口')
    return port


def _candidate(key, port, loopback=False):
    pidfile = SSH_DIR / 'dropbear.candidate.pid'
    log = SSH_DIR / 'dropbear-candidate.log'
    pidfile.unlink(missing_ok=True)
    target = ('127.0.0.1:%d' % port) if loopback else str(port)
    output = log.open('ab', buffering=0)
    process = subprocess.Popen(
        [LOADER, '--library-path', LIBDIR, DROPBEAR, '-F', '-E', '-p', target,
         '-r', str(key), '-P', str(pidfile), '-s'],
        stdin=subprocess.DEVNULL, stdout=output, stderr=subprocess.STDOUT,
        start_new_session=True)
    try:
        banner = _wait_banner(port, 6.0)
        return process, output, banner
    except Exception:
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=2)
        output.close()
        pidfile.unlink(missing_ok=True)
        raise


def _finish_candidate(process, output, port):
    process.terminate()
    try:
        process.wait(timeout=3)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=2)
    output.close()
    (SSH_DIR / 'dropbear.candidate.pid').unlink(missing_ok=True)
    _wait_stopped(port, 3.0)


def status():
    config = ensure_config()
    master = _master_pid()
    actual = _actual_port(master) if master else 0
    public = ''
    try:
        if PUBLIC_KEY.is_file():
            public = PUBLIC_KEY.read_text(errors='strict').strip()
    except OSError:
        public = ''
    client_public = ''
    try:
        if CLIENT_PUBLIC_KEY.is_file():
            client_public = CLIENT_PUBLIC_KEY.read_text(errors='strict').strip()
    except OSError:
        client_public = ''
    return {
        'enabled': config['enabled'], 'configuredPort': config['port'],
        'running': bool(master and actual and _listener_present(actual)),
        'actualPort': actual, 'pid': master,
        'connections': _child_connections(master) if master else 0,
        'publicKey': public, 'hostKeyPresent': HOST_KEY.is_file() and not HOST_KEY.is_symlink(),
        'clientKeyPresent': CLIENT_PRIVATE_KEY.is_file(),
        'authorizedKeyPresent': AUTHORIZED_KEYS.is_file(),
        'clientPublicKey': client_public,
        'authentication': 'root 公钥认证', 'passwordLogin': False,
        'portRange': [PORT_MIN, PORT_MAX]
    }


def set_enabled(enabled):
    if type(enabled) is not bool:
        raise ValueError('enabled 必须是布尔值')
    with _lock:
        old = ensure_config()
        current = status()
        if enabled and current['running']:
            if not old['enabled']:
                _write_config(dict(old, enabled=True))
            return dict(status(), message='SSH 已经在运行')
        if not enabled and not current['running']:
            _write_config(dict(old, enabled=False))
            return dict(status(), message='SSH 已停用')
        candidate = dict(old, enabled=enabled)
        _write_config(candidate)
        try:
            if enabled:
                if not AUTHORIZED_KEYS.is_file():
                    raise RuntimeError('请先生成客户端登录密钥')
                banner = _start_official(candidate['port'])
                return dict(status(), message='SSH 已启用', banner=banner)
            _stop_official(current['actualPort'] or old['port'])
            return dict(status(), message='SSH 已停用；本机管理器仍可重新启用')
        except Exception as error:
            _write_config(old)
            if old['enabled'] and not status()['running']:
                try:
                    _start_official(old['port'])
                except Exception:
                    pass
            raise RuntimeError(str(error) + '；已恢复原 SSH 启用状态')


def change_port(port):
    port = _validate_port(port)
    with _lock:
        old = ensure_config()
        current = status()
        if port == old['port'] and (not current['running'] or current['actualPort'] == port):
            return dict(current, message='SSH 已使用端口 %d' % port)
        if _listener_present(port):
            raise ValueError('端口 %d 已被其他服务占用' % port)
        if not current['running']:
            _write_config(dict(old, port=port))
            return dict(status(), message='SSH 端口已保存为 %d；下次启用时生效' % port)
        process, output, banner = _candidate(HOST_KEY, port)
        _finish_candidate(process, output, port)
        updated = dict(old, port=port)
        _write_config(updated)
        try:
            if current['running']:
                _stop_official(current['actualPort'] or old['port'])
            if old['enabled']:
                banner = _start_official(port)
            result = status()
            return dict(result, message='SSH 端口已切换为 %d' % port, banner=banner,
                        reconnect='请使用新端口重新连接')
        except Exception as error:
            _write_config(old)
            try:
                running = status()
                if running['running']:
                    _stop_official(running['actualPort'])
                if old['enabled']:
                    _start_official(old['port'])
            except Exception as rollback:
                raise RuntimeError('%s；旧端口恢复失败：%s' % (error, rollback))
            raise RuntimeError(str(error) + '；已恢复旧端口 %d' % old['port'])


def regenerate_host_key():
    with _lock:
        config = ensure_config()
        current = status()
        candidate_key = SSH_DIR / 'dropbear_ecdsa_host_key.candidate'
        candidate_public = SSH_DIR / 'dropbear_ecdsa_host_key.pub.candidate'
        candidate_key.unlink(missing_ok=True)
        candidate_public.unlink(missing_ok=True)
        result = subprocess.run(
            [LOADER, '--library-path', LIBDIR, DROPBEARKEY, '-t', 'ecdsa', '-s', '256',
             '-f', str(candidate_key)], capture_output=True, text=True, timeout=15)
        if result.returncode or not candidate_key.is_file():
            raise RuntimeError('生成 ECDSA 主机密钥失败：' +
                               ((result.stdout or '') + (result.stderr or '')).strip())
        os.chmod(candidate_key, 0o600)
        public = _publish_public(candidate_key, candidate_public)
        test_port = _unused_port()
        process, output, _ = _candidate(candidate_key, test_port, loopback=True)
        _finish_candidate(process, output, test_port)
        previous_key = HOST_KEY.with_name(HOST_KEY.name + '.previous')
        previous_public = PUBLIC_KEY.with_name(PUBLIC_KEY.name + '.previous')
        if HOST_KEY.is_file():
            shutil.copyfile(HOST_KEY, previous_key)
            os.chmod(previous_key, 0o600)
        if PUBLIC_KEY.is_file():
            shutil.copyfile(PUBLIC_KEY, previous_public)
            os.chmod(previous_public, 0o644)
        os.replace(candidate_key, HOST_KEY)
        os.replace(candidate_public, PUBLIC_KEY)
        try:
            if current['running']:
                _stop_official(current['actualPort'] or config['port'])
            if config['enabled']:
                _start_official(config['port'])
            return dict(status(), message='ECDSA 主机密钥已重新生成', publicKey=public,
                        reconnect='客户端需要移除旧 known_hosts 记录后重新接受主机密钥')
        except Exception as error:
            if previous_key.is_file():
                shutil.copyfile(previous_key, HOST_KEY)
                os.chmod(HOST_KEY, 0o600)
            if previous_public.is_file():
                shutil.copyfile(previous_public, PUBLIC_KEY)
                os.chmod(PUBLIC_KEY, 0o644)
            try:
                running = status()
                if running['running']:
                    _stop_official(running['actualPort'])
                if config['enabled']:
                    _start_official(config['port'])
            except Exception as rollback:
                raise RuntimeError('%s；旧主机密钥恢复失败：%s' % (error, rollback))
            raise RuntimeError(str(error) + '；已恢复原主机密钥')


def _rootfs_key_tool(program, arguments, timeout=15):
    result = subprocess.run(
        [str(ROOTFS_LOADER), '--library-path', ROOTFS_LIBDIR, str(program)] + list(arguments),
        capture_output=True, text=True, timeout=timeout)
    if result.returncode:
        raise RuntimeError(((result.stdout or '') + (result.stderr or '')).strip()
                           or ('密钥工具退出码 %d' % result.returncode))
    return result


def _client_public_line(dropbear_key):
    result = _rootfs_key_tool(ROOTFS_DROPBEARKEY, ['-y', '-f', str(dropbear_key)])
    for line in result.stdout.splitlines():
        if line.startswith('ecdsa-sha2-nistp256 '):
            return line.strip() + ' root@l410'
    raise RuntimeError('密钥工具没有返回 ECDSA P-256 公钥')


def generate_client_key():
    """Create the login key owned by this installation and authorize it."""
    with _lock:
        SSH_DIR.mkdir(parents=True, exist_ok=True)
        AUTHORIZED_DIR.mkdir(parents=True, exist_ok=True)
        os.chmod(AUTHORIZED_DIR.parent, 0o700)
        os.chmod(AUTHORIZED_DIR, 0o700)
        dropbear_candidate = SSH_DIR / 'l410_client_ecdsa.dropbear.new'
        private_candidate = SSH_DIR / 'l410_client_ecdsa.new'
        public_candidate = SSH_DIR / 'l410_client_ecdsa.pub.new'
        authorized_candidate = AUTHORIZED_DIR / 'authorized_keys.new'
        for path in (dropbear_candidate, private_candidate, public_candidate, authorized_candidate):
            path.unlink(missing_ok=True)
        try:
            _rootfs_key_tool(ROOTFS_DROPBEARKEY,
                             ['-t', 'ecdsa', '-s', '256', '-f', str(dropbear_candidate)])
            _rootfs_key_tool(ROOTFS_DROPBEARCONVERT,
                             ['dropbear', 'openssh', str(dropbear_candidate), str(private_candidate)])
            public = _client_public_line(dropbear_candidate)
            public_candidate.write_text(public + '\n')
            authorized_candidate.write_text(public + '\n')
            os.chmod(private_candidate, 0o600)
            os.chmod(public_candidate, 0o644)
            os.chmod(authorized_candidate, 0o600)
            os.replace(private_candidate, CLIENT_PRIVATE_KEY)
            os.replace(public_candidate, CLIENT_PUBLIC_KEY)
            os.replace(authorized_candidate, AUTHORIZED_KEYS)
        finally:
            for path in (dropbear_candidate, private_candidate, public_candidate,
                         authorized_candidate):
                path.unlink(missing_ok=True)
        return dict(status(), message='客户端 ECDSA P-256 登录密钥已生成；请导出并妥善保存私钥')


def export_client_private_key():
    try:
        private = CLIENT_PRIVATE_KEY.read_text(errors='strict')
    except FileNotFoundError:
        raise RuntimeError('尚未生成客户端登录密钥')
    except OSError as error:
        raise RuntimeError('无法读取客户端私钥：' + str(error))
    return {'filename': 'l410_ssh_ecdsa',
            'privateKeyBase64': base64.b64encode(private.encode()).decode('ascii'),
            'message': '请选择私钥导出位置'}


def self_test():
    """Start a loopback-only candidate without changing service state."""
    with _lock:
        ensure_config()
        port = _unused_port()
        process, output, banner = _candidate(HOST_KEY, port, loopback=True)
        _finish_candidate(process, output, port)
        return {'ok': True, 'port': port, 'banner': banner,
                'message': '候选 Dropbear 启动、SSH banner 与清理均通过'}

