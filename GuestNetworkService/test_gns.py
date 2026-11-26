import json
import pathlib
import socket
import tempfile
import threading
import unittest
from unittest.mock import Mock

import gns


def request(resource='IPAddress', action='Add', **settings):
    return {'Version': 1, 'AdapterMac': '00-15-5D-12-34-56', 'ResourceType': resource, 'RequestType': action, 'Settings': settings}


class ProtocolTests(unittest.TestCase):
    def test_validation(self):
        value = gns.normalize(request(Address='2001:db8::123', OnLinkPrefixLength=64))
        self.assertEqual(value, ('00155d123456', 'IPAddress', 'Add', {'Address': '2001:db8::123', 'Prefix': 64, 'Family': 6}))
        for args in [dict(Address='127.0.0.1', OnLinkPrefixLength=8), dict(Address='192.0.2.1', OnLinkPrefixLength=33), dict(Address='bad', OnLinkPrefixLength=24)]:
            with self.assertRaises(ValueError): gns.normalize(request(**args))
        with self.assertRaises(ValueError): gns.normalize(request('Route', DestinationPrefix='::/0', NextHop='192.0.2.1'))
        with self.assertRaises(ValueError): gns.normalize(request('Command', Command='echo hi'))

    def test_framed_success_and_failure(self):
        for payload, success in [(request(Address='192.0.2.1', OnLinkPrefixLength=24), True), ({}, False)]:
            server, client = socket.socketpair()
            backend = Mock()
            with server, client:
                worker = threading.Thread(target=gns.handle, args=(server, backend))
                worker.start()
                gns.send(client, payload)
                result = gns.receive(client)
                worker.join(2)
                self.assertFalse(worker.is_alive())
                self.assertEqual(result['Success'], success)
                self.assertEqual(backend.apply.call_count, int(success))

    def test_linux_commands(self):
        with tempfile.TemporaryDirectory() as temp:
            adapter = pathlib.Path(temp, 'eth9')
            adapter.mkdir()
            (adapter / 'device').mkdir()
            (adapter / 'address').write_text('00:15:5d:12:34:56')
            execute = Mock(return_value='[]')
            backend = gns.LinuxBackend(execute, temp)
            backend.apply(*gns.normalize(request(Address='192.0.2.10', OnLinkPrefixLength=24)))
            execute.assert_called_with(['ip', '-4', 'address', 'replace', '192.0.2.10/24', 'dev', 'eth9', 'noprefixroute'])
            backend.apply(*gns.normalize(request('Route', DestinationPrefix='::/0', NextHop='2001:db8::1', Metric=10)))
            execute.assert_called_with(['ip', '-6', 'route', 'replace', '::/0', 'via', '2001:db8::1', 'dev', 'eth9', 'metric', '10', 'proto', '242'])
            backend.apply(*gns.normalize(request('DNS', ServerList='192.0.2.53, 2001:db8::53')))
            execute.assert_called_with(['resolvectl', 'dns', 'eth9', '192.0.2.53', '2001:db8::53'])
            execute.return_value = '[{"gateway":"2001:db8::1","metric":10,"protocol":242}]'
            backend.apply(*gns.normalize(request('Route', 'Remove', DestinationPrefix='::/0', NextHop='2001:db8::1', Metric=10)))
            execute.assert_called_with(['ip', '-6', 'route', 'del', '::/0', 'via', '2001:db8::1', 'dev', 'eth9', 'metric', '10', 'proto', '242'])

    def test_windows_commands(self):
        execute = Mock()
        backend = gns.WindowsBackend(execute)
        backend.apply(*gns.normalize(request('DNS', ServerList=['192.0.2.53'])))
        args, kwargs = execute.call_args
        self.assertEqual(args[0][0], 'powershell.exe')
        self.assertEqual(json.loads(kwargs['input'])['Settings']['Servers'], ['192.0.2.53'])
        self.assertTrue(kwargs['check'])

    def test_oversized_frame(self):
        a, b = socket.socketpair()
        with a, b:
            a.sendall((gns.LIMIT + 1).to_bytes(4, 'big'))
            with self.assertRaises(ValueError): gns.receive(b)


if __name__ == '__main__':
    unittest.main()
