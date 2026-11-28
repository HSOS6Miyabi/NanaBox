"""NanaBox guest network configuration protocol (Python 3.12+ on Windows)."""
import argparse
import array
import base64
import hashlib
import hmac
import datetime
import os
import time
import ipaddress
import json
import pathlib
import platform
import re
import socket
import socketserver
import queue
import threading
import struct
import secrets
import stat
import tempfile
import subprocess
import sys

PORT = 27453
DNS_PORT = 27454
BIND_PORT = 27455
BIND_SOCKET = '/run/nanabox-gns/bind.sock'
BIND_REQUEST_SIZE = 20
SO_NETNS_COOKIE = getattr(socket, 'SO_NETNS_COOKIE', 71)
DNS_SERVICE = f'{DNS_PORT:08x}-facb-11e6-bd58-64006a7986d3'
SERVICE = f'{PORT:08x}-facb-11e6-bd58-64006a7986d3'
LIMIT = 256 * 1024
STARTED = time.monotonic()
QUERY_CAPABILITIES = ('Heartbeat', 'GuestInformation', 'Diagnostics', 'Connectivity', 'TimeSync')
FILE_CHUNK = 64 * 1024
MAX_FILE_SIZE = (1 << 63) - 1
SESSION_TTL = 900
MAX_SESSIONS = 16
# Reserved routing tables: 0x40000001 through 0xbfffffff, indexed by ifindex.
# Protocol 242 marks only routes and rules installed by NanaBox.
POLICY_TABLE_BASE = 0x40000000
POLICY_PROTOCOL = '242'
POLICY_DEVICE_PRIORITY = 20000
POLICY_SOURCE_PRIORITY = 21000
LOOPBACK_PROTOCOL = '243'
LOOPBACK_TABLE = 0x3ffffffd
HOST_ADDRESS_TABLE = 0x3ffffffe
LOOPBACK_GATEWAY = '169.254.73.249'
LOOPBACK_GATEWAY_MAC = '00:11:22:33:44:55'


def read_exact(sock, length):
    data = bytearray()
    while len(data) < length:
        part = sock.recv(length - len(data))
        if not part:
            raise EOFError('Incomplete message')
        data.extend(part)
    return bytes(data)


def receive(sock):
    size = struct.unpack('!I', read_exact(sock, 4))[0]
    if not 0 < size <= LIMIT:
        raise ValueError('Invalid frame length')
    return json.loads(read_exact(sock, size))


def send(sock, value):
    data = json.dumps(value, separators=(',', ':')).encode('utf-8')
    if not 0 < len(data) <= LIMIT:
        raise ValueError('Invalid frame length')
    sock.sendall(struct.pack('!I', len(data)) + data)


def mac(value):
    if not isinstance(value, str) or not re.fullmatch(r'(?:[0-9A-Fa-f]{2}[:-]?){5}[0-9A-Fa-f]{2}', value):
        raise ValueError('Expected a MAC address')
    return re.sub('[:-]', '', value).lower()


