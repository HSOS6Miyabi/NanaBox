"""NanaBox guest network configuration protocol (Python 3.12+ on Windows)."""
import argparse
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
import struct
import secrets
import stat
import tempfile
import subprocess
import sys

PORT = 27453
SERVICE = f'{PORT:08x}-facb-11e6-bd58-64006a7986d3'
LIMIT = 256 * 1024
STARTED = time.monotonic()
QUERY_CAPABILITIES = ('Heartbeat', 'GuestInformation', 'Diagnostics', 'Connectivity', 'TimeSync')
FILE_CHUNK = 64 * 1024
MAX_FILE_SIZE = (1 << 63) - 1
SESSION_TTL = 900
MAX_SESSIONS = 16


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
        return target, resource, action, {
            'DiscoverPorts': discover,
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


def linux_listening_ports(proc, addresses):
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
                if port and not address.is_loopback and (address.is_unspecified or address in accepted):
                    ports.add((protocol, port))
    return [{'Protocol': protocol, 'Port': port} for protocol, port in sorted(ports)]


def run(args):
    return subprocess.run(args, check=True, capture_output=True, text=True, timeout=30).stdout


class LinuxBackend:
    def __init__(self, execute=run, sysfs='/sys/class/net'):
        self.execute = execute
        self.sysfs = pathlib.Path(sysfs)

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

    def _route_change(self, dev, verb, entry):
        # Append instead of replace: equal-cost routes on another interface
        # must remain intact.
        cmd = ['ip', f"-{entry['Family']}", 'route', 'append' if verb == 'add' else verb, entry['Destination']]
        if not ipaddress.ip_address(entry['NextHop']).is_unspecified:
            cmd += ['via', entry['NextHop']]
        cmd += ['dev', dev, 'metric', str(entry['Metric']), 'proto', '242']
        self.execute(cmd)

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
                if snapshot['DiscoverPorts'] and interface['Connected'] else []}

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


def handle(sock, backend, tools=None):
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
        elif resource == 'Reconcile':
            response['Data'] = backend.reconcile(target, settings)
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


def serve(exchange_root=None, allow_clock_set=False):
    exchange = FileExchange(exchange_root) if exchange_root is not None else None
    tools = GuestTools(exchange, allow_clock_set)
    windows = platform.system() == 'Windows'
    backend = WindowsBackend() if windows else LinuxBackend()
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
                    handle(conn, backend, tools)
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
    host = commands.add_parser('send')
    host.add_argument('vm_id', help='HCS runtime VM ID')
    host.add_argument('request', type=pathlib.Path, help='JSON request')
    query = commands.add_parser('query', help='Query guest agent information')
    query.add_argument('vm_id', help='HCS runtime VM ID')
    query.add_argument('resource', choices=QUERY_CAPABILITIES[:3])
    options = cli.parse_args()
    if options.command == 'serve':
        serve(options.exchange_root, options.allow_clock_set)
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
