"""Validate real IPv4 loopback policy changes in a disposable network namespace."""
import json
import pathlib
import tempfile

import gns


def route(destination, *args):
    return json.loads(gns.run(['ip', '-4', '-j', 'route', 'get', destination, *args]))[0]


def main():
    gns.run(['ip', 'link', 'set', 'lo', 'up'])
    gns.run(['ip', 'link', 'add', 'nb-gel', 'type', 'veth', 'peer', 'name', 'nb-peer'])
    gns.run(['ip', 'link', 'add', 'nb-mirror', 'type', 'veth', 'peer', 'name', 'nb-other'])
    for dev in ('nb-gel', 'nb-peer', 'nb-mirror', 'nb-other'):
        gns.run(['ip', 'link', 'set', dev, 'up'])
    gns.run(['ip', 'address', 'add', '192.0.2.10/24', 'dev', 'nb-mirror'])
    original = gns.run(['sysctl', '-n', 'net/ipv4/conf/all/rp_filter']).strip()
    with tempfile.TemporaryDirectory() as temp:
        state = pathlib.Path(temp, 'state.json')
        router = gns.LoopbackRouting(gns.run, state)
        assert route('127.0.0.1', 'ipproto', 'tcp')['dev'] == 'lo'
        try:
            report = router.reconcile('nb-gel', {'nb-mirror': ['192.0.2.10']}, [53])
            assert report['HostAddressRouteCount'] == 1
            assert route('127.0.0.1', 'ipproto', 'tcp')['dev'] == 'nb-gel'
            assert route('127.0.0.1', 'ipproto', 'udp')['dev'] == 'nb-gel'
            assert route('127.0.0.1', 'ipproto', 'icmp')['dev'] == 'lo'
            assert route('127.0.0.1', 'ipproto', 'udp', 'dport', '53')['dev'] == 'lo'
            assert route('192.0.2.10', 'ipproto', 'tcp')['dev'] == 'nb-mirror'
            # Reconciliation after restart must retain exactly the same rules.
            fresh = gns.LoopbackRouting(gns.run, state)
            before = fresh._rules()
            fresh.reconcile('nb-gel', {'nb-mirror': ['192.0.2.10']}, [53])
            assert fresh._rules() == before
            fresh.reconcile('nb-gel', {'nb-mirror': []}, [53])
            assert route('192.0.2.10', 'ipproto', 'tcp')['dev'] == 'lo'
        finally:
            gns.LoopbackRouting(gns.run, state).clear()
        assert route('127.0.0.1', 'ipproto', 'tcp')['dev'] == 'lo'
        assert gns.run(['sysctl', '-n', 'net/ipv4/conf/all/rp_filter']).strip() == original
        assert not state.exists()
        assert not router._routes()
        assert not any(row.get('protocol') == gns.LOOPBACK_PROTOCOL for row in router._rules())
        print('Linux GELNIC loopback route reconciliation and cleanup passed')


if __name__ == '__main__':
    main()
