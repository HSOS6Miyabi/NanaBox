# Run on the Windows Hyper-V host as administrator.
$ErrorActionPreference = 'Stop'
$guid = '{00006b3d-facb-11e6-bd58-64006a7986d3}'
$key = "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Virtualization\GuestCommunicationServices\$guid"
if (Test-Path -LiteralPath $key) {
    $existing = (Get-ItemProperty -LiteralPath $key -Name ElementName -ErrorAction SilentlyContinue).ElementName
    if ($existing -ne 'NanaBox Guest Network Service') { throw 'The service GUID is already registered by another application.' }
    return
}
New-Item -Path $key -Force | Out-Null
New-ItemProperty -Path $key -Name ElementName -Value 'NanaBox Guest Network Service' -PropertyType String -Force | Out-Null
