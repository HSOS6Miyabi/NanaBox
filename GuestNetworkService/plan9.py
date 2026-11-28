#!/usr/bin/env python3
"""Mount HCS Plan 9 shares through Hyper-V sockets in the guest mount namespace."""
import argparse
import contextlib
import ctypes
import json
import os
import pathlib
import re
import socket
import stat
import struct
import sys
import tempfile

PORT = 27456
LIMIT = 256 * 1024
STATE_DIR = pathlib.Path('/run/nanabox-plan9')
RESERVED_PORTS = {27453, 27454, 27455, PORT}
MS_RDONLY, MS_NOSUID, MS_NODEV = 1, 2, 4


def name(value):
    if (not isinstance(value, str) or not value or value in ('.', '..') or
            value != value.strip() or len(value.encode('utf-8')) > 200 or
            any(c in value for c in '/\\,:\x00') or any(ord(c) < 32 or ord(c) == 127 for c in value)):
        raise ValueError('Invalid Plan 9 share name')
    return value


def share(value):
    if not isinstance(value, dict) or set(value) != {'Name', 'Port', 'ReadOnly', 'AutoMount'}:
        raise ValueError('Invalid share descriptor')
    name(value['Name'])
    port = value['Port']
    if type(port) is not int or not 1 <= port < 0xffffffff or port in RESERVED_PORTS:
        raise ValueError('Invalid or reserved Plan 9 port')
    for key in ('ReadOnly', 'AutoMount'):
        if type(value[key]) is not bool:
            raise ValueError('Invalid share flag')
    return dict(value)


def manifest(request):
    if (not isinstance(request, dict) or type(request.get('Version')) is not int or
            request['Version'] != 1 or request.get('ResourceType') != 'Plan9Shares'):
        raise ValueError('Invalid Plan 9 request')
    action = request.get('RequestType')
    settings = request.get('Settings')
    if action == 'Query' and settings == {}:
        return None
    if action != 'Update' or not isinstance(settings, dict) or set(settings) != {'Shares'}:
        raise ValueError('Invalid Plan 9 operation')
    items = settings['Shares']
    if not isinstance(items, list) or len(items) > 128:
        raise ValueError('Invalid share list')
    result = {}
    seen = set()
    for item in items:
        item = share(item)
        folded = item['Name'].casefold()
        if folded in seen:
            raise ValueError('Duplicate share name')
        seen.add(folded)
        result[item['Name']] = item
    return result


def read_exact(sock, count):
    data = bytearray()
    while len(data) < count:
        part = sock.recv(count - len(data))
        if not part:
            raise EOFError('Incomplete Plan 9 message')
        data.extend(part)
    return bytes(data)


def receive(sock):
    size = struct.unpack('!I', read_exact(sock, 4))[0]
    if not 0 < size <= LIMIT:
        raise ValueError('Invalid message size')
    return json.loads(read_exact(sock, size))


def send(sock, data):
    data = json.dumps(data, ensure_ascii=False, separators=(',', ':')).encode()
    if not 0 < len(data) <= LIMIT:
        raise ValueError('Invalid response size')
    sock.sendall(struct.pack('!I', len(data)) + data)


def source(item):
    return 'nanabox-9p:' + str(item['Port']) + ':' + item['Name']


def unescape_mountinfo(value):
    return re.sub(r'\\([0-7]{3})', lambda match: chr(int(match[1], 8)), value)


def mountinfo():
    result = {}
    with open('/proc/self/mountinfo', encoding='utf-8', errors='surrogateescape') as file:
        for line in file:
            left, right = line.rstrip('\n').split(' - ', 1)
            a, b = left.split(), right.split()
            target = unescape_mountinfo(a[4])
            result.setdefault(target, []).append({
                'Id': int(a[0]), 'Type': b[0], 'Source': unescape_mountinfo(b[1]),
                'ReadOnly': 'ro' in a[5].split(','),
            })
    return result