def number(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError('Invalid integer')
    return value


def normalize(request):
    if not isinstance(request, dict) or type(request.get('Version')) is not int or request['Version'] != 1:
        raise ValueError('Unsupported protocol version')
    resource = request.get('ResourceType')
    if resource == 'LoopbackReconcile' and request.get('RequestType') == 'Update':
        settings = request.get('Settings')
        if not isinstance(settings, dict) or type(settings.get('Enabled')) is not bool:
            raise ValueError('Invalid loopback settings')
        if not settings['Enabled']:
            if set(settings) != {'Enabled'}:
                raise ValueError('Unexpected disabled loopback settings')
            return '', resource, 'Update', {'Enabled': False}
        adapter = mac(settings['AdapterMac'])
        interfaces = settings['Interfaces']
        if not isinstance(interfaces, list) or len(interfaces) > 128:
            raise ValueError('Invalid mirrored interface list')
        result = {}
        addresses = set()
        for entry in interfaces:
            target = mac(entry['AdapterMac'])
            if target in result or target == adapter:
                raise ValueError('Duplicate loopback interface')
            items = entry['Addresses']
            if not isinstance(items, list) or len(items) > 256:
                raise ValueError('Invalid loopback address list')
            normalized = []
            for item in items:
                address = ipaddress.IPv4Address(item)
                if address.is_loopback or address.is_multicast or address.is_unspecified or address.is_link_local:
                    raise ValueError('Invalid mirrored host address')
                value = str(address)
                if value in addresses:
                    raise ValueError('Duplicate mirrored host address')
                addresses.add(value)
                normalized.append(value)
            result[target] = sorted(normalized)
        ignored = settings.get('LocalPorts', [])
        if not isinstance(ignored, list) or len(ignored) > 256:
            raise ValueError('Too many local-only ports')
        ignored = sorted({number(port, 1, 65535) for port in ignored})
        discover = settings.get('DiscoverPorts', False)
        if type(discover) is not bool:
            raise ValueError('Invalid loopback discovery setting')
        return '', resource, 'Update', {'Enabled': True, 'AdapterMac': adapter,
                                      'Interfaces': result, 'LocalPorts': ignored, 'DiscoverPorts': discover}
    if resource in ('FileExchange', 'TimeSync'):
        action = request.get('RequestType')
        settings = normalize_tools(resource, action, request.get('Settings'))
        return '', resource, action, settings
    if resource in ('Heartbeat', 'GuestInformation', 'Diagnostics') and request.get('RequestType') == 'Query':
        if request.get('Settings') != {}:
            raise ValueError('Query settings must be an empty object')
        return '', resource, 'Query', {}
    if request.get('ResourceType') == 'Connectivity' and request.get('RequestType') == 'Query':
        s = request['Settings']
        if not isinstance(s, dict):
            raise ValueError('Invalid connectivity request')
        address = ipaddress.ip_address(s['Address'])
        if address.is_unspecified or address.is_multicast:
            raise ValueError('Invalid connectivity target')
        return '', 'Connectivity', 'Query', {'Address': str(address), 'Port': number(s['Port'], 1, 65535)}
    target = mac(request['AdapterMac'])
    action, resource, settings = request['RequestType'], request['ResourceType'], request['Settings']
    if action not in ('Add', 'Update', 'Remove') or not isinstance(settings, dict):
        raise ValueError('Invalid operation')
    if resource == 'Reconcile' and action == 'Update':
        def validated(kind, value, verb='Update'):
            return normalize({'Version': 1, 'AdapterMac': target, 'ResourceType': kind,
                              'RequestType': verb, 'Settings': value})[3]

        interface = validated('Interface', settings['Interface'])
        if not interface.get('Mirrored'):
            raise ValueError('Reconcile requires a dedicated mirrored adapter')
        addresses, routes = settings['Addresses'], settings['Routes']
        if not isinstance(addresses, list) or not isinstance(routes, list) or len(addresses) > 256 or len(routes) > 4096:
            raise ValueError('Invalid snapshot size')
        addresses = [validated('IPAddress', entry) for entry in addresses]
        routes = [validated('Route', entry) for entry in routes]
        if interface['Mtu'] < 1280 and (any(a['Family'] == 6 for a in addresses) or any(r['Family'] == 6 for r in routes)):
            raise ValueError('IPv6 requires an MTU of at least 1280')
        if not interface['Connected'] and (addresses or routes or settings['DNS'].get('ServerList')):
            raise ValueError('Disconnected adapter must have an empty network configuration')
        if len({address_key(a) for a in addresses}) != len(addresses) or len({route_key(r) for r in routes}) != len(routes):
            raise ValueError('Duplicate snapshot entries')
        discover = settings.get('DiscoverPorts', False)
        if type(discover) is not bool:
            raise ValueError('DiscoverPorts must be Boolean')
        dns_tunnel = settings.get('DnsTunnel', False)
        if type(dns_tunnel) is not bool:
            raise ValueError('DnsTunnel must be Boolean')
        return target, resource, action, {
            'DiscoverPorts': discover,
            'DnsTunnel': dns_tunnel,
            'Interface': interface,
            'Addresses': addresses,
            'Routes': sorted(routes, key=route_order),
            'DNS': validated('DNS', settings['DNS'], 'Update' if settings['DNS'].get('ServerList') else 'Remove'),
            'PortRange': validated('PortRange', settings['PortRange']),
        }
    if resource == 'IPAddress':
        address = ipaddress.ip_address(settings['Address'])
        prefix = number(settings['OnLinkPrefixLength'], 0, address.max_prefixlen)
        if address.is_unspecified or address.is_multicast or address.is_loopback:
            raise ValueError('Invalid interface address')
        mirrored = settings.get('Mirrored', False)
        if type(mirrored) is not bool:
            raise ValueError('Mirrored must be Boolean')
        result = {'Address': str(address), 'Prefix': prefix, 'Family': address.version}
        if mirrored:
            result['Mirrored'] = True
        return target, resource, action, result
    if resource == 'Route':
        dest = ipaddress.ip_network(settings['DestinationPrefix'], strict=False)
        hop = ipaddress.ip_address(settings.get('NextHop') or ('0.0.0.0' if dest.version == 4 else '::'))
        if hop.version != dest.version or hop.is_multicast or hop.is_loopback:
            raise ValueError('Invalid next hop')
        return target, resource, action, {'Destination': str(dest), 'NextHop': str(hop), 'Family': dest.version, 'Metric': number(settings.get('Metric', 0), 0, 0xffffffff)}
    if resource == 'Interface' and action != 'Remove':
        if type(settings.get('Connected')) is not bool:
            raise ValueError('Connected must be Boolean')
        mirrored = settings.get('Mirrored', False)
        if type(mirrored) is not bool:
            raise ValueError('Mirrored must be Boolean')
        result = {'Connected': settings['Connected'], 'Mtu': number(settings['NlMtu'], 576, 65535)}
        if mirrored:
            result['Mirrored'] = True
        for family in (4, 6):
            key = f'IPv{family}Metric'
            if key in settings:
                result[key] = number(settings[key], 0, 0xffffffff)
        return target, resource, action, result
    if resource == 'PortRange' and action == 'Update':
        start = number(settings['Start'], 1024, 65535)
        end = number(settings['End'], start, 65535)
        if end - start + 1 < 255:
            raise ValueError('Dynamic port range must contain at least 255 ports')
        return target, resource, action, {'Start': start, 'End': end}
    if resource == 'DNS':
        servers = settings.get('ServerList', [])
        if isinstance(servers, str):
            servers = servers.replace(',', ' ').replace(';', ' ').split()
        if not isinstance(servers, list) or len(servers) > 16:
            raise ValueError('Invalid DNS server list')
        servers = [str(ipaddress.ip_address(value)) for value in servers]
        if action != 'Remove' and not servers:
            raise ValueError('DNS server list must not be empty')
        result = {'Servers': servers}
        if 'Suffix' in settings:
            suffix = settings['Suffix']
            if not isinstance(suffix, str):
                raise ValueError('Invalid DNS suffix')
            suffix = suffix.rstrip('.').lower()
            if suffix:
                suffix = suffix.encode('idna').decode('ascii')
                if len(suffix) > 253 or any(not re.fullmatch(r'[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?', label) for label in suffix.split('.')):
                    raise ValueError('Invalid DNS suffix')
            result['Suffix'] = suffix
        return target, resource, action, result
    raise ValueError('Unsupported resource or operation')


def address_key(entry):
    return entry['Family'], entry['Address'], entry['Prefix']


def route_key(entry):
    return entry['Family'], entry['Destination'], entry['NextHop'], entry['Metric']


def route_order(entry):
    # Direct routes establish gateway reachability; remove them last.
    return (not ipaddress.ip_address(entry['NextHop']).is_unspecified,
            entry['Family'], -ipaddress.ip_network(entry['Destination']).prefixlen,
            entry['Metric'], entry['Destination'], entry['NextHop'])


def decode_proc_address(value, family):
    raw = bytes.fromhex(value)
    if len(raw) != (4 if family == 4 else 16):
        raise ValueError('Invalid proc socket address')
    # Linux /proc/net uses native byte order for each 32-bit word.
    if sys.byteorder == 'little':
        raw = b''.join(raw[i:i+4][::-1] for i in range(0, len(raw), 4))
    address = ipaddress.ip_address(raw)
    return address.ipv4_mapped if family == 6 and address.ipv4_mapped else address


def linux_listening_ports(proc, addresses, include_loopback=False):
    accepted = {ipaddress.ip_address(a['Address']) for a in addresses}
    ports = set()
    for protocol in ('TCP', 'UDP'):
        for family in (4, 6):
            path = pathlib.Path(proc, protocol.lower() + ('6' if family == 6 else ''))
            try:
                lines = path.read_text().splitlines()[1:]
            except FileNotFoundError:
                continue
            for line in lines:
                fields = line.split()
                if len(fields) < 4:
                    continue
                if protocol == 'TCP' and fields[3] != '0A':
                    continue
                if protocol == 'UDP' and int(fields[2].rsplit(':', 1)[1], 16) != 0:
                    continue
                raw, port = fields[1].rsplit(':', 1)
                address = decode_proc_address(raw, family)
                port = int(port, 16)
                if port and ((not address.is_loopback and (address.is_unspecified or address in accepted)) or
                             (include_loopback and address == ipaddress.IPv4Address('127.0.0.1'))):
                    ports.add((protocol, port))
    return [{'Protocol': protocol, 'Port': port} for protocol, port in sorted(ports)]


def run(args):
    return subprocess.run(args, check=True, capture_output=True, text=True, timeout=30).stdout


class LoopbackRouting:
    """Dedicated GELNIC IPv4 policy routing. Only protocol 243 is managed."""
    def __init__(self, execute, state_path):
        self.execute = execute
        self.path = pathlib.Path(state_path)

    def _ip(self, *args):
        return self.execute(['ip', '-N', '-4', *args])

    def _rules(self):
        return json.loads(self._ip('-j', 'rule', 'show'))

    def _routes(self):
        return [row for row in json.loads(self._ip('-j', 'route', 'show', 'table', 'all'))
                if str(row.get('table')) in (str(LOOPBACK_TABLE), str(HOST_ADDRESS_TABLE))]

    @staticmethod
    def _table(row):
        value = str(row.get('table', ''))
        return '255' if value == 'local' else value

    @staticmethod
    def _protocol(row):
        return str(row.get('protocol', ''))

    @staticmethod
    def _key(row):
        proto = str(row.get('ipproto', '')).removeprefix('ipproto-')
        proto = {'6': 'tcp', '17': 'udp'}.get(proto, proto)
        dport = str(row.get('dport', ''))
        if '-' in dport:
            first, last = dport.split('-', 1)
            if first == last:
                dport = first
        destination = row.get('dst', 'all')
        if destination != 'all':
            destination = str(ipaddress.ip_network(destination, strict=False))
        return (int(row['priority']), LoopbackRouting._table(row), row.get('iif', ''),
                proto, destination, dport)

    @staticmethod
    def _default_local(row):
        return (LoopbackRouting._key(row) == (0, '255', '', '', 'all', '') and
                row.get('src', 'all') == 'all' and not row.get('fwmark') and
                LoopbackRouting._protocol(row) != LOOPBACK_PROTOCOL and
                set(row) <= {'priority', 'src', 'dst', 'table', 'protocol', 'flags'})

    def _rule(self, verb, key, owned=True):
        priority, table, interface, proto, destination, port = key
        args = ['rule', verb, 'pref', str(priority)]
        if interface:
            args += ['iif', interface]
        if destination != 'all':
            args += ['to', destination]
        if proto:
            args += ['ipproto', proto]
        if port:
            args += ['dport', port]
        args += ['lookup', table]
        if owned:
            args += ['protocol', LOOPBACK_PROTOCOL]
        self._ip(*args)

    def _route(self, verb, table, destination, dev):
        args = ['route', verb, destination, 'via', LOOPBACK_GATEWAY, 'dev', dev,
                'table', str(table), 'proto', LOOPBACK_PROTOCOL]
        if verb != 'del':
            args += ['onlink']
        self._ip(*args)

    def _state(self):
        if not self.path.exists():
            return {'Sysctls': {}, 'Neighbors': {}}
        if self.path.is_symlink():
            raise RuntimeError('Unsafe loopback state file')
        state = json.loads(self.path.read_text(encoding='utf-8'))
        if not isinstance(state.get('Sysctls'), dict) or not isinstance(state.get('Neighbors'), dict):
            raise RuntimeError('Invalid loopback state file')
        return state

    def _save(self, state):
        self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        fd, name = tempfile.mkstemp(prefix='.loopback-', dir=self.path.parent)
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as file:
                json.dump(state, file)
            os.replace(name, self.path)
        finally:
            if os.path.exists(name):
                os.unlink(name)

    def _sysctl(self, state, dev, name, value):
        key = f'net/ipv4/conf/{dev}/{name}'
        current = self.execute(['sysctl', '-n', key]).strip()
        if key not in state['Sysctls']:
            state['Sysctls'][key] = [current, value]
            self._save(state)
        if current != value:
            self.execute(['sysctl', '-w', f'{key}={value}'])

    def _neighbors(self, dev):
        return json.loads(self._ip('-j', 'neigh', 'show', 'to', LOOPBACK_GATEWAY, 'dev', dev))

    def _neighbor(self, state, dev):
        rows = self._neighbors(dev)
        if rows and (len(rows) != 1 or rows[0].get('lladdr', '').lower() != LOOPBACK_GATEWAY_MAC or
                     'PERMANENT' not in rows[0].get('state', [])):
            raise RuntimeError('Loopback gateway neighbor is managed by another application')
        if not rows:
            state['Neighbors'][dev] = True
            self._save(state)
            self._ip('neigh', 'replace', LOOPBACK_GATEWAY, 'lladdr', LOOPBACK_GATEWAY_MAC,
                     'nud', 'permanent', 'dev', dev)

    def _restore_unused(self, state, devices):
        for dev in list(state['Neighbors']):
            if dev in devices:
                continue
            if any(link['ifname'] == dev for link in json.loads(self._ip('-j', 'link', 'show'))):
                rows = self._neighbors(dev)
                if len(rows) == 1 and rows[0].get('lladdr', '').lower() == LOOPBACK_GATEWAY_MAC and 'PERMANENT' in rows[0].get('state', []):
                    self._ip('neigh', 'del', LOOPBACK_GATEWAY, 'dev', dev)
            del state['Neighbors'][dev]
        for key, (original, managed) in list(state['Sysctls'].items()):
            dev = key.split('/')[3]
            if dev in devices or (dev == 'all' and devices):
                continue
            try:
                current = self.execute(['sysctl', '-n', key]).strip()
            except subprocess.CalledProcessError:
                if dev == 'all' or any(link['ifname'] == dev for link in json.loads(self._ip('-j', 'link', 'show'))):
                    raise
                current = None
            if current == managed and current != original:
                self.execute(['sysctl', '-w', f'{key}={original}'])
            del state['Sysctls'][key]
        self._save(state)

    def clear(self):
        state = self._state()
        rules = self._rules()
        ours = [row for row in rules if self._protocol(row) == LOOPBACK_PROTOCOL]
        # Install the ordinary local rule before removing the fallback rule.
        if ours or self.path.exists():
            if not any(self._default_local(row) for row in rules):
                self._rule('add', (0, '255', '', '', 'all', ''), owned=False)
            for row in ours:
                self._rule('del', self._key(row))
            for row in self._routes():
                if self._protocol(row) == LOOPBACK_PROTOCOL:
                    self._route('del', int(row['table']), row['dst'], row['dev'])
            self._restore_unused(state, set())
            self.path.unlink(missing_ok=True)

    def reconcile(self, gelnic, interfaces, local_ports):
        devices = set(interfaces) | {gelnic}
        if gelnic in interfaces or 'lo' in devices:
            raise ValueError('A dedicated host loopback adapter is required')
        routes = {(LOOPBACK_TABLE, '127.0.0.1/32', gelnic)}
        for dev, addresses in interfaces.items():
            for address in addresses:
                routes.add((HOST_ADDRESS_TABLE, address + '/32', dev))
        wanted = {(2, '255', '', '', 'all', '')}
        for proto in ('tcp', 'udp'):
            for dev in devices:
                wanted.add((0, '255', dev, proto, 'all', ''))
            wanted.add((1, str(LOOPBACK_TABLE), '', proto, 'all', ''))
            if any(interfaces.values()):
                wanted.add((1, str(HOST_ADDRESS_TABLE), '', proto, 'all', ''))
            for port in local_ports:
                wanted.add((0, '255', '', proto, '127.0.0.1/32', str(port)))
        rules = self._rules()
        actual = [self._key(row) for row in rules if self._protocol(row) == LOOPBACK_PROTOCOL]
        default = [row for row in rules if self._default_local(row)]
        foreign = [row for row in rules if int(row['priority']) <= 2 and
                   self._protocol(row) != LOOPBACK_PROTOCOL and not self._default_local(row)]
        if foreign or len(default) > 1:
            raise RuntimeError('Existing high-priority IPv4 rules conflict with mirrored loopback')
        current = self._routes()
        if any(self._protocol(row) != LOOPBACK_PROTOCOL for row in current):
            raise RuntimeError('Loopback routing table contains an unmanaged route')
        actual_routes = {(int(row['table']), str(ipaddress.ip_network(row['dst'], strict=False)), row['dev'])
                         for row in current if row.get('gateway') == LOOPBACK_GATEWAY}
        state = self._state()
        # Keep the original settings across guest service restarts.
        try:
            for dev in sorted(devices):
                self._sysctl(state, dev, 'accept_local', '1')
                self._sysctl(state, dev, 'rp_filter', '0')
                self._sysctl(state, dev, 'route_localnet', '1')
                self._neighbor(state, dev)
            self._sysctl(state, 'all', 'rp_filter', '0')
            if 'UP' not in json.loads(self._ip('-j', 'link', 'show', 'dev', gelnic))[0]['flags']:
                self._ip('link', 'set', 'dev', gelnic, 'up')
            if (actual_routes != routes or len(current) != len(routes) or
                    set(actual) != wanted or len(actual) != len(wanted) or default):
                # Restore ordinary local delivery while updating the managed
                # tables; keep all unrelated routes and rules untouched.
                self.clear()
                state = self._state()
                for dev in sorted(devices):
                    self._sysctl(state, dev, 'accept_local', '1')
                    self._sysctl(state, dev, 'rp_filter', '0')
                    self._sysctl(state, dev, 'route_localnet', '1')
                    self._neighbor(state, dev)
                self._sysctl(state, 'all', 'rp_filter', '0')
                for table, destination, dev in sorted(routes):
                    self._route('replace', table, destination, dev)
                self._rule('add', (2, '255', '', '', 'all', ''))
                # The fallback local rule now protects non-TCP/UDP traffic.
                self._rule('del', (0, '255', '', '', 'all', ''), owned=False)
                for key in sorted(wanted - {(2, '255', '', '', 'all', '')}):
                    self._rule('add', key)
            self._restore_unused(state, devices)
            return {'Loopback': True, 'LoopbackDevice': gelnic, 'HostAddressRouteCount': len(routes) - 1}
        except BaseException:
            self.clear()
            raise


class LinuxBackend:
    def __init__(self, execute=run, sysfs='/sys/class/net', loopback_state=None):
        self.execute = execute
        self.sysfs = pathlib.Path(sysfs)
        self.loopback = LoopbackRouting(execute, loopback_state) if loopback_state else None

    def reconcile_loopback(self, settings):
        if self.loopback is None:
            raise RuntimeError('Install the Linux guest service with loopback routing enabled')
        if not settings['Enabled']:
            self.loopback.clear()
            return {'Loopback': False}
        gelnic = self.adapter(settings['AdapterMac'])
        interfaces = {self.adapter(target): addresses for target, addresses in settings['Interfaces'].items()}
        result = self.loopback.reconcile(gelnic, interfaces, settings['LocalPorts'])
        if settings.get('DiscoverPorts', False):
            result['ListeningPorts'] = [entry for entry in linux_listening_ports('/proc/net', [], include_loopback=True)
                                        if entry['Port'] not in settings['LocalPorts']]
        return result

    def adapter(self, target):
        matches = []
        for path in self.sysfs.iterdir():
            try:
                if (path / 'device').exists() and mac((path / 'address').read_text().strip()) == target:
                    matches.append(path.name)
            except (OSError, ValueError):
                continue
        if len(matches) != 1:
            raise ValueError('Expected exactly one matching hardware network adapter')
        return matches[0]

    def diagnostics(self):
        addresses = json.loads(self.execute(['ip', '-j', 'address', 'show']))
        gpu = []
        for device in pathlib.Path('/sys/class/drm').glob('card*'):
            if not re.fullmatch(r'card\d+', device.name):
                continue
            try:
                gpu.append({'Name': device.name, 'Vendor': (device / 'device/vendor').read_text().strip(),
                            'Device': (device / 'device/device').read_text().strip()})
            except OSError:
                continue
        return {'OperatingSystem': 'Linux', 'Addresses': [a['local'] for row in addresses for a in row.get('addr_info', [])],
                'Adapters': addresses, 'Gpu': gpu, 'Dxg': pathlib.Path('/dev/dxg').exists(),
                'DynamicPortRange': pathlib.Path('/proc/sys/net/ipv4/ip_local_port_range').read_text().split()}

    def _addresses(self, dev):
        rows = json.loads(self.execute(['ip', '-j', 'address', 'show', 'dev', dev]))
        result = {}
        for row in rows:
            for item in row.get('addr_info', []):
                address = ipaddress.ip_address(item['local'])
                # IPv6 link-local addresses are generated by the guest kernel.
                if address.is_loopback or address.is_multicast or address.is_unspecified or (address.version == 6 and address.is_link_local):
                    continue
                entry = {'Address': str(address), 'Family': address.version, 'Prefix': item['prefixlen'], 'Mirrored': True}
                result[address_key(entry)] = entry
        return result

    def _routes(self, dev, managed_only=False):
        result = {}
        for family in (4, 6):
            args = ['ip', f'-{family}', '-j', 'route', 'show', 'table', 'main', 'dev', dev]
            if managed_only:
                args += ['proto', '242']
            for item in json.loads(self.execute(args)):
                if item.get('type', 'unicast') != 'unicast':
                    continue
                zero = '0.0.0.0' if family == 4 else '::'
                dest = item.get('dst', 'default')
                if dest == 'default':
                    dest = zero + '/0'
                for hop in item.get('nexthops') or [item]:
                    if hop.get('dev', item.get('dev', dev)) != dev:
                        continue
                    entry = {'Family': family, 'Destination': str(ipaddress.ip_network(dest, strict=False)),
                             'NextHop': str(ipaddress.ip_address(hop.get('gateway', zero))),
                             'Metric': item.get('metric', 0)}
                    result[route_key(entry)] = entry
        return result

    def _route_change(self, dev, verb, entry, table=None):
        # Append instead of replace: equal-cost routes on another interface
        # must remain intact.
        cmd = ['ip', f"-{entry['Family']}", 'route', 'append' if verb == 'add' else verb, entry['Destination']]
        if not ipaddress.ip_address(entry['NextHop']).is_unspecified:
            cmd += ['via', entry['NextHop']]
        cmd += ['dev', dev, 'metric', str(entry['Metric']), 'proto', POLICY_PROTOCOL]
        if table is not None:
            cmd += ['table', str(table)]
            # A host IPv6 link-local gateway need not have an explicit
            # link-local prefix route in the guest's interface table.
            if verb == 'add' and not ipaddress.ip_address(entry['NextHop']).is_unspecified:
                cmd += ['onlink']
        self.execute(cmd)

    def _policy_table(self, ifindex):
        if not 1 <= ifindex <= 0x7fffffff:
            raise ValueError('Invalid interface index for policy routing')
        return POLICY_TABLE_BASE + ifindex

    def _policy_rules(self, family):
        return json.loads(self.execute(['ip', '-N', f'-{family}', '-j', 'rule', 'show']))

    def _policy_route_rows(self, family):
        return json.loads(self.execute(['ip', '-N', f'-{family}', '-j', 'route', 'show', 'table', 'all']))

    @staticmethod
    def _policy_rule_key(row):
        source = row.get('src', 'all')
        if source != 'all':
            source = str(ipaddress.ip_network(source, strict=False))
        return int(row['priority']), source, row.get('oif', '')

    def _policy_rule_change(self, family, verb, table, key):
        priority, source, dev = key
        command = ['ip', f'-{family}', 'rule', verb, 'pref', str(priority)]
        if source != 'all':
            command += ['from', source]
        if dev:
            command += ['oif', dev]
        self.execute(command + ['lookup', str(table), 'protocol', POLICY_PROTOCOL])

    def _policy_routes(self, dev, family, table):
        result = {}
        zero = '0.0.0.0' if family == 4 else '::'
        for item in self._policy_route_rows(family):
            if str(item.get('table', '254')) != str(table):
                continue
            if str(item.get('protocol')) != POLICY_PROTOCOL or str(item.get('type', '1')) not in ('1', 'unicast'):
                raise RuntimeError(f'Policy routing table {table} contains an unmanaged route')
            dest = item.get('dst', 'default')
            if dest == 'default':
                dest = zero + '/0'
            for hop in item.get('nexthops') or [item]:
                if hop.get('dev', item.get('dev')) != dev:
                    raise RuntimeError(f'Policy routing table {table} belongs to another interface')
                entry = {'Family': family, 'Destination': str(ipaddress.ip_network(dest, strict=False)),
                         'NextHop': str(ipaddress.ip_address(hop.get('gateway', zero))),
                         'Metric': item.get('metric', 0)}
                result[route_key(entry)] = entry
        return result

    def _reconcile_policy(self, dev, link, interface, addresses, routes):
        table = self._policy_table(link['ifindex'])
        connected = interface['Connected']
        for family in (4, 6):
            desired_routes = {k: v for k, v in routes.items() if v['Family'] == family} if connected else {}
            desired_rules = set()
            if connected and desired_routes:
                desired_rules.add((POLICY_DEVICE_PRIORITY, 'all', dev))
                priority = POLICY_SOURCE_PRIORITY + min(interface.get(f'IPv{family}Metric', 0), 9999)
                for entry in addresses.values():
                    if entry['Family'] == family:
                        ip = ipaddress.ip_address(entry['Address'])
                        desired_rules.add((priority, f'{ip}/{ip.max_prefixlen}', ''))
            rows = [row for row in self._policy_rules(family) if str(row.get('table')) == str(table)]
            if any(str(row.get('protocol')) != POLICY_PROTOCOL for row in rows):
                raise RuntimeError(f'Policy routing table {table} has an unmanaged rule')
            current_routes = self._policy_routes(dev, family, table)
            current_rules = set()
            for row in rows:
                key = self._policy_rule_key(row)
                if key not in desired_rules or key in current_rules:
                    self._policy_rule_change(family, 'del', table, key)
                else:
                    current_rules.add(key)
            for entry in sorted((v for k, v in current_routes.items() if k not in desired_routes), key=route_order, reverse=True):
                self._route_change(dev, 'del', entry, table)
            for entry in sorted(desired_routes.values(), key=route_order):
                if route_key(entry) not in current_routes:
                    self._route_change(dev, 'add', entry, table)
            for key in sorted(desired_rules - current_rules):
                self._policy_rule_change(family, 'add', table, key)
            actual_rules = [self._policy_rule_key(row) for row in self._policy_rules(family)
                            if str(row.get('table')) == str(table) and str(row.get('protocol')) == POLICY_PROTOCOL]
            if set(actual_rules) != desired_rules or len(actual_rules) != len(desired_rules):
                raise RuntimeError('Guest policy rules did not converge')
            if self._policy_routes(dev, family, table).keys() != desired_routes.keys():
                raise RuntimeError('Guest policy routing table did not converge')
        return table

    def _reap_policy_rules(self):
        # A removed virtual adapter loses its routes in the kernel. Rules
        # referring to its former interface index may survive hot unplug.
        active = {self._policy_table(link['ifindex'])
                  for link in json.loads(self.execute(['ip', '-j', 'link', 'show']))}
        for family in (4, 6):
            for row in self._policy_rules(family):
                try:
                    table = int(row.get('table', 0))
                except (ValueError, TypeError):
                    continue
                if (POLICY_TABLE_BASE < table <= POLICY_TABLE_BASE + 0x7fffffff and table not in active
                        and str(row.get('protocol')) == POLICY_PROTOCOL):
                    self._policy_rule_change(family, 'del', table, self._policy_rule_key(row))

    def _link(self, dev):
        rows = json.loads(self.execute(['ip', '-j', 'link', 'show', 'dev', dev]))
        if len(rows) != 1:
            raise ValueError('Mirrored link not found')
        return rows[0]

    def _dns(self, dev):
        value = self.execute(['resolvectl', 'dns', dev])
        # Output: Link N (ethX): address address ...
        _, separator, servers = value.partition(':')
        if not separator:
            raise ValueError('Unable to read resolver configuration')
        return [str(ipaddress.ip_address(v.split('%', 1)[0])) for v in servers.split()]

    def _domain(self, dev):
        value = self.execute(['resolvectl', 'domain', dev])
        _, separator, domains = value.partition(':')
        if not separator:
            raise ValueError('Unable to read resolver search domains')
        return domains.split()

    def reconcile(self, target, snapshot):
        dev = self.adapter(target)
        interface = snapshot['Interface']
        addresses = {address_key(a): a for a in snapshot['Addresses']}
        routes = {}
        for entry in snapshot['Routes']:
            route = dict(entry)
            # Linux combines the host's interface and route metrics in one field.
            route['Metric'] = min(0xffffffff, route['Metric'] + interface.get(f"IPv{route['Family']}Metric", 0))
            # IPv6 metric zero is interpreted by Linux as 1024, not highest priority.
            if route['Family'] == 6 and route['Metric'] == 0:
                route['Metric'] = 1
            routes[route_key(route)] = route
        dns = snapshot['DNS']['Servers']
        port_range = snapshot['PortRange']
        link = self._link(dev)
        # Match WSL's per-interface behavior without changing global forwarding
        # or permitting loopback traffic on external interfaces.
        for family, name, wanted in (
                (4, 'accept_local', '1'), (4, 'rp_filter', '0'), (4, 'arp_filter', '1'),
                (6, 'accept_ra', '0'), (6, 'autoconf', '0'), (6, 'dad_transmits', '0'),
                (6, 'use_tempaddr', '0')):
            key = f'net/ipv{family}/conf/{dev}/{name}'
            if self.execute(['sysctl', '-n', key]).strip() != wanted:
                self.execute(['sysctl', '-w', f'{key}={wanted}'])
        if link['mtu'] != interface['Mtu']:
            self.execute(['ip', 'link', 'set', 'dev', dev, 'mtu', str(interface['Mtu'])])
        if interface['Connected'] and 'UP' not in link['flags']:
            self.execute(['ip', 'link', 'set', 'dev', dev, 'up'])

        # Remove stale managed gateway routes, then direct routes and addresses.
        managed = self._routes(dev, managed_only=True)
        for entry in sorted((v for k, v in managed.items() if k not in routes), key=route_order, reverse=True):
            self._route_change(dev, 'del', entry)
        actual = self._addresses(dev)
        for key, entry in actual.items():
            if key not in addresses:
                self.apply(target, 'IPAddress', 'Remove', entry)
        for key, entry in addresses.items():
            if key not in actual:
                self.apply(target, 'IPAddress', 'Add', entry)

        # Never replace a route belonging to a different interface or application.
        actual_routes = self._routes(dev)
        for entry in sorted(routes.values(), key=route_order):
            if route_key(entry) not in actual_routes:
                self._route_change(dev, 'add', entry)
        policy_table = self._reconcile_policy(dev, link, interface, addresses, routes)
        self._reap_policy_rules()
        if self._dns(dev) != dns:
            self.execute(['resolvectl', 'dns', dev, *(dns or [''])])
        suffix = snapshot['DNS'].get('Suffix')
        domains = [suffix] if suffix else []
        if suffix is not None and self._domain(dev) != domains:
            self.execute(['resolvectl', 'domain', dev, *(domains or [''])])
        current_range = self.execute(['sysctl', '-n', 'net.ipv4.ip_local_port_range']).split()
        expected_range = [str(port_range['Start']), str(port_range['End'])]
        if current_range != expected_range:
            self.apply(target, 'PortRange', 'Update', port_range)
        if not interface['Connected'] and 'UP' in self._link(dev)['flags']:
            self.execute(['ip', 'link', 'set', 'dev', dev, 'down'])

        # Success is based on the actual guest state, including after service restart.
        actual = self._addresses(dev)
        actual_routes = self._routes(dev)
        link = self._link(dev)
        if actual.keys() != addresses.keys() or not routes.keys() <= actual_routes.keys():
            raise RuntimeError('Guest addresses or routes did not converge')
        if self._routes(dev, managed_only=True).keys() - routes.keys():
            raise RuntimeError('Stale managed guest routes remain')
        if ('UP' in link['flags']) != interface['Connected'] or link['mtu'] != interface['Mtu']:
            raise RuntimeError('Guest link state did not converge')
        if self._dns(dev) != dns or self.execute(['sysctl', '-n', 'net.ipv4.ip_local_port_range']).split() != expected_range:
            raise RuntimeError('Guest DNS or dynamic port range did not converge')
        if suffix is not None and self._domain(dev) != domains:
            raise RuntimeError('Guest DNS search suffix did not converge')
        return {'ListeningPorts': linux_listening_ports('/proc/net', snapshot['Addresses'])
                if snapshot['DiscoverPorts'] and interface['Connected'] else [],
                'PolicyRoutingTable': policy_table}

    def apply(self, target, resource, action, s):
        dev = self.adapter(target)
        if resource == 'PortRange':
            self.execute(['sysctl', '-w', f"net.ipv4.ip_local_port_range={s['Start']} {s['End']}"])
        elif resource == 'Interface':
            if s.get('Mirrored'):
                for name in ('accept_ra', 'autoconf', 'dad_transmits'):
                    self.execute(['sysctl', '-w', f'net/ipv6/conf/{dev}/{name}=0'])
            self.execute(['ip', 'link', 'set', 'dev', dev, 'mtu', str(s['Mtu'])])
            self.execute(['ip', 'link', 'set', 'dev', dev, 'up' if s['Connected'] else 'down'])
        elif resource == 'IPAddress':
            family = '-4' if s['Family'] == 4 else '-6'
            cidr = f"{s['Address']}/{s['Prefix']}"
            if action == 'Remove':
                rows = json.loads(self.execute(['ip', family, '-j', 'address', 'show', 'dev', dev]))
                if not any(a.get('local') == s['Address'] and a.get('prefixlen') == s['Prefix'] for row in rows for a in row.get('addr_info', [])):
                    return
                self.execute(['ip', family, 'address', 'del', cidr, 'dev', dev])
            else:
                command = ['ip', family, 'address', 'replace', cidr, 'dev', dev, 'noprefixroute']
                if family == '-6' and s.get('Mirrored'):
                    command.append('nodad')
                self.execute(command)
        elif resource == 'Route':
            family = '-4' if s['Family'] == 4 else '-6'
            hop = s['NextHop']
            onlink = ipaddress.ip_address(hop).is_unspecified
            destination = s['Destination']
            if action == 'Remove':
                rows = json.loads(self.execute(['ip', family, '-j', 'route', 'show', 'exact', destination, 'dev', dev]))
                if not any(r.get('gateway', '0.0.0.0' if s['Family'] == 4 else '::') == hop and r.get('metric', 0) == s['Metric'] and str(r.get('protocol')) == '242' for r in rows):
                    return
            cmd = ['ip', family, 'route', 'del' if action == 'Remove' else 'replace', destination]
            if not onlink:
                cmd += ['via', hop]
            self.execute(cmd + ['dev', dev, 'metric', str(s['Metric']), 'proto', '242'])
        elif resource == 'DNS':
            self.execute(['resolvectl', 'revert', dev] if action == 'Remove' else ['resolvectl', 'dns', dev, *s['Servers']])


WINDOWS_SCRIPT = r'''
$ErrorActionPreference = 'Stop'
$r = [Console]::In.ReadToEnd() | ConvertFrom-Json
$m = @(Get-NetAdapter -IncludeHidden | Where-Object { (([string]$_.MacAddress) -replace '[:-]','').ToLowerInvariant() -eq $r.Target -and ($_.HardwareInterface -or $_.PnPDeviceID -like 'VMBUS\*' -or $_.InterfaceDescription -like 'Microsoft Hyper-V Network Adapter*') })
if ($m.Count -ne 1) { throw 'Expected exactly one matching hardware network adapter' }
$i = $m[0].ifIndex
$s = $r.Settings
$remove = $r.Action -eq 'Remove'
switch ($r.Resource) {
  'PortRange' {
    $count = [int]$s.End - [int]$s.Start + 1
    foreach ($af in @('ipv4','ipv6')) {
      foreach ($protocol in @('tcp','udp')) {
        & netsh.exe int $af set dynamicport $protocol ("start=" + $s.Start) ("num=" + $count)
        if ($LASTEXITCODE -ne 0) { throw "Could not set $af $protocol dynamic port range" }
      }
    }
  }
  'IPAddress' {
    $af = if ($s.Family -eq 4) { 'IPv4' } else { 'IPv6' }
    $old = @(Get-NetIPAddress -InterfaceIndex $i -AddressFamily $af -PolicyStore ActiveStore -ErrorAction SilentlyContinue | Where-Object IPAddress -eq $s.Address)
    if ($remove) {
      $old | Where-Object PrefixLength -eq $s.Prefix | Remove-NetIPAddress -Confirm:$false
    } elseif ($old.Count) {
      $old | Where-Object PrefixLength -ne $s.Prefix | Set-NetIPAddress -PrefixLength $s.Prefix
    } else {
      New-NetIPAddress -InterfaceIndex $i -IPAddress $s.Address -PrefixLength $s.Prefix -PolicyStore ActiveStore | Out-Null
    }
  }
  'Route' {
    $old = @(Get-NetRoute -InterfaceIndex $i -DestinationPrefix $s.Destination -PolicyStore ActiveStore -ErrorAction SilentlyContinue | Where-Object NextHop -eq $s.NextHop)
    if ($remove) { $old | Where-Object RouteMetric -eq $s.Metric | Remove-NetRoute -Confirm:$false }
    elseif ($old.Count) { $old | Set-NetRoute -RouteMetric $s.Metric }
    else { New-NetRoute -InterfaceIndex $i -DestinationPrefix $s.Destination -NextHop $s.NextHop -RouteMetric $s.Metric -PolicyStore ActiveStore | Out-Null }
  }
  'Interface' {
    if ($s.Mirrored) {
      Set-NetIPInterface -InterfaceIndex $i -AddressFamily IPv4 -Dhcp Disabled -DadTransmits 0 -PolicyStore ActiveStore
      Set-NetIPInterface -InterfaceIndex $i -AddressFamily IPv6 -RouterDiscovery Disabled -DadTransmits 0 -PolicyStore ActiveStore
    }
    Set-NetIPInterface -InterfaceIndex $i -NlMtuBytes $s.Mtu -PolicyStore ActiveStore
    if ($s.Connected) { $m[0] | Enable-NetAdapter -Confirm:$false }
    else { $m[0] | Disable-NetAdapter -Confirm:$false }
  }
  'DNS' {
    if ($remove) { Set-DnsClientServerAddress -InterfaceIndex $i -ResetServerAddresses }
    else { Set-DnsClientServerAddress -InterfaceIndex $i -ServerAddresses $s.Servers }
  }
  default { throw 'Unsupported resource' }
}
'''


WINDOWS_DIAGNOSTICS = r'''
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$adapters = @(Get-NetAdapter -IncludeHidden | Select-Object Name, MacAddress, ifIndex, Status, InterfaceDescription)
$addresses = @(Get-NetIPAddress | Select-Object -ExpandProperty IPAddress)
$gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name, PNPDeviceID, DriverVersion, Status)
$ports = @(foreach ($af in @('ipv4','ipv6')) { foreach ($p in @('tcp','udp')) { (& netsh.exe int $af show dynamicport $p) -join "`n" } })
[Console]::WriteLine((ConvertTo-Json -InputObject @{OperatingSystem='Windows'; Addresses=$addresses; Adapters=$adapters; Gpu=$gpu; Dxg=$false; DynamicPortRange=$ports} -Depth 6 -Compress))
'''


WINDOWS_RECONCILE = r'''
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$r = [Console]::In.ReadToEnd() | ConvertFrom-Json
$s = $r.Settings
$m = @(Get-NetAdapter -IncludeHidden | Where-Object {
    (([string]$_.MacAddress) -replace '[:-]','').ToLowerInvariant() -eq $r.Target -and
    ($_.HardwareInterface -or $_.PnPDeviceID -like 'VMBUS\*' -or $_.InterfaceDescription -like 'Microsoft Hyper-V Network Adapter*')
})
if ($m.Count -ne 1) { throw 'Expected exactly one matching Hyper-V network adapter' }
$i = $m[0].ifIndex
if ($s.Interface.Connected -and [string]$m[0].Status -eq 'Disabled') {
    $m[0] | Enable-NetAdapter -Confirm:$false
}
function ReadInterfaces {
    @(Get-NetIPInterface -PolicyStore ActiveStore | Where-Object InterfaceIndex -eq $i)
}
function InterfaceMtu($iface) {
    # IPv6 requires a minimum IP MTU of 1280. IPv4-only mirrored
    # snapshots may use a smaller MTU without enabling IPv6 addressing.
    if ([string]$iface.AddressFamily -eq 'IPv6') {
        return [uint32][Math]::Max(1280, [int]$s.Interface.Mtu)
    }
    return [uint32]$s.Interface.Mtu
}
$interfaces = @(ReadInterfaces)
if ($s.Interface.Connected -and -not $interfaces.Count) { throw 'Guest IP interface not available' }
foreach ($iface in $interfaces) {
    $params = @{InterfaceIndex=$i; AddressFamily=$iface.AddressFamily; PolicyStore='ActiveStore'}
    $mtu = InterfaceMtu $iface
    if ($iface.NlMtu -ne $mtu -and $iface.NlMtuBytes -ne $mtu) { $params.NlMtuBytes = $mtu }
    if ($iface.DadTransmits -ne 0) { $params.DadTransmits = 0 }
    if ([string]$iface.AddressFamily -eq 'IPv4' -and [string]$iface.Dhcp -ne 'Disabled') { $params.Dhcp = 'Disabled' }
    if ([string]$iface.RouterDiscovery -ne 'Disabled') { $params.RouterDiscovery = 'Disabled' }
    $metric = if ([string]$iface.AddressFamily -eq 'IPv4') { $s.Interface.IPv4Metric } else { $s.Interface.IPv6Metric }
    if ($null -ne $metric -and ($iface.InterfaceMetric -ne $metric -or [string]$iface.AutomaticMetric -ne 'Disabled')) {
        $params.InterfaceMetric = [uint32]$metric
        $params.AutomaticMetric = 'Disabled'
    }
    if ($params.Count -gt 3) { Set-NetIPInterface @params }
}
function AddressKey($address,$prefix) { return ([ipaddress]$address).ToString() + '/' + $prefix }
function RouteKey($destination,$hop,$metric) { return $destination + '|' + ([ipaddress]$hop).ToString() + '|' + $metric }
function IsDirect($hop) { return $hop -in @('0.0.0.0','::') }
function ReadAddresses {
    @(Get-NetIPAddress -PolicyStore ActiveStore | Where-Object {
        if ($_.InterfaceIndex -eq $i) {
            $ip = [ipaddress]$_.IPAddress
            -not [ipaddress]::IsLoopback($ip) -and -not $ip.IsIPv6LinkLocal -and -not $ip.IsIPv6Multicast
        }
    })
}
function ReadRoutes {
    @(Get-NetRoute -PolicyStore ActiveStore | Where-Object InterfaceIndex -eq $i)
}
$desiredAddresses = @{}
foreach ($a in $s.Addresses) { $desiredAddresses[(AddressKey $a.Address $a.Prefix)] = $a }
$desiredRoutes = @{}
foreach ($route in $s.Routes) { $desiredRoutes[(RouteKey $route.Destination $route.NextHop $route.Metric)] = $route }
# Only routes installed by a management API or automatic IP configuration on
# this dedicated adapter may be removed; Windows-owned local routes remain.
$stale = @(ReadRoutes | Where-Object {
    [string]$_.Protocol -in @('NetMgmt','Dhcp','RouterAdvertisement') -and
    -not $desiredRoutes.ContainsKey((RouteKey $_.DestinationPrefix $_.NextHop $_.RouteMetric))
})
foreach ($route in ($stale | Sort-Object @{Expression={ IsDirect $_.NextHop }})) {
    $route | Remove-NetRoute -Confirm:$false
}
foreach ($a in (ReadAddresses)) {
    if (-not $desiredAddresses.ContainsKey((AddressKey $a.IPAddress $a.PrefixLength))) {
        $a | Remove-NetIPAddress -Confirm:$false
    }
}
$actual = @(ReadAddresses)
foreach ($a in $s.Addresses) {
    if (-not @($actual | Where-Object { (AddressKey $_.IPAddress $_.PrefixLength) -eq (AddressKey $a.Address $a.Prefix) }).Count) {
        New-NetIPAddress -InterfaceIndex $i -IPAddress $a.Address -PrefixLength $a.Prefix -PolicyStore ActiveStore | Out-Null
    }
}
# Python has already ordered direct routes before those using a gateway.
$actualRoutes = @(ReadRoutes)
foreach ($route in $s.Routes) {
    $same = @($actualRoutes | Where-Object {
        $_.DestinationPrefix -eq $route.Destination -and ([ipaddress]$_.NextHop).Equals([ipaddress]$route.NextHop)
    })
    if ($same.Count) {
        # Windows generates on-link routes for assigned addresses itself.
        if (-not (IsDirect $route.NextHop) -and -not @($same | Where-Object RouteMetric -eq $route.Metric).Count) {
            $same | Set-NetRoute -RouteMetric $route.Metric
        }
    } else {
        New-NetRoute -InterfaceIndex $i -DestinationPrefix $route.Destination -NextHop $route.NextHop -RouteMetric $route.Metric -Protocol NetMgmt -PolicyStore ActiveStore | Out-Null
    }
}
$dns = @((Get-DnsClientServerAddress -InterfaceIndex $i).ServerAddresses | Where-Object { $_ })
$wantedDns = @($s.DNS.Servers | Where-Object { $_ })
if ((@($dns | Sort-Object) -join ',') -ne (@($wantedDns | Sort-Object) -join ',')) {
    if ($wantedDns.Count) { Set-DnsClientServerAddress -InterfaceIndex $i -ServerAddresses $wantedDns }
    else { Set-DnsClientServerAddress -InterfaceIndex $i -ResetServerAddresses }
}
if ($null -ne $s.DNS.PSObject.Properties['Suffix']) {
    $client = Get-DnsClient -InterfaceIndex $i
    if ([string]$client.ConnectionSpecificSuffix -ne [string]$s.DNS.Suffix) {
        Set-DnsClient -InterfaceIndex $i -ConnectionSpecificSuffix ([string]$s.DNS.Suffix)
    }
    if ([string](Get-DnsClient -InterfaceIndex $i).ConnectionSpecificSuffix -ne [string]$s.DNS.Suffix) {
        throw 'Guest DNS search suffix did not converge'
    }
}
$count = [int]$s.PortRange.End - [int]$s.PortRange.Start + 1
foreach ($af in @('ipv4','ipv6')) {
    foreach ($protocol in @('tcp','udp')) {
        $output = (& netsh.exe int $af show dynamicport $protocol | Out-String)
        if ($LASTEXITCODE -ne 0) { throw 'Could not read dynamic port range' }
        $values = @([regex]::Matches($output, '(?m):\s*(\d+)\s*$') | ForEach-Object { [int]$_.Groups[1].Value })
        if ($values.Count -ne 2 -or $values[0] -ne $s.PortRange.Start -or $values[1] -ne $count) {
            & netsh.exe int $af set dynamicport $protocol ("start=" + $s.PortRange.Start) ("num=" + $count) | Out-Null
            if ($LASTEXITCODE -ne 0) { throw 'Could not set dynamic port range' }
        }
    }
}
if (-not $s.Interface.Connected -and [string]$m[0].Status -ne 'Disabled') { $m[0] | Disable-NetAdapter -Confirm:$false }
$actualAddresses = @(ReadAddresses | ForEach-Object { AddressKey $_.IPAddress $_.PrefixLength })
if (@($actualAddresses | Where-Object { -not $desiredAddresses.ContainsKey($_) }).Count -or
    @($desiredAddresses.Keys | Where-Object { $_ -notin $actualAddresses }).Count) { throw 'Guest addresses did not converge' }
$actualRoutes = @(ReadRoutes)
foreach ($route in $s.Routes) {
    if (-not @($actualRoutes | Where-Object { $_.DestinationPrefix -eq $route.Destination -and
        ([ipaddress]$_.NextHop).Equals([ipaddress]$route.NextHop) -and ((IsDirect $route.NextHop) -or $_.RouteMetric -eq $route.Metric) }).Count) {
        throw ('Guest route did not converge: ' + $route.Destination)
    }
}
$dns = @((Get-DnsClientServerAddress -InterfaceIndex $i).ServerAddresses | Where-Object { $_ })
if ((@($dns | Sort-Object) -join ',') -ne (@($wantedDns | Sort-Object) -join ',')) { throw 'Guest DNS did not converge' }
$actualAdapter = Get-NetAdapter -InterfaceIndex $i -IncludeHidden
if (([string]$actualAdapter.Status -ne 'Disabled') -ne [bool]$s.Interface.Connected) { throw 'Guest interface state did not converge' }
$interfaces = @(ReadInterfaces)
if ($s.Interface.Connected -and -not $interfaces.Count) { throw 'Guest IP interface disappeared' }
foreach ($iface in $interfaces) {
    $mtu = InterfaceMtu $iface
    if ($iface.NlMtu -ne $mtu -and $iface.NlMtuBytes -ne $mtu) { throw 'Guest MTU did not converge' }
}
$ports = @()
if ($s.DiscoverPorts -and $s.Interface.Connected) {
    $addresses = @($s.Addresses | ForEach-Object { ([ipaddress]$_.Address).ToString() })
    $sockets = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | ForEach-Object {
        [pscustomobject]@{Protocol='TCP';Port=[int]$_.LocalPort;Address=$_.LocalAddress}
    }) + @(Get-NetUDPEndpoint -ErrorAction SilentlyContinue | ForEach-Object {
        [pscustomobject]@{Protocol='UDP';Port=[int]$_.LocalPort;Address=$_.LocalAddress}
    })
    $ports = @($sockets | Where-Object {
        $ip = [ipaddress]$_.Address
        if ($ip.IsIPv4MappedToIPv6) { $ip = $ip.MapToIPv4() }
        $_.Port -gt 0 -and -not [ipaddress]::IsLoopback($ip) -and
        ($ip.ToString() -in @('0.0.0.0','::') -or $ip.ToString() -in $addresses)
    } | Select-Object Protocol,Port -Unique)
}
[Console]::WriteLine((ConvertTo-Json -InputObject @{ListeningPorts=$ports} -Depth 4 -Compress))
'''


class WindowsBackend:
    def __init__(self, execute=subprocess.run):
        self.execute = execute

    def diagnostics(self):
        result = self.execute(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', WINDOWS_DIAGNOSTICS],
                              encoding='utf-8', capture_output=True, check=True, timeout=30)
        return json.loads(result.stdout)

    def reconcile(self, target, snapshot):
        result = self.execute(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', WINDOWS_RECONCILE],
                              input=json.dumps({'Target': target, 'Settings': snapshot}), encoding='utf-8',
                              capture_output=True, check=True, timeout=60)
        return json.loads(result.stdout)

    def apply(self, target, resource, action, settings):
        self.execute(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', WINDOWS_SCRIPT],
                     input=json.dumps({'Target': target, 'Resource': resource, 'Action': action, 'Settings': settings}),
                     text=True, capture_output=True, check=True, timeout=30)


def exchange_name(name):
    # The exchange directory contains regular files only. Apply the same
    # portable filename rules on both guest operating systems.
    if not isinstance(name, str) or not 1 <= len(name.encode('utf-8')) <= 240:
        raise ValueError('Invalid filename')
    if name.startswith('.') or name.endswith((' ', '.')) or any(
            ord(c) < 32 or c in '/\\:*?"<>|' for c in name):
        raise ValueError('Invalid filename')
    stem = name.split('.')[0].upper()
    if stem in ('CON', 'PRN', 'AUX', 'NUL') or re.fullmatch(r'(?:COM|LPT)[1-9¹²³]', stem):
        raise ValueError('Reserved filename')
    return name


def normalize_tools(resource, action, settings):
    if not isinstance(settings, dict):
        raise ValueError('Invalid settings')
    if resource == 'TimeSync':
        if action == 'Query' and not settings:
            return {}
        if action == 'Update' and set(settings) == {'UnixMilliseconds'}:
            return {'UnixMilliseconds': number(settings['UnixMilliseconds'], 946684800000, 4102444800000)}
        raise ValueError('Invalid clock request')
    fields = {
        'BeginUpload': {'Name', 'Size', 'Overwrite'},
        'OpenDownload': {'Name'},
        'Write': {'Token', 'Offset', 'Data'},
        'Read': {'Token', 'Offset', 'Length'},
        'Commit': {'Token', 'Sha256'},
        'FinishDownload': {'Token'},
        'Abort': {'Token'},
    }
    if not isinstance(action, str) or action not in fields or set(settings) != fields[action]:
        raise ValueError('Invalid file exchange request')
    result = dict(settings)
    if 'Name' in result:
        result['Name'] = exchange_name(result['Name'])
    if 'Size' in result:
        result['Size'] = number(result['Size'], 0, MAX_FILE_SIZE)
        if type(result['Overwrite']) is not bool:
            raise ValueError('Overwrite must be Boolean')
    if 'Token' in result:
        token = result['Token']
        if not isinstance(token, str) or not re.fullmatch(r'[0-9a-f]{32}', token):
            raise ValueError('Invalid transfer token')
    if 'Offset' in result:
        result['Offset'] = number(result['Offset'], 0, MAX_FILE_SIZE)
    if 'Length' in result:
        result['Length'] = number(result['Length'], 1, FILE_CHUNK)
    if 'Data' in result:
        encoded = result['Data']
        if not isinstance(encoded, str) or len(encoded) > ((FILE_CHUNK + 2) // 3) * 4:
            raise ValueError('Invalid transfer chunk')
        result['Data'] = base64.b64decode(encoded, validate=True)
        if not 0 < len(result['Data']) <= FILE_CHUNK:
            raise ValueError('Invalid transfer chunk')
    if 'Sha256' in result and (not isinstance(result['Sha256'], str)
                               or not re.fullmatch(r'[0-9a-f]{64}', result['Sha256'])):
        raise ValueError('Invalid SHA-256 digest')
    return result


def regular_file(info):
    return stat.S_ISREG(info.st_mode) and not (
        getattr(info, 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0))


def publish_file(temporary, target, overwrite):
    if overwrite:
        # Never follow an existing symbolic link or reparse point.
        try:
            if not regular_file(target.lstat()):
                raise ValueError('Destination is not a regular file')
        except FileNotFoundError:
            pass
        os.replace(temporary, target)
    elif os.name == 'nt':
        os.rename(temporary, target)  # fails if the destination exists
    else:
        os.link(temporary, target)  # atomic no-replace publication
        temporary.unlink()


class FileExchange:
    """File transactions scoped to a directory administered by the guest."""
    def __init__(self, root, monotonic=time.monotonic):
        self.root = pathlib.Path(root)
        self.root.mkdir(mode=0o700, parents=True, exist_ok=True)
        info = self.root.lstat()
        if not stat.S_ISDIR(info.st_mode) or self.root.is_symlink() or (
                getattr(info, 'st_file_attributes', 0) & getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0)):
            raise ValueError('Exchange root must be a real directory')
        if os.name != 'nt' and (info.st_uid != os.geteuid() or info.st_mode & 0o022):
            raise PermissionError('Exchange root must be owned by the service and not writable by other users')
        self.root = self.root.resolve(strict=True)
        self.monotonic = monotonic
        self.sessions = {}
        # A service restart invalidates previous session tokens.
        for path in self.root.glob('.nanabox-upload-*'):
            if re.fullmatch(r'\.nanabox-upload-[0-9a-f]{32}', path.name):
                path.unlink()

    def abort(self, token):
        session = self.sessions.pop(token, None)
        if session:
            try:
                session['File'].close()
            finally:
                if session.get('Temporary') is not None:
                    session['Temporary'].unlink(missing_ok=True)

    def close(self):
        for token in list(self.sessions):
            self.abort(token)

    def reap(self):
        now = self.monotonic()
        for token, session in list(self.sessions.items()):
            if now - session['Touched'] > SESSION_TTL:
                self.abort(token)

    def request(self, action, settings):
        self.reap()
        if action in ('BeginUpload', 'OpenDownload'):
            if len(self.sessions) >= MAX_SESSIONS:
                raise RuntimeError('Too many active transfers')
            name = exchange_name(settings['Name'])
            target = self.root / name
            token = secrets.token_hex(16)
            session = {'Name': name, 'Target': target, 'Offset': 0, 'Hash': hashlib.sha256(),
                       'Touched': self.monotonic()}
            if action == 'BeginUpload':
                try:
                    existing = target.lstat()
                except FileNotFoundError:
                    existing = None
                if existing is not None and (not settings['Overwrite'] or not regular_file(existing)):
                    raise FileExistsError('Destination already exists or is not a regular file')
                temporary = self.root / ('.nanabox-upload-' + token)
                fd = os.open(temporary, os.O_CREAT | os.O_EXCL | os.O_WRONLY |
                             getattr(os, 'O_BINARY', 0), 0o600)
                session.update(File=os.fdopen(fd, 'wb'), Temporary=temporary,
                               Size=settings['Size'], Overwrite=settings['Overwrite'], Upload=True)
            else:
                info = target.lstat()
                if not regular_file(info):
                    raise ValueError('Source is not a regular file')
                fd = os.open(target, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0) |
                             getattr(os, 'O_BINARY', 0))
                stream = os.fdopen(fd, 'rb')
                current = os.fstat(stream.fileno())
                if not regular_file(current) or not os.path.samestat(info, current):
                    stream.close()
                    raise ValueError('Source changed during open')
                session.update(File=stream, Size=current.st_size, Info=current, Upload=False)
            self.sessions[token] = session
            return {'Token': token, 'Name': name, 'Size': session['Size'], 'ChunkSize': FILE_CHUNK}

        token = settings['Token']
        if action == 'Abort':
            self.abort(token)
            return {'Aborted': True}
        session = self.sessions.get(token)
        if session is None:
            raise ValueError('Unknown or expired transfer token')
        session['Touched'] = self.monotonic()
        stream = session['File']
        offset = session['Offset']
        if action in ('Read', 'Write') and settings['Offset'] != offset:
            raise ValueError('Unexpected transfer offset')
        if action == 'Write' and session['Upload']:
            chunk = settings['Data']
            if offset + len(chunk) > session['Size']:
                raise ValueError('Chunk exceeds declared file size')
            try:
                if stream.write(chunk) != len(chunk):
                    raise OSError('Short file write')
            except Exception:
                self.abort(token)
                raise
            session['Hash'].update(chunk)
            session['Offset'] += len(chunk)
            return {'Offset': session['Offset']}
        if action == 'Read' and not session['Upload']:
            chunk = stream.read(min(settings['Length'], session['Size'] - offset))
            if not chunk and offset < session['Size']:
                self.abort(token)
                raise OSError('Source file was truncated')
            session['Hash'].update(chunk)
            session['Offset'] += len(chunk)
            return {'Offset': session['Offset'], 'Data': base64.b64encode(chunk).decode('ascii'),
                    'End': session['Offset'] == session['Size']}
        if action in ('Commit', 'FinishDownload'):
            upload = session['Upload']
            if (action == 'Commit') != upload:
                raise ValueError('Incorrect transfer direction')
            try:
                if offset != session['Size']:
                    raise ValueError('Incomplete transfer')
                digest = session['Hash'].hexdigest()
                if upload:
                    if not hmac.compare_digest(settings['Sha256'], digest):
                        raise ValueError('SHA-256 mismatch')
                    stream.flush()
                    os.fsync(stream.fileno())
                    stream.close()
                    publish_file(session['Temporary'], session['Target'], session['Overwrite'])
                else:
                    current = os.fstat(stream.fileno())
                    if current.st_size != session['Size'] or current.st_mtime_ns != session['Info'].st_mtime_ns:
                        raise ValueError('Source file changed during download')
                return {'Name': session['Name'], 'Size': offset, 'Sha256': digest}
            finally:
                self.abort(token)
        raise ValueError('Incorrect transfer operation')


