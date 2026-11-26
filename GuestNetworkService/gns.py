"""NanaBox guest network configuration protocol (Python 3.12+ on Windows)."""
import argparse
import ipaddress
import json
import pathlib
import platform
import re
import socket
import struct
import subprocess
import sys

PORT = 27453
SERVICE = f'{PORT:08x}-facb-11e6-bd58-64006a7986d3'
LIMIT = 256 * 1024


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
    target = mac(request['AdapterMac'])
    action, resource, settings = request['RequestType'], request['ResourceType'], request['Settings']
    if action not in ('Add', 'Update', 'Remove') or not isinstance(settings, dict):
        raise ValueError('Invalid operation')
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
        return target, resource, action, {'Destination': str(dest), 'NextHop': str(hop), 'Family': dest.version, 'Metric': number(settings.get('Metric', 0), 0, 65535)}
    if resource == 'Interface' and action != 'Remove':
        if type(settings.get('Connected')) is not bool:
            raise ValueError('Connected must be Boolean')
        mirrored = settings.get('Mirrored', False)
        if type(mirrored) is not bool:
            raise ValueError('Mirrored must be Boolean')
        result = {'Connected': settings['Connected'], 'Mtu': number(settings['NlMtu'], 1280, 65535)}
        if mirrored:
            result['Mirrored'] = True
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
        return target, resource, action, {'Servers': servers}
    raise ValueError('Unsupported resource or operation')


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
$m = @(Get-NetAdapter -IncludeHidden | Where-Object { (([string]$_.MacAddress) -replace '[:-]','').ToLowerInvariant() -eq $r.Target -and $_.HardwareInterface })
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


class WindowsBackend:
    def __init__(self, execute=subprocess.run):
        self.execute = execute

    def apply(self, target, resource, action, settings):
        self.execute(['powershell.exe', '-NoProfile', '-NonInteractive', '-Command', WINDOWS_SCRIPT],
                     input=json.dumps({'Target': target, 'Resource': resource, 'Action': action, 'Settings': settings}),
                     text=True, capture_output=True, check=True, timeout=30)


def handle(sock, backend):
    try:
        request = receive(sock)
        backend.apply(*normalize(request))
        response = {'Version': 1, 'Success': True}
    except Exception as exc:
        detail = getattr(exc, 'stderr', '') or str(exc)
        response = {'Version': 1, 'Success': False, 'Error': detail[:4000]}
    send(sock, response)


def serve():
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
                conn.settimeout(45)
                try:
                    handle(conn, backend)
                except (OSError, EOFError, ValueError) as exc:
                    print(f'GNS connection: {exc}', file=sys.stderr)


def host_send(vm_id, request):
    normalize(request)
    with socket.socket(socket.AF_HYPERV, socket.SOCK_STREAM, socket.HV_PROTOCOL_RAW) as conn:
        conn.settimeout(45)
        conn.connect((vm_id, SERVICE))
        send(conn, request)
        return receive(conn)


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    commands = cli.add_subparsers(dest='command', required=True)
    commands.add_parser('serve')
    host = commands.add_parser('send')
    host.add_argument('vm_id', help='HCS runtime VM ID')
    host.add_argument('request', type=pathlib.Path, help='JSON request')
    options = cli.parse_args()
    if options.command == 'serve':
        serve()
    else:
        import uuid
        result = host_send(str(uuid.UUID(options.vm_id)), json.loads(options.request.read_text(encoding='utf-8-sig')))
        print(json.dumps(result, ensure_ascii=False))
        return 0 if result.get('Success') is True else 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
