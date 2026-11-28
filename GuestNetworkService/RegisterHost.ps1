# Run on the Windows Hyper-V host as administrator.
$ErrorActionPreference = 'Stop'
$services = @{
    '{00006b3d-facb-11e6-bd58-64006a7986d3}' = 'NanaBox Guest Network Service'
    '{00006b3e-facb-11e6-bd58-64006a7986d3}' = 'NanaBox DNS Tunnel'
    '{00006b3f-facb-11e6-bd58-64006a7986d3}' = 'NanaBox Bind Coordination'
}
foreach ($guid in $services.Keys) {
    $name = $services[$guid]
    $key = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Virtualization\GuestCommunicationServices\$guid"
    if (Test-Path -LiteralPath $key) {
        $existing = (Get-ItemProperty -LiteralPath $key -Name ElementName -ErrorAction SilentlyContinue).ElementName
        if ($existing -ne $name) { throw "The service GUID $guid is already registered by another application." }
    } else {
        New-Item -Path $key -Force | Out-Null
        New-ItemProperty -Path $key -Name ElementName -Value $name -PropertyType String -Force | Out-Null
    }
}
