"""Run inside an isolated Linux network namespace with iproute2 and CAP_NET_ADMIN."""
import json
import pathlib
import subprocess
import tempfile

import gns


def main():
    # The veth pair and dynamic port range exist only in this network namespace.
    gns.run(['ip', 'link', 'add', 'nb-test0', 'type', 'veth', 'peer', 'name', 'nb-test1'])
    gns.run(['ip', 'link', 'set', 'nb-test1', 'up'])
    gns.run(['ip', 'link', 'set', 'nb-test0', 'address', '00:15:5d:12:34:56'])
    dns, domains = [], []

    def execute(args):
        if args[0] == 'resolvectl':
            values = dns if args[1] == 'dns' else domains
            if len(args) == 3:
                return 'Link 1 (nb-test0): ' + ' '.join(values)
            values[:] = [v for v in args[3:] if v]
            return ''
        return gns.run(args)

    try:
        with tempfile.TemporaryDirectory() as directory:
            adapter = pathlib.Path(directory, 'nb-test0')
            adapter.mkdir()
            (adapter / 'device').mkdir()
            (adapter / 'address').write_text('00:15:5d:12:34:56')
            backend = gns.LinuxBackend(execute, directory)
            request = {
                'Version': 1, 'AdapterMac': '00-15-5D-12-34-56', 'RequestType': 'Update', 'ResourceType': 'Reconcile',
                'Settings': {
                    'Interface': {'Connected': True, 'NlMtu': 1500, 'Mirrored': True, 'IPv4Metric': 0, 'IPv6Metric': 0},
                    'Addresses': [{'Address': '192.0.2.10', 'OnLinkPrefixLength': 24, 'Mirrored': True},
                                  {'Address': '2001:db8::10', 'OnLinkPrefixLength': 64, 'Mirrored': True}],
                    'Routes': [
                        {'DestinationPrefix': '0.0.0.0/0', 'NextHop': '192.0.2.1', 'Metric': 10},
                        {'DestinationPrefix': '192.0.2.0/24', 'NextHop': '0.0.0.0', 'Metric': 10},
                        {'DestinationPrefix': '::/0', 'NextHop': '2001:db8::1', 'Metric': 0},
                        {'DestinationPrefix': '2001:db8::/64', 'NextHop': '::', 'Metric': 0},
                    ],
                    'DNS': {'ServerList': ['192.0.2.53'], 'Suffix': 'Example.Test.'},
                    'PortRange': {'Start': 49152, 'End': 53247},
                }
            }
            mac, _, _, snapshot = gns.normalize(request)
            backend.reconcile(mac, snapshot)
            routes = json.loads(gns.run(['ip', '-4', '-j', 'route', 'show', 'dev', 'nb-test0', 'proto', '242']))
            assert {r['dst'] for r in routes} == {'default', '192.0.2.0/24'}, routes
            routes6 = json.loads(gns.run(['ip', '-6', '-j', 'route', 'show', 'dev', 'nb-test0', 'proto', '242']))
            assert {r['dst'] for r in routes6} == {'default', '2001:db8::/64'}, routes6
            assert all(r['metric'] == 1 for r in routes6), routes6
            # A second interface's routes must survive reconciliation and cleanup.
            gns.run(['ip', '-4', 'address', 'add', '198.18.0.10/24', 'dev', 'nb-test1', 'noprefixroute'])
            gns.run(['ip', '-4', 'route', 'add', '198.18.0.0/24', 'dev', 'nb-test1', 'proto', '242'])
            gns.run(['ip', '-4', 'route', 'add', 'default', 'via', '198.18.0.1', 'dev', 'nb-test1', 'metric', '10', 'proto', '242'])
            # Simulate drift and a restarted service; no in-memory state is reused.
            gns.run(['ip', '-4', 'route', 'del', 'default', 'via', '192.0.2.1', 'dev', 'nb-test0', 'metric', '10', 'proto', '242'])
            gns.run(['ip', '-4', 'address', 'del', '192.0.2.10/24', 'dev', 'nb-test0'])
            gns.run(['ip', '-4', 'address', 'add', '198.51.100.10/24', 'dev', 'nb-test0', 'noprefixroute'])
            backend = gns.LinuxBackend(execute, directory)
            backend.reconcile(mac, snapshot)
            backend.reconcile(mac, snapshot)
            assert dns == ['192.0.2.53']
            assert domains == ['example.test']
            assert gns.run(['sysctl', '-n', 'net/ipv4/conf/nb-test0/rp_filter']).strip() == '0'
            assert gns.run(['sysctl', '-n', 'net/ipv4/conf/nb-test0/arp_filter']).strip() == '1'
            # Disconnect and verify cleanup, then reconnect from a fresh instance.
            request['Settings']['Interface']['Connected'] = False
            request['Settings']['Addresses'] = []
            request['Settings']['Routes'] = []
            request['Settings']['DNS'] = {'ServerList': [], 'Suffix': ''}
            mac, _, _, snapshot = gns.normalize(request)
            backend.reconcile(mac, snapshot)
            assert not dns and not domains
            assert not backend._addresses('nb-test0')
            assert not backend._routes('nb-test0', managed_only=True)
            assert 'UP' not in backend._link('nb-test0')['flags']
            other_routes = gns.run(['ip', '-4', 'route', 'show', 'dev', 'nb-test1', 'proto', '242'])
            assert '198.18.0.0/24' in other_routes
            assert 'default via 198.18.0.1' in other_routes
            print('Linux mirrored network reconciliation: passed')
    finally:
        gns.run(['ip', 'link', 'del', 'nb-test0'])


if __name__ == '__main__':
    main()
