import errno
import os
import pathlib
import platform
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest

import gns


@unittest.skipUnless(platform.system() == 'Linux', 'Linux bind coordinator')
class BindTests(unittest.TestCase):
    def test_inet_diag_bound_inactive(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as tcp, \
                socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
            tcp.bind(('127.0.0.1', 0))
            udp.bind(('127.0.0.1', 0))
            keys = gns.bound_port_keys()
            self.assertIn((6, tcp.getsockname()[1]), keys)
            self.assertIn((17, udp.getsockname()[1]), keys)

    def test_renew_and_expire(self):
        now = [100]
        live = set()
        with tempfile.TemporaryDirectory() as temp:
            coordinator = gns.BindCoordinator(pathlib.Path(temp, 'bind.sock'), inspect_ports=lambda: live, clock=lambda: now[0])
            first = struct.pack('!BBH', 6, 4, 32000) + socket.inet_aton('127.0.0.1') + bytes(12)
            second = struct.pack('!BBH', 17, 4, 32001) + bytes(16)
            local, host = socket.socketpair()
            frames = []
            def worker():
                with host:
                    while True:
                        try:
                            request = gns.read_exact(host, 20)
                            frames.append(request)
                            host.sendall(b'\x00')
                        except (EOFError, OSError):
                            return
            thread = threading.Thread(target=worker)
            thread.start()
            coordinator.attach(local)
            try:
                self.assertEqual(coordinator.reserve(first), 0)
                self.assertEqual(coordinator.reserve(second), 0)
                now[0] += 20
                live.add((6, 32000))
                coordinator.renew()
                self.assertEqual(frames, [first, second, first, bytes(20)])
                self.assertEqual(list(coordinator.pending), [first])
                live.clear()
                coordinator.renew()
                self.assertFalse(coordinator.pending)
            finally:
                coordinator.close()
                thread.join(5)
            self.assertFalse(thread.is_alive())

    def test_native_bind_conflict_and_success(self):
        if not shutil.which('cc'):
            self.fail('C compiler required for bind interposer integration test')
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            library = root / 'bind.so'
            subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-shared', '-fPIC', str(pathlib.Path(__file__).with_name('bind_coord.c')),
                            '-o', str(library)], check=True)
            # Obtain unused local ports; the simulated HCN host rejects one
            # even though both are free in the guest kernel.
            with socket.socket() as a, socket.socket() as b:
                a.bind(('127.0.0.1', 0))
                b.bind(('127.0.0.1', 0))
                denied, allowed = a.getsockname()[1], b.getsockname()[1]
            coordinator = gns.BindCoordinator(root / 'bind.sock')
            local, host = socket.socketpair()
            frames = []
            def worker():
                with host:
                    while True:
                        try:
                            req = gns.read_exact(host, 20)
                            frames.append(req)
                            port = struct.unpack_from('!H', req, 2)[0]
                            host.sendall(bytes([1 if port == denied else 0]))
                        except (EOFError, OSError):
                            return
            thread = threading.Thread(target=worker)
            thread.start()
            coordinator.attach(local)
            coordinator.start(listen_host=False)
            env = dict(os.environ, LD_PRELOAD=str(library), NANABOX_BIND_SOCKET=str(coordinator.path))
            try:
                code = (
                    'import errno,socket,sys\n'
                    's=socket.socket(); p=int(sys.argv[1]); q=int(sys.argv[2])\n'
                    'try:\n s.bind(("127.0.0.1",p))\n'
                    'except OSError as e:\n assert e.errno==errno.EADDRINUSE,e\n'
                    'else:\n raise AssertionError("Host conflict was ignored")\n'
                    's.bind(("127.0.0.1",q)); assert s.getsockname()[1]==q\n'
                    's.listen(1); s.close()\n'
                    'with socket.socket(socket.AF_UNIX) as u:\n'
                    ' u.bind("\\0nanabox-bind-test")\n'
                )
                subprocess.run([sys.executable, '-c', code, str(denied), str(allowed)], env=env, check=True, timeout=15)
                self.assertEqual([gns.bind_request_key(r) for r in frames if any(r)], [(6, denied), (6, allowed)])
                coordinator.close()
                code = ('import errno,socket,sys\n'
                        'try:\n socket.socket().bind(("127.0.0.1",int(sys.argv[1])))\n'
                        'except OSError as e:\n assert e.errno==errno.ENETDOWN,e\n'
                        'else:\n raise AssertionError("Missing broker was ignored")\n')
                subprocess.run([sys.executable, '-c', code, str(allowed)], env=env, check=True, timeout=15)
            finally:
                coordinator.close()
                thread.join(5)
            self.assertFalse(thread.is_alive())


if __name__ == '__main__':
    unittest.main()
