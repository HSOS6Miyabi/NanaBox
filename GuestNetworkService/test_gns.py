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
    def test_snapshot_validation_and_route_order(self):
        settings = {
            'Interface': {'Connected': True, 'NlMtu': 1500, 'Mirrored': True},
            'Addresses': [{'Address': '192.0.2.10', 'OnLinkPrefixLength': 24, 'Mirrored': True}],
            'Routes': [
                {'DestinationPrefix': '0.0.0.0/0', 'NextHop': '192.0.2.1', 'Metric': 10},
                {'DestinationPrefix': '192.0.2.0/24', 'NextHop': '0.0.0.0', 'Metric': 10},
            ],
            'DNS': {'ServerList': []}, 'PortRange': {'Start': 49152, 'End': 53247},
        }
        target, resource, action, normalized = gns.normalize(request('Reconcile', 'Update', **settings))
        self.assertEqual(normalized['Routes'][0]['Destination'], '192.0.2.0/24')
        self.assertEqual(normalized['Routes'][1]['Destination'], '0.0.0.0/0')
        execute = Mock()
        execute.return_value.stdout = '{"ListeningPorts":[]}'
        backend = gns.WindowsBackend(execute)
        self.assertEqual(backend.reconcile(target, normalized), {'ListeningPorts': []})
        args, kwargs = backend.execute.call_args
        self.assertIn('ReadAddresses', args[0][-1])
        self.assertEqual(json.loads(kwargs['input'])['Settings'], normalized)
        settings['Routes'].append(settings['Routes'][0])
        with self.assertRaises(ValueError):
            gns.normalize(request('Reconcile', 'Update', **settings))
        settings['Routes'].pop()
        settings['Interface']['Connected'] = False
        with self.assertRaises(ValueError):
            gns.normalize(request('Reconcile', 'Update', **settings))

    def test_listening_ports(self):
        def line(local, remote, state):
            return f'0: {local} {remote} {state} 00000000:00000000\n'
        def proc4(address, port):
            import ipaddress
            raw = ipaddress.ip_address(address).packed
            if gns.sys.byteorder == 'little':
                raw = raw[::-1]
            return raw.hex() + f':{port:04x}'
        with tempfile.TemporaryDirectory() as temp:
            p = pathlib.Path(temp)
            p.joinpath('tcp').write_text('header\n' +
                line(proc4('0.0.0.0', 8080), proc4('0.0.0.0', 0), '0A') +
                line(proc4('127.0.0.1', 8081), proc4('0.0.0.0', 0), '0A') +
                line(proc4('192.0.2.10', 8082), proc4('0.0.0.0', 0), '0A') +
                line(proc4('192.0.2.20', 8083), proc4('0.0.0.0', 0), '0A'))
            p.joinpath('udp').write_text('header\n' +
                line(proc4('0.0.0.0', 5353), proc4('0.0.0.0', 0), '07') +
                line(proc4('0.0.0.0', 1234), proc4('192.0.2.20', 53), '01'))
            self.assertEqual(gns.linux_listening_ports(p, [{'Address': '192.0.2.10'}]), [
                {'Protocol': 'TCP', 'Port': 8080}, {'Protocol': 'TCP', 'Port': 8082},
                {'Protocol': 'UDP', 'Port': 5353}])

    def test_validation(self):
        value = gns.normalize(request(Address='2001:db8::123', OnLinkPrefixLength=64))
        self.assertEqual(value, ('00155d123456', 'IPAddress', 'Add', {'Address': '2001:db8::123', 'Prefix': 64, 'Family': 6}))
        for args in [dict(Address='127.0.0.1', OnLinkPrefixLength=8), dict(Address='192.0.2.1', OnLinkPrefixLength=33), dict(Address='bad', OnLinkPrefixLength=24)]:
            with self.assertRaises(ValueError): gns.normalize(request(**args))
        with self.assertRaises(ValueError): gns.normalize(request('Route', DestinationPrefix='::/0', NextHop='192.0.2.1'))
        with self.assertRaises(ValueError): gns.normalize(request('Command', Command='echo hi'))

    def test_diagnostics_request(self):
        backend = Mock()
        backend.diagnostics.return_value = {'OperatingSystem': 'Linux', 'Addresses': ['192.0.2.1']}
        server, client = socket.socketpair()
        with server, client:
            worker = threading.Thread(target=gns.handle, args=(server, backend))
            worker.start()
            gns.send(client, {'Version': 1, 'ResourceType': 'Diagnostics', 'RequestType': 'Query', 'Settings': {}})
            reply = gns.receive(client)
            worker.join(2)
            self.assertTrue(reply['Success'])
            self.assertEqual(reply['Data'], backend.diagnostics.return_value)
            backend.apply.assert_not_called()

    def test_connectivity_request(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen(1)
            server, client = socket.socketpair()
            with server, client:
                worker = threading.Thread(target=gns.handle, args=(server, Mock()))
                worker.start()
                gns.send(client, {'Version': 1, 'ResourceType': 'Connectivity', 'RequestType': 'Query',
                                  'Settings': {'Address': '127.0.0.1', 'Port': listener.getsockname()[1]}})
                reply = gns.receive(client)
                worker.join(2)
                self.assertTrue(reply['Success'])
                self.assertEqual(reply['Data']['LocalAddress'], '127.0.0.1')
        with self.assertRaises(ValueError):
            gns.normalize({'Version': 1, 'ResourceType': 'Connectivity', 'RequestType': 'Query',
                           'Settings': {'Address': 'example.com', 'Port': 80}})

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

    def test_mirrored_interface(self):
        with tempfile.TemporaryDirectory() as temp:
            adapter = pathlib.Path(temp, 'eth9')
            adapter.mkdir()
            (adapter / 'device').mkdir()
            (adapter / 'address').write_text('00:15:5d:12:34:56')
            execute = Mock(return_value='')
            backend = gns.LinuxBackend(execute, temp)
            backend.apply(*gns.normalize(request('Interface', 'Update', Connected=True, NlMtu=1500, Mirrored=True)))
            self.assertIn(['sysctl', '-w', 'net/ipv6/conf/eth9/accept_ra=0'], [c.args[0] for c in execute.call_args_list])
            self.assertIn(['sysctl', '-w', 'net/ipv6/conf/eth9/dad_transmits=0'], [c.args[0] for c in execute.call_args_list])
            backend.apply(*gns.normalize(request(Address='2001:db8::10', OnLinkPrefixLength=64, Mirrored=True)))
            execute.assert_called_with(['ip', '-6', 'address', 'replace', '2001:db8::10/64', 'dev', 'eth9', 'noprefixroute', 'nodad'])
        with self.assertRaises(ValueError):
            gns.normalize(request('Interface', 'Update', Connected=True, NlMtu=1500, Mirrored='yes'))

    def test_windows_commands(self):
        execute = Mock()
        backend = gns.WindowsBackend(execute)
        backend.apply(*gns.normalize(request('DNS', ServerList=['192.0.2.53'])))
        args, kwargs = execute.call_args
        self.assertEqual(args[0][0], 'powershell.exe')
        self.assertEqual(json.loads(kwargs['input'])['Settings']['Servers'], ['192.0.2.53'])
        self.assertTrue(kwargs['check'])

    def test_port_range_validation(self):
        value = gns.normalize(request('PortRange', 'Update', Start=49152, End=53247))
        self.assertEqual(value[3], {'Start': 49152, 'End': 53247})
        with self.assertRaises(ValueError):
            gns.normalize(request('PortRange', 'Update', Start=49152, End=49160))
        with tempfile.TemporaryDirectory() as temp:
            adapter = pathlib.Path(temp, 'eth9')
            adapter.mkdir()
            (adapter / 'device').mkdir()
            (adapter / 'address').write_text('00:15:5d:12:34:56')
            execute = Mock(return_value='')
            gns.LinuxBackend(execute, temp).apply(*value)
            execute.assert_called_once_with(['sysctl', '-w', 'net.ipv4.ip_local_port_range=49152 53247'])

    def test_oversized_frame(self):
        a, b = socket.socketpair()
        with a, b:
            a.sendall((gns.LIMIT + 1).to_bytes(4, 'big'))
            with self.assertRaises(ValueError): gns.receive(b)


if __name__ == '__main__':
    unittest.main()
