#pragma once
constexpr std::string_view WINDOWS_SCRIPT = R"NANABOX(
[Console]::InputEncoding = New-Object System.Text.UTF8Encoding($false)

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

)NANABOX";

constexpr std::string_view WINDOWS_DIAGNOSTICS = R"NANABOX(
[Console]::InputEncoding = New-Object System.Text.UTF8Encoding($false)

$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = New-Object System.Text.UTF8Encoding($false)
$adapters = @(Get-NetAdapter -IncludeHidden | Select-Object Name, MacAddress, ifIndex, Status, InterfaceDescription)
$addresses = @(Get-NetIPAddress | Select-Object -ExpandProperty IPAddress)
$gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name, PNPDeviceID, DriverVersion, Status)
$ports = @(foreach ($af in @('ipv4','ipv6')) { foreach ($p in @('tcp','udp')) { (& netsh.exe int $af show dynamicport $p) -join "`n" } })
[Console]::WriteLine((ConvertTo-Json -InputObject @{OperatingSystem='Windows'; Addresses=$addresses; Adapters=$adapters; Gpu=$gpu; Dxg=$false; DynamicPortRange=$ports} -Depth 6 -Compress))

)NANABOX";

constexpr std::string_view WINDOWS_RECONCILE = R"NANABOX(
[Console]::InputEncoding = New-Object System.Text.UTF8Encoding($false)

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
# Direct routes precede gateway routes.
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

)NANABOX";