def clock_milliseconds():
    return time.time_ns() // 1000000


def set_guest_clock(milliseconds):
    if platform.system() == 'Windows':
        import ctypes
        from ctypes import wintypes

        class SystemTime(ctypes.Structure):
            _fields_ = [(name, wintypes.WORD) for name in (
                'Year', 'Month', 'DayOfWeek', 'Day', 'Hour', 'Minute', 'Second', 'Milliseconds')]

        seconds, remainder = divmod(milliseconds, 1000)
        dt = datetime.datetime.fromtimestamp(seconds, datetime.timezone.utc)
        value = SystemTime(dt.year, dt.month, (dt.weekday() + 1) % 7, dt.day,
                           dt.hour, dt.minute, dt.second, remainder)
        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel32.SetSystemTime.argtypes = [ctypes.POINTER(SystemTime)]
        kernel32.SetSystemTime.restype = wintypes.BOOL
        if not kernel32.SetSystemTime(ctypes.byref(value)):
            raise ctypes.WinError(ctypes.get_last_error())
    else:
        time.clock_settime_ns(time.CLOCK_REALTIME, milliseconds * 1000000)


class GuestTools:
    def __init__(self, exchange=None, allow_clock_set=False,
                 clock=clock_milliseconds, set_clock=set_guest_clock):
        self.exchange = exchange
        self.allow_clock_set = allow_clock_set
        self.clock = clock
        self.set_clock = set_clock

    def capabilities(self):
        return {'FileExchange': self.exchange is not None, 'ClockSet': self.allow_clock_set,
                'FileChunkSize': FILE_CHUNK}

    def request(self, resource, action, settings):
        if resource == 'FileExchange':
            if self.exchange is None:
                raise PermissionError('File exchange is disabled in the guest service')
            return self.exchange.request(action, settings)
        if action == 'Update':
            if not self.allow_clock_set:
                raise PermissionError('Clock synchronization is disabled in the guest service')
            self.set_clock(settings['UnixMilliseconds'])
        return {'UnixMilliseconds': self.clock(), 'ClockSetEnabled': self.allow_clock_set}


