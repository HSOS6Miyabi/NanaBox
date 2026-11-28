import socket
import struct
import threading
import time
import unittest

import gns

QUERY = bytes.fromhex('cafe01000001000000000000') + b'\x07example\x03com\x00\x00\x01\x00\x01'


class DnsTunnelTest(unittest.TestCase):
    def test_validation(self):
        self.assertEqual(gns.dns_question_end(QUERY), len(QUERY))
        failure = gns.dns_failure(QUERY)
        self.assertEqual(failure[:2], QUERY[:2])
        self.assertEqual(failure[3] & 15, 2)
        self.assertEqual(failure[12:], QUERY[12:])
        for invalid in (b'', QUERY[:13], QUERY[:12] + b'\xc0\x0c\x00\x01\x00\x01',
                        QUERY[:2] + b'\x81' + QUERY[3:]):
            with self.assertRaises(ValueError):
                gns.dns_question_end(invalid)

    def test_udp_tcp_and_disconnect(self):
        with gns.DnsTunnel(port=0, timeout=2) as tunnel:
            guest, host = socket.socketpair()
            host.settimeout(4)
            worker = threading.Thread(target=tunnel.host_channel, args=(guest,), daemon=True)
            worker.start()
            calls = []
            errors = []

            def resolver():
                try:
                    while len(calls) < 3:
                        length = struct.unpack('!I', gns.read_exact(host, 4))[0]
                        if not length:
                            continue
                        frame = gns.read_exact(host, length)
                        calls.append(frame)
                        response = frame[1:2] + frame[2:3] + b'\x81\x83' + frame[5:]
                        if len(calls) == 3:
                            response = b''
                        host.sendall(struct.pack('!I', len(response)) + response)
                except Exception as exc:
                    errors.append(exc)

            resolver_thread = threading.Thread(target=resolver, daemon=True)
            resolver_thread.start()
            deadline = time.monotonic() + 2
            while not tunnel.active and time.monotonic() < deadline:
                time.sleep(.01)
            self.assertEqual(tunnel.active, 1)
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as udp:
                udp.settimeout(3)
                udp.sendto(QUERY, ('127.0.0.1', tunnel.port))
                response, _ = udp.recvfrom(65535)
                self.assertEqual(response[:4], b'\xca\xfe\x81\x83')
            with socket.create_connection(('127.0.0.1', tunnel.port), timeout=3) as tcp:
                for expected in (3, 2):
                    tcp.sendall(struct.pack('!H', len(QUERY)) + QUERY)
                    length = struct.unpack('!H', gns.read_exact(tcp, 2))[0]
                    response = gns.read_exact(tcp, length)
                    self.assertEqual(response[:2], QUERY[:2])
                    self.assertEqual(response[3] & 15, expected)
            resolver_thread.join(4)
            self.assertFalse(resolver_thread.is_alive())
            self.assertEqual(errors, [])
            self.assertEqual(calls, [b'\x00' + QUERY, b'\x01' + QUERY, b'\x01' + QUERY])
            host.close()
            worker.join(3)
            self.assertFalse(worker.is_alive())
            self.assertEqual(tunnel.resolve(QUERY, False)[3] & 15, 2)

    def test_configuration(self):
        snapshot = {'DnsTunnel': True, 'DNS': {'Servers': ['192.0.2.1'], 'Suffix': 'example.com'},
                    'Interface': {'Connected': True}}
        with self.assertRaises(RuntimeError):
            gns.prepare_tunnel_snapshot(snapshot, None, False)
        with gns.DnsTunnel(port=0) as tunnel:
            self.assertEqual(gns.prepare_tunnel_snapshot(snapshot, tunnel, True)['DNS']['Servers'], ['127.0.0.1'])
            self.assertEqual(gns.prepare_tunnel_snapshot(snapshot, tunnel, False)['DNS']['Servers'], [])
            self.assertEqual(snapshot['DNS']['Servers'], ['192.0.2.1'])
            snapshot['Interface']['Connected'] = False
            self.assertEqual(gns.prepare_tunnel_snapshot(snapshot, tunnel, True)['DNS']['Servers'], [])


if __name__ == '__main__':
    unittest.main()
