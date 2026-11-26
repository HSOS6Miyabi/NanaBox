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
    dns = []

    def execute(args):
        if args[0] == 'resolvectl':
            if len(args) == 3:
                return 'Link 1 (nb-test0): ' + ' '.join(dns)
            dns[:] = [v for v in args[3:] if v]
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
                    'Interface': {'Connected': True, 'NlMtu': 1500, 'Mirrored': True},
                    'Addresses': [{'Address': '192.0.2.10', 'OnLinkPrefixLength': 24, 'Mirrored': True}],
                    'Routes': [
                        {'DestinationPrefix': '0.0.0.0/0', 'NextHop': '192.0.2.1', 'Metric': 10},
                        {'DestinationPrefix': '192.0.2.0/24', 'NextHop': '0.0.0.0', 'Metric': 10},
                    ],
                    'DNS': {'ServerList': ['192.0.2.53']},
                    'PortRange': {'Start': 49152, 'End': 53247},
                }
            }
            mac, _, _, snapshot = gns.normalize(request)
            backend.reconcile(mac, snapshot)
            routes = json.loads(gns.run(['ip', '-4', '-j', 'route', 'show', 'dev', 'nb-test0', 'proto', '242']))
            assert {r['dst'] for r in routes} == {'default', '192.0.2.0/24'}, routes
            # Simulate drift and a restarted service; no in-memory state is reused.
            gns.run(['ip', '-4', 'route', 'del', 'default', 'via', '192.0.2.1', 'dev', 'nb-test0', 'metric', '10', 'proto', '242'])
            gns.run(['ip', '-4', 'address', 'del', '192.0.2.10/24', 'dev', 'nb-test0'])
            gns.run(['ip', '-4', 'address', 'add', '198.51.100.10/24', 'dev', 'nb-test0', 'noprefixroute'])
            backend = gns.LinuxBackend(execute, directory)
            backend.reconcile(mac, snapshot)
            backend.reconcile(mac, snapshot)
            assert dns == ['192.0.2.53']
            # Disconnect and verify cleanup, then reconnect from a fresh instance.
            request['Settings']['Interface']['Connected'] = False
            request['Settings']['Addresses'] = []
            request['Settings']['Routes'] = []
            request['Settings']['DNS'] = {'ServerList': []}
            mac, _, _, snapshot = gns.normalize(request)
            backend.reconcile(mac, snapshot)
            assert not backend._addresses('nb-test0')
            assert not backend._routes('nb-test0', managed_only=True)
            assert 'UP' not in backend._link('nb-test0')['flags']
            print('Linux mirrored network reconciliation: passed')
    finally:
        gns.run(['ip', 'link', 'del', 'nb-test0'])


if __name__ == '__main__':
    main()