def secure_directory(path):
    path = pathlib.Path(path)
    if not path.is_absolute() or path == pathlib.Path('/') or '..' in path.parts:
        raise ValueError('Choose an absolute mount directory other than /')
    # Check every parent. Root-controlled directories prevent a local process
    # from replacing a mount point with a symlink during the mount operation.
    for parent in reversed(path.parents):
        info = parent.lstat()
        if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
            raise ValueError('Mount directory parents must be root-owned directories without group/world write access')
    created = False
    try:
        path.mkdir(mode=0o755)
        created = True
    except FileExistsError:
        pass
    info = path.lstat()
    if not stat.S_ISDIR(info.st_mode) or info.st_uid != 0 or info.st_mode & 0o022:
        raise ValueError('Unsafe mount directory')
    if any(path.iterdir()):
        raise ValueError('Mount directory is not empty')
    return created


def native_mount(item, target):
    # Use the established fd transport for compatibility with Linux kernels
    # predating the native 9p vsock transport. The kernel retains the socket
    # file reference when mount(2) succeeds; no proxy remains in the data path.
    libc = ctypes.CDLL(None, use_errno=True)
    libc.mount.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p,
                           ctypes.c_ulong, ctypes.c_char_p]
    libc.mount.restype = ctypes.c_int
    with socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM) as conn:
        conn.settimeout(5)
        conn.connect((socket.VMADDR_CID_HOST, item['Port']))
        conn.setblocking(True)
        for option in (socket.SO_RCVBUF, socket.SO_SNDBUF):
            conn.setsockopt(socket.SOL_SOCKET, option, 65536)
        fd = conn.fileno()
        options = f'trans=fd,rfdno={fd},wfdno={fd},msize=65536,cache=none,aname={item["Name"]}'
        flags = MS_NOSUID | MS_NODEV
        if item['ReadOnly']:
            flags |= MS_RDONLY
        if libc.mount(os.fsencode(source(item)), os.fsencode(target), b'9p', flags, options.encode()):
            error = ctypes.get_errno()
            raise OSError(error, os.strerror(error), str(target))


def native_unmount(target):
    libc = ctypes.CDLL(None, use_errno=True)
    libc.umount2.argtypes = [ctypes.c_char_p, ctypes.c_int]
    libc.umount2.restype = ctypes.c_int
    if libc.umount2(os.fsencode(target), 0):
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error), str(target))