def guest_information(tools=None):
    return {
        'HostName': socket.gethostname(),
        'OperatingSystem': platform.system(),
        'Release': platform.release(),
        'Version': platform.version(),
        'Architecture': platform.machine(),
        'LogicalProcessorCount': os.cpu_count(),
        'UtcTime': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'ServiceUptimeSeconds': max(0, int(time.monotonic() - STARTED)),
        'ProtocolVersion': 1,
        'QueryCapabilities': list(QUERY_CAPABILITIES),
        'Capabilities': (tools or GuestTools()).capabilities(),
    }


def dns_question_end(packet):
    """Accept a standard DNS query with exactly one uncompressed question."""
    if not 17 <= len(packet) <= 65535:
        raise ValueError('Invalid DNS message size')
    if packet[2] & 0xf8 or packet[4:6] != b'\x00\x01' or packet[6:10] != b'\x00' * 4:
        raise ValueError('Invalid DNS query header')
    offset = 12
    while offset < len(packet) and offset < 267:
        length = packet[offset]
        offset += 1
        if not length:
            if offset + 4 <= len(packet):
                return offset + 4
            break
        if length > 63 or offset + length >= len(packet):
            break
        offset += length
    raise ValueError('Invalid DNS question')


def dns_failure(packet):
    end = dns_question_end(packet)
    # Preserve transaction ID, recursion desired and the complete question.
    return (packet[:2] + bytes((0x80 | (packet[2] & 1), 0x82))
            + b'\x00\x01' + b'\x00' * 6 + packet[12:end])


