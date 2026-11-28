import base64
import hashlib
import pathlib
import tempfile
import unittest
from unittest.mock import Mock

import gns


class GuestToolsTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = pathlib.Path(self.directory.name)
        self.now = 0
        self.exchange = gns.FileExchange(self.root, monotonic=lambda: self.now)
        self.addCleanup(self.exchange.close)
        self.tools = gns.GuestTools(self.exchange)

    def request(self, action, **settings):
        _, resource, verb, normalized = gns.normalize({
            'Version': 1, 'ResourceType': 'FileExchange', 'RequestType': action, 'Settings': settings
        })
        return self.tools.request(resource, verb, normalized)

    def upload(self, name, data, overwrite=False):
        token = self.request('BeginUpload', Name=name, Size=len(data), Overwrite=overwrite)['Token']
        for offset in range(0, len(data), gns.FILE_CHUNK):
            chunk = data[offset:offset + gns.FILE_CHUNK]
            reply = self.request('Write', Token=token, Offset=offset, Data=base64.b64encode(chunk).decode())
            self.assertEqual(reply['Offset'], offset + len(chunk))
        return self.request('Commit', Token=token, Sha256=hashlib.sha256(data).hexdigest())

    def test_roundtrip_and_empty_file(self):
        for name, data in [('测试.txt', b''), ('payload.bin', bytes(range(256)) * 1025)]:
            reply = self.upload(name, data)
            self.assertEqual(reply['Sha256'], hashlib.sha256(data).hexdigest())
            self.assertEqual((self.root / name).read_bytes(), data)
            opened = self.request('OpenDownload', Name=name)
            token = opened['Token']
            received = bytearray()
            while len(received) < opened['Size']:
                chunk = self.request('Read', Token=token, Offset=len(received), Length=gns.FILE_CHUNK)
                received += base64.b64decode(chunk['Data'], validate=True)
                self.assertEqual(chunk['Offset'], len(received))
            reply = self.request('FinishDownload', Token=token)
            self.assertEqual(bytes(received), data)
            self.assertEqual(reply['Sha256'], hashlib.sha256(data).hexdigest())
        self.assertFalse(self.exchange.sessions)

    def test_integrity_overwrite_and_expiry(self):
        token = self.request('BeginUpload', Name='data', Size=3, Overwrite=False)['Token']
        self.assertFalse((self.root / 'data').exists())
        with self.assertRaises(ValueError):
            self.request('Write', Token=token, Offset=1, Data='YWJj')
        self.request('Write', Token=token, Offset=0, Data='YWJj')
        with self.assertRaisesRegex(ValueError, 'SHA-256'):
            self.request('Commit', Token=token, Sha256='0' * 64)
        self.assertFalse(list(self.root.iterdir()))
        self.upload('data', b'old')
        with self.assertRaises(FileExistsError):
            self.upload('data', b'new')
        self.upload('data', b'new', True)
        self.assertEqual((self.root / 'data').read_bytes(), b'new')
        token = self.request('BeginUpload', Name='unfinished', Size=2, Overwrite=False)['Token']
        self.now += gns.SESSION_TTL + 1
        with self.assertRaisesRegex(ValueError, 'expired'):
            self.request('Write', Token=token, Offset=0, Data='YQ==')
        self.assertFalse((self.root / 'unfinished').exists())
        self.assertFalse(list(self.root.glob('.nanabox-upload-*')))

    def test_validation_and_disabled(self):
        for name in ('..', '../escape', 'a/b', 'a\\b', 'C:bad', '.hidden', 'CON.txt', 'trail.', 'bad\0name'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                self.request('BeginUpload', Name=name, Size=1, Overwrite=False)
        with self.assertRaises(PermissionError):
            gns.GuestTools().request('FileExchange', 'OpenDownload', {'Name': 'x'})
        with self.assertRaises(ValueError):
            self.request('BeginUpload', Name='x', Size=True, Overwrite=False)

    def test_file_mutation_and_abort(self):
        self.upload('data', b'hello')
        token = self.request('OpenDownload', Name='data')['Token']
        self.request('Read', Token=token, Offset=0, Length=5)
        try:
            (self.root / 'data').write_bytes(b'longer')
        except PermissionError:
            self.request('Abort', Token=token)
            return
        with self.assertRaises(ValueError):
            self.request('FinishDownload', Token=token)
        self.assertFalse(self.exchange.sessions)

    def test_clock_query_and_opt_in(self):
        setter = Mock()
        tools = gns.GuestTools(clock=lambda: 1800000000000, set_clock=setter)
        reply = tools.request('TimeSync', 'Query', {})
        self.assertEqual(reply['UnixMilliseconds'], 1800000000000)
        with self.assertRaises(PermissionError):
            tools.request('TimeSync', 'Update', {'UnixMilliseconds': 1800000000000})
        setter.assert_not_called()
        tools = gns.GuestTools(allow_clock_set=True, clock=lambda: 1800000000000, set_clock=setter)
        resource = {'Version': 1, 'ResourceType': 'TimeSync', 'RequestType': 'Update',
                    'Settings': {'UnixMilliseconds': 1800000000000}}
        _, kind, action, settings = gns.normalize(resource)
        self.assertTrue(tools.request(kind, action, settings)['ClockSetEnabled'])
        setter.assert_called_once_with(1800000000000)
        resource['Settings']['UnixMilliseconds'] = -1
        with self.assertRaises(ValueError):
            gns.normalize(resource)


if __name__ == '__main__':
    unittest.main()