class Manager:
    def __init__(self, root=STATE_DIR, media='/media', mounts=mountinfo,
                 mount=native_mount, unmount=native_unmount, prepare=secure_directory):
        self.root = pathlib.Path(root)
        self.media = pathlib.Path(media)
        self.mounts = mounts
        self.mount = mount
        self.unmount = unmount
        self.prepare = prepare
        self.statefile = self.root / 'state.json'

    @contextlib.contextmanager
    def locked(self):
        import fcntl
        self.root.mkdir(mode=0o700, parents=True, exist_ok=True)
        if self.root.is_symlink() or not self.root.is_dir():
            raise RuntimeError('Unsafe Plan 9 state directory')
        fd = os.open(self.root / 'lock', os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, 'rb+') as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            yield

    def load(self):
        if self.statefile.is_symlink():
            raise RuntimeError('Unsafe Plan 9 state file')
        if not self.statefile.exists():
            return {'Shares': {}, 'Mounted': {}, 'Suppressed': [], 'Errors': {}}
        return json.loads(self.statefile.read_text(encoding='utf-8'))

    def save(self, state):
        fd, filename = tempfile.mkstemp(dir=self.root, prefix='.state-')
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as file:
                json.dump(state, file, ensure_ascii=False)
                file.flush()
                os.fsync(file.fileno())
            os.replace(filename, self.statefile)
        finally:
            if os.path.exists(filename):
                os.unlink(filename)

    def current(self, record):
        rows = self.mounts().get(record['Target'], [])
        if not rows:
            return None
        if len(rows) != 1:
            raise RuntimeError('Multiple mounts occupy the mount directory')
        row = rows[0]
        if row['Type'] != '9p' or row['Source'] != source(record['Share']) or (
                record.get('Id') is not None and row['Id'] != record['Id']):
            raise RuntimeError('Mount directory is occupied by another filesystem')
        if row['ReadOnly'] != record['Share']['ReadOnly']:
            raise RuntimeError('Mounted filesystem has unexpected access mode')
        return row

    def drop(self, state, key):
        record = state['Mounted'][key]
        current = self.current(record)
        if current:
            self.unmount(record['Target'])
            if self.mounts().get(record['Target']):
                raise RuntimeError('Mount directory is still occupied')
        if record['Created']:
            try:
                pathlib.Path(record['Target']).rmdir()
            except (FileNotFoundError, OSError):
                pass
        del state['Mounted'][key]
        self.save(state)

    def attach(self, state, item, target, manual=False):
        key = item['Name']
        target = str(pathlib.Path(target))
        if key in state['Mounted']:
            record = state['Mounted'][key]
            if record['Share'] != item or record['Target'] != target:
                raise RuntimeError('Unmount the existing share before changing its mount directory or options')
            if self.current(record):
                return
            self.drop(state, key)
        if self.mounts().get(target):
            raise RuntimeError('Mount directory is already occupied')
        created = self.prepare(target)
        # Persist intent before mount(2). A restart between the mount and
        # updating its ID can safely recover the uniquely identified mount.
        record = {'Share': dict(item), 'Target': target, 'Created': created,
                  'Manual': manual, 'Id': None}
        state['Mounted'][key] = record
        self.save(state)
        try:
            self.mount(item, target)
            current = self.current(record)
            if not current:
                raise RuntimeError('9P mount did not appear in mountinfo')
            record['Id'] = current['Id']
            self.save(state)
        except BaseException:
            # Never remove or unmount a filesystem belonging to another tool.
            try:
                if self.current(record) is None:
                    self.drop(state, key)
            except Exception:
                pass
            raise

    def update(self, new_shares):
        with self.locked():
            state = self.load()
            previous = state['Shares']
            state['Shares'] = new_shares
            state['Suppressed'] = [key for key in state['Suppressed']
                                   if key in new_shares and previous.get(key) == new_shares[key]]
            self.save(state)
            return self.status(state)

    def reconcile(self):
        with self.locked():
            state = self.load()
            errors = {}
            for key, record in list(state['Mounted'].items()):
                try:
                    item = state['Shares'].get(key)
                    if item is None or any(item[field] != record['Share'][field]
                                           for field in ('Name', 'Port')) or (item['ReadOnly'] and not record['Share']['ReadOnly']) or (
                            not record['Manual'] and item['ReadOnly'] != record['Share']['ReadOnly']) or (
                            not record['Manual'] and not item['AutoMount']):
                        self.drop(state, key)
                    elif self.current(record) is None:
                        # A manual unmount is respected. A missing automatic
                        # mount is recovered unless explicitly suppressed.
                        if record['Manual']:
                            state['Suppressed'].append(key)
                        self.drop(state, key)
                except Exception as exc:
                    errors[key] = str(exc)[:500]
            for key, item in state['Shares'].items():
                if not item['AutoMount'] or key in state['Suppressed'] or key in state['Mounted'] or key in errors:
                    continue
                try:
                    self.media.mkdir(mode=0o755, exist_ok=True)
                    self.attach(state, item, self.media / key)
                except Exception as exc:
                    errors[key] = str(exc)[:500]
            state['Errors'] = errors
            self.save(state)

    def status(self, state=None):
        if state is None:
            with self.locked():
                return self.status(self.load())
        result = []
        keys = sorted(set(state['Shares']) | set(state['Mounted']))
        for key in keys:
            item = state['Shares'].get(key)
            record = state['Mounted'].get(key)
            entry = dict(item or record['Share'])
            entry['Available'] = item is not None
            entry['MountPoint'] = record['Target'] if record else str(self.media / key)
            entry['Mounted'] = False
            entry['Suppressed'] = key in state['Suppressed']
            if record:
                try:
                    entry['Mounted'] = bool(self.current(record))
                except Exception as exc:
                    entry['Error'] = str(exc)
            if key in state['Errors']:
                entry['Error'] = state['Errors'][key]
            result.append(entry)
        return {'Shares': result}

    def manual_mount(self, key, target=None, port=None, readonly=False):
        key = name(key)
        with self.locked():
            state = self.load()
            item = state['Shares'].get(key)
            if item is None:
                if port is None:
                    raise ValueError('Unknown share; specify its Hyper-V socket port or wait for the host inventory')
                item = share({'Name': key, 'Port': port, 'ReadOnly': readonly, 'AutoMount': False})
                state['Shares'][key] = item
            elif port is not None and port != item['Port']:
                raise ValueError('Port differs from host share configuration')
            item = dict(item)
            if readonly:
                item['ReadOnly'] = True
            if target is None:
                self.media.mkdir(mode=0o755, exist_ok=True)
                target = self.media / key
            self.attach(state, item, target, manual=True)
            state['Suppressed'] = [value for value in state['Suppressed'] if value != key]
            state['Errors'].pop(key, None)
            self.save(state)
            return self.status(state)

    def manual_unmount(self, key):
        key = name(key)
        with self.locked():
            state = self.load()
            if key not in state['Shares'] and key not in state['Mounted']:
                raise ValueError('Unknown share')
            if key in state['Mounted']:
                self.drop(state, key)
            if key not in state['Suppressed']:
                state['Suppressed'].append(key)
            self.save(state)
            return self.status(state)

    def cleanup(self):
        with self.locked():
            state = self.load()
            errors = []
            for key in list(state['Mounted']):
                try:
                    self.drop(state, key)
                except Exception as exc:
                    errors.append(f'{key}: {exc}')
            if errors:
                raise RuntimeError('; '.join(errors))