class DnsJob:
    def __init__(self, packet, tcp, timeout):
        self.packet = packet
        self.tcp = tcp
        self.deadline = time.monotonic() + timeout
        self.done = threading.Event()
        self.response = None


class LimitedDnsServer(socketserver.ThreadingMixIn):
    daemon_threads = True
    block_on_close = False

    def __init__(self, *args):
        self.slots = threading.BoundedSemaphore(32)
        super().__init__(*args)

    def server_bind(self):
        if platform.system() == 'Windows':
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()


class LocalDnsUdp(LimitedDnsServer, socketserver.UDPServer):
    pass


class LocalDnsTcp(LimitedDnsServer, socketserver.TCPServer):
    pass


class DnsUdpHandler(socketserver.BaseRequestHandler):
    def handle(self):
        packet, connection = self.request
        try:
            response = self.server.tunnel.resolve(packet, False)
            connection.sendto(response, self.client_address)
        except (ValueError, OSError):
            pass


class DnsTcpHandler(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(20)
        try:
            while not self.server.tunnel.stopping.is_set():
                size = struct.unpack('!H', read_exact(self.request, 2))[0]
                packet = read_exact(self.request, size)
                response = self.server.tunnel.resolve(packet, True)
                self.request.sendall(struct.pack('!H', len(response)) + response)
        except (ValueError, OSError, EOFError):
            pass


class DnsTunnel:
    """Bounded local DNS proxy; only the parent partition resolves queries."""

    def __init__(self, port=53, timeout=18):
        self.timeout = timeout
        self.jobs = queue.Queue(maxsize=64)
        self.stopping = threading.Event()
        self.lock = threading.Lock()
        self.active = 0
        self.host_slots = threading.BoundedSemaphore(4)
        self.host_connections = set()
        self.servers = []
        self.threads = []
        self.listener = None
        try:
            tcp = LocalDnsTcp(('127.0.0.1', port), DnsTcpHandler)
            self.servers.append(tcp)
            self.port = tcp.server_address[1]
            self.servers.append(LocalDnsUdp(('127.0.0.1', self.port), DnsUdpHandler))
            for server in self.servers:
                server.tunnel = self
                thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': 0.2}, daemon=True)
                thread.start()
                self.threads.append(thread)
        except BaseException:
            self.close()
            raise

    def resolve(self, packet, tcp):
        dns_question_end(packet)
        job = DnsJob(packet, tcp, self.timeout)
        with self.lock:
            if not self.active or self.stopping.is_set():
                return dns_failure(packet)
            try:
                self.jobs.put_nowait(job)
            except queue.Full:
                return dns_failure(packet)
        job.done.wait(self.timeout)
        return job.response if job.response is not None else dns_failure(packet)

    def host_channel(self, connection):
        with self.lock:
            if self.stopping.is_set():
                connection.close()
                return
            self.host_connections.add(connection)
            self.active += 1
        try:
            with connection:
                connection.settimeout(20)
                while not self.stopping.is_set():
                    try:
                        job = self.jobs.get(timeout=1)
                    except queue.Empty:
                        connection.sendall(b'\x00' * 4)
                        continue
                    try:
                        if time.monotonic() >= job.deadline:
                            continue
                        frame = bytes((int(job.tcp),)) + job.packet
                        connection.sendall(struct.pack('!I', len(frame)) + frame)
                        size = struct.unpack('!I', read_exact(connection, 4))[0]
                        if size > 65535:
                            raise ValueError('Invalid DNS response length')
                        if size:
                            response = read_exact(connection, size)
                            if len(response) < 12 or response[:2] != job.packet[:2] or not response[2] & 0x80:
                                raise ValueError('Invalid DNS response')
                            job.response = response
                    finally:
                        job.done.set()
        except (OSError, EOFError, ValueError):
            pass
        finally:
            with self.lock:
                self.active -= 1
                self.host_connections.discard(connection)
                if not self.active:
                    while True:
                        try:
                            self.jobs.get_nowait().done.set()
                        except queue.Empty:
                            break

    def start_host_listener(self, windows):
        if windows:
            listener = socket.socket(socket.AF_HYPERV, socket.SOCK_STREAM, socket.HV_PROTOCOL_RAW)
            address = (socket.HV_GUID_PARENT, DNS_SERVICE)
        else:
            listener = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
            address = (socket.VMADDR_CID_ANY, DNS_PORT)
        try:
            listener.bind(address)
            listener.listen(4)
            listener.settimeout(1)
        except BaseException:
            listener.close()
            raise
        self.listener = listener

        def accept_host():
            while not self.stopping.is_set():
                try:
                    connection, peer = listener.accept()
                except TimeoutError:
                    continue
                except OSError:
                    break
                if (not windows and peer[0] != socket.VMADDR_CID_HOST) or not self.host_slots.acquire(blocking=False):
                    connection.close()
                    continue

                def serve_host(conn):
                    try:
                        self.host_channel(conn)
                    finally:
                        self.host_slots.release()

                try:
                    threading.Thread(target=serve_host, args=(connection,), daemon=True).start()
                except BaseException:
                    self.host_slots.release()
                    connection.close()
                    raise

        thread = threading.Thread(target=accept_host, daemon=True)
        thread.start()
        self.threads.append(thread)

    def close(self):
        self.stopping.set()
        if self.listener is not None:
            self.listener.close()
        with self.lock:
            for connection in self.host_connections:
                try:
                    connection.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass
            while True:
                try:
                    self.jobs.get_nowait().done.set()
                except queue.Empty:
                    break
        for server, thread in zip(self.servers, self.threads):
            if thread.is_alive():
                server.shutdown()
        for server in self.servers:
            server.server_close()
        for thread in self.threads:
            thread.join(timeout=2)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()


def bound_port_keys():
    """Read Linux inet_diag, including TCP sockets bound but not listening."""
    result = set()
    deadline = time.monotonic() + 3
    with socket.socket(socket.AF_NETLINK, socket.SOCK_RAW, 4) as diag:
        diag.bind((0, 0))
        for sequence, (family, protocol) in enumerate(
                ((socket.AF_INET, 6), (socket.AF_INET6, 6),
                 (socket.AF_INET, 17), (socket.AF_INET6, 17)), 1):
            # inet_diag_req_v2 with all states (including TCP_BOUND_INACTIVE).
            payload = struct.pack('=BBBBI', family, protocol, 0, 0, 0xffffffff) + bytes(48)
            diag.sendto(struct.pack('=IHHII', len(payload) + 16, 20, 0x301, sequence, 0) + payload, (0, 0))
            done = False
            while not done:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError('Socket diagnostics timed out')
                diag.settimeout(remaining)
                data, _, flags, peer = diag.recvmsg(262144)
                if flags & socket.MSG_TRUNC or peer[0] != 0:
                    raise RuntimeError('Incomplete socket diagnostic response')
                offset = 0
                while offset + 16 <= len(data):
                    length, kind, nlflags, seq, _ = struct.unpack_from('=IHHII', data, offset)
                    if length < 16 or offset + length > len(data) or seq != sequence or nlflags & 0x10:
                        raise RuntimeError('Invalid or interrupted socket diagnostic response')
                    body = data[offset + 16:offset + length]
                    offset += (length + 3) & ~3
                    if kind == 3:
                        if len(body) >= 4 and struct.unpack_from('=i', body)[0]:
                            raise RuntimeError('Socket diagnostic dump failed')
                        done = True
                    elif kind == 2:
                        code = struct.unpack_from('=i', body)[0]
                        if code:
                            raise OSError(-code, os.strerror(-code))
                    elif kind == 20 and len(body) >= 72:
                        port = struct.unpack_from('!H', body, 4)[0]
                        if port:
                            result.add((protocol, port))
    return result


def check_bind_support():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as probe:
        probe.bind(('127.0.0.1', 0))
        if (6, probe.getsockname()[1]) not in bound_port_keys():
            raise RuntimeError('Bind coordination requires Linux support for bound-inactive TCP diagnostics (Linux 6.7+)')
    return True


def bind_request_key(request):
    if len(request) != BIND_REQUEST_SIZE:
        raise ValueError('Invalid bind request size')
    proto, family, port = struct.unpack_from('!BBH', request)
    if proto not in (6, 17) or family not in (4, 6) or not port:
        raise ValueError('Invalid bind request')
    if family == 4 and any(request[8:]):
        raise ValueError('Invalid IPv4 address padding')
    return proto, port


class BindCoordinator:
    """Local application broker for one host-initiated Hyper-V channel."""
    def __init__(self, path, group=None, inspect_ports=bound_port_keys, clock=time.monotonic):
        self.path = pathlib.Path(path)
        self.group = group
        self.inspect_ports = inspect_ports
        self.clock = clock
        self.lock = threading.Lock()
        self.pending = {}
        self.channel = None
        self.local_listener = None
        self.socket_identity = None
        self.netns_cookie = None
        self.host_listener = None
        self.stopping = threading.Event()
        self.threads = []
        self.failures = 0
        self.conflicts = 0

    def attach(self, channel):
        channel.settimeout(3)
        with self.lock:
            if self.stopping.is_set():
                channel.close()
                return
            if self.channel is not None:
                self.channel.close()
            self.channel = channel

    def _exchange(self, request):
        # Called under the lock. A lost response never permits a new bind.
        if self.channel is None:
            return 2
        try:
            self.channel.sendall(request)
            status = read_exact(self.channel, 1)[0]
            if status not in (0, 1, 2):
                raise ValueError('Invalid host bind status')
            return status
        except (OSError, EOFError, ValueError):
            self.channel.close()
            self.channel = None
            self.failures += 1
            return 2

    def reserve(self, request):
        key = bind_request_key(request)
        with self.lock:
            if self.stopping.is_set() or (request not in self.pending and len(self.pending) >= 4096):
                return 2
            status = self._exchange(request)
            if status == 0:
                # Keep the reservation while the application's actual bind is
                # still in progress. Failed binds age out automatically.
                self.pending[request] = self.clock() + 15
            elif status == 1:
                self.conflicts += 1
            return status

    def renew(self):
        live = self.inspect_ports()
        now = self.clock()
        with self.lock:
            requests = list(self.pending)
        for request in requests:
            key = bind_request_key(request)
            with self.lock:
                expiry = self.pending.get(request)
                if expiry is None:
                    continue
                if expiry <= now and key not in live:
                    del self.pending[request]
                    continue
                status = self._exchange(request)
                if status:
                    self.failures += 1
            if self.stopping.is_set():
                break
        with self.lock:
            self._exchange(bytes(BIND_REQUEST_SIZE))

    def _renew_loop(self):
        while not self.stopping.wait(5):
            try:
                self.renew()
            except (OSError, RuntimeError, ValueError) as exc:
                self.failures += 1
                print(f'Bind coordination: {exc}', file=sys.stderr)
                with self.lock:
                    self._exchange(bytes(BIND_REQUEST_SIZE))

    def _read_local_request(self, conn):
        # SCM_RIGHTS supplies the socket that will be bound. SO_NETNS_COOKIE
        # verifies its actual namespace without inspecting another UID's
        # /proc entries or granting the service CAP_SYS_PTRACE.
        fds = array.array('i')
        data, ancillary, flags, _ = conn.recvmsg(
            BIND_REQUEST_SIZE, socket.CMSG_SPACE(4 * fds.itemsize), socket.MSG_CMSG_CLOEXEC)
        try:
            for level, kind, value in ancillary:
                if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
                    fds.frombytes(value[:len(value) - len(value) % fds.itemsize])
            if flags & (socket.MSG_CTRUNC | socket.MSG_TRUNC) or len(fds) != 1:
                raise ValueError('One application socket is required')
            request = data + read_exact(conn, BIND_REQUEST_SIZE - len(data))
            protocol, _ = bind_request_key(request)
            with socket.socket(fileno=os.dup(fds[0])) as target:
                if target.getsockopt(socket.SOL_SOCKET, SO_NETNS_COOKIE, 8) != self.netns_cookie:
                    raise ValueError('Application socket belongs to another network namespace')
                if target.getsockopt(socket.SOL_SOCKET, socket.SO_PROTOCOL) != protocol:
                    raise ValueError('Application socket protocol does not match request')
                family = target.getsockopt(socket.SOL_SOCKET, socket.SO_DOMAIN)
                if family not in (socket.AF_INET, socket.AF_INET6) or (
                        request[1] == 6 and family != socket.AF_INET6):
                    raise ValueError('Application socket address family does not match request')
            return request
        finally:
            for fd in fds:
                os.close(fd)

    def _accept_local(self):
        while not self.stopping.is_set():
            try:
                conn, _ = self.local_listener.accept()
            except TimeoutError:
                continue
            except OSError:
                break
            with conn:
                conn.settimeout(3)
                try:
                    request = self._read_local_request(conn)
                    conn.sendall(bytes([self.reserve(request)]))
                except (OSError, ValueError, EOFError):
                    try:
                        conn.sendall(b'\x02')
                    except OSError:
                        pass

    def _accept_host(self):
        while not self.stopping.is_set():
            try:
                conn, peer = self.host_listener.accept()
            except TimeoutError:
                continue
            except OSError:
                break
            if peer[0] != socket.VMADDR_CID_HOST:
                conn.close()
            else:
                self.attach(conn)

    def start(self, listen_host=True):
        if self.path.is_symlink():
            raise RuntimeError('Unsafe bind coordination socket')
        if self.path.exists():
            previous = self.path.lstat()
            if not stat.S_ISSOCK(previous.st_mode) or previous.st_uid != os.geteuid():
                raise RuntimeError('Bind coordination path is not an owned socket')
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as probe:
                probe.settimeout(1)
                try:
                    probe.connect(str(self.path))
                except ConnectionRefusedError:
                    if self.path.lstat().st_ino != previous.st_ino:
                        raise RuntimeError('Bind socket changed during recovery')
                    self.path.unlink()
                else:
                    raise RuntimeError('Bind coordination service is already running')
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as probe:
            self.netns_cookie = probe.getsockopt(socket.SOL_SOCKET, SO_NETNS_COOKIE, 8)
        self.local_listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            self.local_listener.bind(str(self.path))
            info = self.path.lstat()
            self.socket_identity = (info.st_dev, info.st_ino)
            if self.group is not None:
                os.chown(self.path, -1, self.group)
            os.chmod(self.path, 0o660)
            self.local_listener.listen(64)
            self.local_listener.settimeout(0.5)
            if listen_host:
                self.host_listener = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
                self.host_listener.bind((socket.VMADDR_CID_ANY, BIND_PORT))
                self.host_listener.listen(1)
                self.host_listener.settimeout(0.5)
            for target in ([self._accept_local, self._renew_loop] +
                           ([self._accept_host] if listen_host else [])):
                thread = threading.Thread(target=target, daemon=True)
                thread.start()
                self.threads.append(thread)
        except BaseException:
            self.close()
            raise

    def close(self):
        self.stopping.set()
        for listener in (self.local_listener, self.host_listener):
            if listener is not None:
                listener.close()
        with self.lock:
            if self.channel is not None:
                self.channel.close()
                self.channel = None
        for thread in self.threads:
            thread.join(timeout=5)
        try:
            info = self.path.lstat()
            if stat.S_ISSOCK(info.st_mode) and (info.st_dev, info.st_ino) == self.socket_identity:
                self.path.unlink()
        except FileNotFoundError:
            pass


def prepare_tunnel_snapshot(settings, tunnel, windows):
    if not settings['DnsTunnel']:
        return settings
    if tunnel is None or tunnel.stopping.is_set():
        raise RuntimeError('Install and start the guest service with DNS tunneling enabled')
    settings = dict(settings)
    settings['DNS'] = dict(settings['DNS'])
    # Linux uses the global systemd-resolved loopback stub configured by
    # the installer. Windows can use the local resolver on each adapter.
    settings['DNS']['Servers'] = ['127.0.0.1'] if windows and settings['Interface']['Connected'] else []
    return settings


def handle(sock, backend, tools=None, dns_tunnel=None):
    try:
        request = receive(sock)
        target, resource, action, settings = normalize(request)
        response = {'Version': 1, 'Success': True}
        if resource == 'Heartbeat':
            response['Data'] = {'Alive': True, 'ProtocolVersion': 1,
                                'ServiceUptimeSeconds': max(0, int(time.monotonic() - STARTED))}
        elif resource == 'GuestInformation':
            response['Data'] = guest_information(tools)
        elif resource in ('TimeSync', 'FileExchange'):
            response['Data'] = (tools or GuestTools()).request(resource, action, settings)
        elif resource == 'Diagnostics':
            response['Data'] = backend.diagnostics()
        elif resource == 'Connectivity':
            with socket.create_connection((settings['Address'], settings['Port']), timeout=5) as connection:
                response['Data'] = {'LocalAddress': connection.getsockname()[0], 'RemoteAddress': settings['Address'], 'Port': settings['Port']}
        elif resource == 'LoopbackReconcile':
            if platform.system() != 'Linux':
                raise RuntimeError('GELNIC routing currently requires a Linux guest')
            response['Data'] = backend.reconcile_loopback(settings)
        elif resource == 'Reconcile':
            snapshot = prepare_tunnel_snapshot(settings, dns_tunnel, platform.system() == 'Windows')
            response['Data'] = backend.reconcile(target, snapshot)
            if settings['DnsTunnel']:
                response['Data']['DnsTunnel'] = True
        else:
            backend.apply(target, resource, action, settings)
    except Exception as exc:
        detail = getattr(exc, 'stderr', '') or str(exc)
        if isinstance(detail, bytes):
            detail = detail.decode('utf-8', errors='replace')
        response = {'Version': 1, 'Success': False, 'Error': detail[:4000]}
    try:
        send(sock, response)
    except (TypeError, ValueError, OverflowError):
        # Encoding and size validation precede transmission of the frame.
        send(sock, {'Version': 1, 'Success': False, 'Error': 'Guest response cannot be encoded or exceeds the message limit'})


def serve(exchange_root=None, allow_clock_set=False, enable_dns_tunnel=False, loopback_state=None, bind_coordination=False):
    exchange = FileExchange(exchange_root) if exchange_root is not None else None
    tools = GuestTools(exchange, allow_clock_set)
    windows = platform.system() == 'Windows'
    backend = WindowsBackend() if windows else LinuxBackend(loopback_state=loopback_state)
    tunnel = DnsTunnel() if enable_dns_tunnel else None
    coordinator = None
    try:
        if bind_coordination:
            if windows:
                raise RuntimeError('Bind coordination requires a Linux guest')
            import grp
            check_bind_support()
            coordinator = BindCoordinator(BIND_SOCKET, grp.getgrnam('nanabox-bind').gr_gid)
            coordinator.start()
        if tunnel is not None:
            tunnel.start_host_listener(windows)
        serve_network(backend, tools, windows, tunnel)
    finally:
        if coordinator is not None:
            coordinator.close()
        if tunnel is not None:
            tunnel.close()


def serve_network(backend, tools, windows, tunnel):
    if windows:
        listener = socket.socket(socket.AF_HYPERV, socket.SOCK_STREAM, socket.HV_PROTOCOL_RAW)
        address = (socket.HV_GUID_PARENT, SERVICE)
    else:
        listener = socket.socket(socket.AF_VSOCK, socket.SOCK_STREAM)
        address = (socket.VMADDR_CID_ANY, PORT)
    with listener:
        listener.bind(address)
        listener.listen(8)
        while True:
            conn, peer = listener.accept()
            with conn:
                if not windows and peer[0] != socket.VMADDR_CID_HOST:
                    continue
                conn.settimeout(90)
                try:
                    handle(conn, backend, tools, tunnel)
                except (OSError, EOFError, ValueError) as exc:
                    print(f'GNS connection: {exc}', file=sys.stderr)


def host_send(vm_id, request):
    normalize(request)
    with socket.socket(socket.AF_HYPERV, socket.SOCK_STREAM, socket.HV_PROTOCOL_RAW) as conn:
        conn.settimeout(90)
        conn.connect((vm_id, SERVICE))
        send(conn, request)
        return receive(conn)


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    commands = cli.add_subparsers(dest='command', required=True)
    server = commands.add_parser('serve')
    server.add_argument('--exchange-root', type=pathlib.Path, help='Enable file exchange in this protected directory')
    server.add_argument('--allow-clock-set', action='store_true', help='Allow the parent partition to synchronize the guest clock')
    server.add_argument('--dns-tunnel', action='store_true', help='Enable the loopback DNS proxy on port 53')
    server.add_argument('--loopback-state', type=pathlib.Path, help='Enable Linux GELNIC routing and store its prior settings')
    server.add_argument('--bind-coordination', action='store_true', help='Enable the Linux application bind broker')
    commands.add_parser('check-bind-support', help='Verify Linux bound socket diagnostic support')
    clear = commands.add_parser('clear-loopback', help='Restore ordinary Linux loopback routing')
    clear.add_argument('state', type=pathlib.Path)
    host = commands.add_parser('send')
    host.add_argument('vm_id', help='HCS runtime VM ID')
    host.add_argument('request', type=pathlib.Path, help='JSON request')
    query = commands.add_parser('query', help='Query guest agent information')
    query.add_argument('vm_id', help='HCS runtime VM ID')
    query.add_argument('resource', choices=QUERY_CAPABILITIES[:3])
    options = cli.parse_args()
    if options.command == 'serve':
        serve(options.exchange_root, options.allow_clock_set, options.dns_tunnel, options.loopback_state, options.bind_coordination)
    elif options.command == 'check-bind-support':
        check_bind_support()
    elif options.command == 'clear-loopback':
        if platform.system() != 'Linux':
            raise RuntimeError('Linux is required')
        LoopbackRouting(run, options.state).clear()
    else:
        import uuid
        request = ({'Version': 1, 'ResourceType': options.resource, 'RequestType': 'Query', 'Settings': {}}
                   if options.command == 'query' else json.loads(options.request.read_text(encoding='utf-8-sig')))
        result = host_send(str(uuid.UUID(options.vm_id)), request)
        print(json.dumps(result, ensure_ascii=False))
        return 0 if result.get('Success') is True else 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
