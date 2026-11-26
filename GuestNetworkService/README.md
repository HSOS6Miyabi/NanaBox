# NanaBox Guest Network Service

A guest network configuration agent for Windows and Linux virtual machines. It accepts versioned network configuration requests from the Hyper-V parent partition over Hyper-V sockets (Linux VSOCK port 27453). It identifies a hardware network adapter by its MAC address and applies IPv4/IPv6 addresses, routes, DNS servers and interface state/MTU. It does not accept executable commands.

The NanaBox host's MirroredNetworking component registers an HCN guest network service using Microsoft's installed WSL RPC dependency, creates endpoints on HCN flow-steering networks, reserves an ephemeral port range and configured TCP/UDP listening ports, and synchronizes host interface changes over this protocol. Enable Mirrored networking in the VM's Network settings before boot. Use a dedicated guest with its network manager configured for host-managed adapters. The host supports explicit listening-port reservations and optional periodic discovery of non-loopback guest TCP listeners and bound UDP ports. Explicit reservations are acquired before starting the synchronization worker.

## Installation

Run `RegisterHost.ps1` as an administrator on the Windows Hyper-V host.

On a Linux guest with systemd, Python 3, iproute2 and systemd-resolved:

```sh
sudo install -d -m 755 /opt/nanabox-gns
sudo install -m 644 gns.py /opt/nanabox-gns/gns.py
sudo install -m 644 nanabox-gns.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now nanabox-gns.service
```

On a Windows guest with Python 3.12 or later installed for all users, run as administrator:

```powershell
.\InstallWindowsGuest.ps1 -PythonPath 'C:\Program Files\Python313\python.exe'
```

Specify the installed interpreter's actual path. The startup task runs as SYSTEM. Configure only adapters dedicated to host-managed networking; other guest network managers may overwrite these settings.

## Host requests

Use Python 3.12+ on Windows. Send a request to the running HCS VM's runtime GUID:

```powershell
python gns.py send <runtime-vm-guid> request.json
```

Example request (replace the MAC and addresses with the target interface's values):

```json
{
  "Version": 1,
  "AdapterMac": "00-15-5D-12-34-56",
  "ResourceType": "IPAddress",
  "RequestType": "Add",
  "Settings": {
    "Address": "192.0.2.20",
    "OnLinkPrefixLength": 24
  }
}
```

| ResourceType | RequestType | Settings |
|---|---|---|
| IPAddress | Add, Update, Remove | Address, OnLinkPrefixLength |
| Route | Add, Update, Remove | DestinationPrefix, NextHop (optional for on-link routes), Metric |
| DNS | Add, Update, Remove | ServerList (array of IP addresses; Remove restores resolver defaults) |
| Interface | Add, Update | Connected (boolean), NlMtu |
| PortRange | Update | Start, End (inclusive dynamic port range, at least 255 ports) |

Messages use a 4-byte big-endian length followed by UTF-8 JSON (maximum 256 KiB); one request and one response per connection. Replies include Version, Success and, on failure, Error. Linux route operations use protocol number 242 to distinguish agent-managed routes. Linux DNS management requires systemd-resolved. On Windows the agent calls the NetTCPIP and DnsClient PowerShell cmdlets.

## Testing

```sh
python -m unittest discover -s GuestNetworkService -v
```

The tests verify protocol framing, validation, error replies and command generation for both platforms without modifying runner network settings. Actual Hyper-V socket connectivity and network changes require a Hyper-V host and installed guest agent.

## Architecture references

The guest-side operations follow the approach in Microsoft's MIT-licensed [WSL GnsEngine](https://github.com/microsoft/WSL/blob/master/src/linux/init/GnsEngine.cpp), [NetworkManager](https://github.com/microsoft/WSL/blob/master/src/linux/init/NetworkManager.cpp) and [HCN schema](https://github.com/microsoft/WSL/blob/master/src/shared/inc/hns_schema.h). NanaBox uses its own transport and implementation for ordinary Windows and Linux guests. See also [Hyper-V socket integration services](https://learn.microsoft.com/virtualization/hyper-v-on-windows/user-guide/make-integration-service).

For mirrored networking, dedicate guest adapters to host-managed configuration. Exclude those interfaces from guest DHCP clients and network managers. The service disables IPv6 router advertisements, autoconfiguration, and duplicate address detection on mirrored Linux interfaces; on Windows it disables DHCP, router discovery, and duplicate address detection on the corresponding managed interfaces. Applications using listening ports outside the reserved ephemeral range must have their ports explicitly configured in NanaBox before the VM starts.

Read-only requests: `{"Version":1,"ResourceType":"Diagnostics","RequestType":"Query","Settings":{}}` returns actual guest addresses, adapters, GPU information and dynamic port ranges. `Connectivity` with `RequestType: "Query"` and settings `Address` (literal IP) and `Port` attempts a single TCP connection and reports its local source address. Both operations use the same host-only transport.
The host registers the Hyper-V socket service when starting a mirrored VM, checking that the service GUID is not registered to another application.

The host sends a complete `Reconcile` request for each dedicated mirrored adapter. The guest validates and compares the desired addresses, routes, DNS, MTU and dynamic port range with its actual network configuration. Direct routes are installed before gateway routes; stale agent-managed routes are removed before addresses. Repeated synchronization repairs configuration drift after guest service restarts without a host-side applied-state cache. IPv6 link-local addresses remain managed by the guest kernel. Use the current host and guest service versions together.

On Ubuntu, the guest workflow additionally runs `live_network_test.py` in an isolated network namespace with a real veth pair. It checks route ordering, stale-address cleanup, repeated reconciliation and recovery after simulated drift. Resolver calls in that test use a controlled in-memory resolver; production resolver commands use systemd-resolved.

Automatic listening-port reservation is enabled by default in mirrored mode and may be disabled using `MirroredAutoReservePorts`. The guest reports TCP listeners and non-loopback UDP endpoints, and the host reserves or releases HCN ports as the discovered set changes. Conflicts with existing host ports are shown in the VM status. Periodic discovery happens after the guest application binds; applications requiring guaranteed availability at bind time should reserve those ports explicitly before starting the VM. This does not intercept arbitrary application bind system calls.
Host interface metrics are synchronized alongside routes to retain relative preference across Ethernet, wireless, VPN and other mirrored interfaces. The Linux integration test also checks IPv6 gateway routes and preserves routes belonging to a second interface.

NanaBox subscribes to HCN network events and Windows IP Helper address, interface and route notifications, then periodically reconciles actual guest state. The host re-registers its HCN session after a disconnect and reuses endpoint identities during the VM process lifetime. `MirroredIgnoredPorts` excludes ports from periodic listener reservation (both TCP and UDP); it cannot overlap explicit reservations and does not override the HCN dynamic port range. The host also mirrors the connection-specific DNS suffix. This is search-suffix synchronization, not WSL DNS Tunneling. The Linux guest applies per-interface ARP and reverse-path settings for dedicated mirrored interfaces; global forwarding and external-interface loopback settings are unchanged.