def handle(conn, manager):
    try:
        desired = manifest(receive(conn))
        result = manager.status() if desired is None else manager.update(desired)
        send(conn, {'Version': 1, 'Success': True, 'Data': result})
    except Exception as exc:
        send(conn, {'Version': 1, 'Success': False, 'Error': str(exc)[:2000]})


def serve(manager):
    with socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM) as listener:
        listener.bind((socket.VMADDR_CID_ANY, PORT))
        listener.listen(4)
        listener.settimeout(2)
        while True:
            try:
                conn, peer = listener.accept()
            except TimeoutError:
                conn = None
            if conn is not None:
                with conn:
                    if peer[0] != socket.VMADDR_CID_HOST:
                        continue
                    conn.settimeout(8)
                    try:
                        handle(conn, manager)
                    except (OSError, EOFError, ValueError) as exc:
                        print(exc, file=sys.stderr)
            try:
                manager.reconcile()
            except Exception as exc:
                print(f'Plan 9 reconciliation: {exc}', file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    commands.add_parser('serve')
    commands.add_parser('list')
    commands.add_parser('cleanup')
    mount = commands.add_parser('mount')
    mount.add_argument('name')
    mount.add_argument('target', nargs='?')
    mount.add_argument('--port', type=int)
    mount.add_argument('--read-only', action='store_true')
    unmount = commands.add_parser('unmount')
    unmount.add_argument('name')
    args = parser.parse_args()
    if sys.platform != 'linux' or os.geteuid() != 0:
        parser.error('Run on a Linux guest as root')
    manager = Manager()
    try:
        if args.command == 'serve':
            serve(manager)
        elif args.command == 'cleanup':
            manager.cleanup()
        else:
            if args.command == 'mount':
                data = manager.manual_mount(args.name, args.target, args.port, args.read_only)
            elif args.command == 'unmount':
                data = manager.manual_unmount(args.name)
            else:
                data = manager.status()
            print(json.dumps(data, ensure_ascii=False, indent=2))
    except (OSError, ValueError, RuntimeError) as exc:
        print(str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
