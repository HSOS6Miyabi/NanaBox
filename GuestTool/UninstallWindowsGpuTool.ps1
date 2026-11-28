# Disable GPU-PV devices before removing the service.
param([switch]$PrepareMaintenance)
$ErrorActionPreference = 'Stop'
$name = 'NanaBoxGuestGpu'
$root = Join-Path $env:ProgramFiles 'NanaBox Guest GPU'
$binary = Join-Path $root 'nanabox-gpu.exe'
if (Get-Process -Name 'Mile.Cirno' -ErrorAction SilentlyContinue) { throw 'Disable GPU-PV devices and dismount Mile.Cirno first.' }
$service = Get-Service -Name $name -ErrorAction SilentlyContinue
if ($PrepareMaintenance) {
    if ($service) {
        Set-Service -Name $name -StartupType Disabled
        Write-Host 'The service will stay stopped after the next Windows restart. Restart Windows before updating or uninstalling it.'
    }
    return
}
if ($service -and $service.Status -ne 'Stopped' -and -not $service.CanStop) {
    throw 'A native share transport is active. Unmount managed Plan 9 shares, or run this script with -PrepareMaintenance and restart Windows before removal.'
}
if ($service -and $service.Status -ne 'Stopped') {
    Stop-Service -Name $name
    (Get-Service -Name $name).WaitForStatus('Stopped',[TimeSpan]::FromSeconds(30))
}
if (Test-Path -LiteralPath $binary -PathType Leaf) {
    & $binary --cleanup
    if ($LASTEXITCODE -ne 0) { throw 'GPU link cleanup failed.' }
}
if ($service) {
    & sc.exe delete $name
    if ($LASTEXITCODE -ne 0) { throw 'GPU service deletion failed.' }
}
$key = 'HKLM:\SOFTWARE\NanaBox\GuestTool\Gpu'
if (Test-Path -LiteralPath $key) { Remove-Item -LiteralPath $key -Force }
if (Test-Path -LiteralPath $root) {
    $item = Get-Item -LiteralPath $root -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Unsafe GPU service directory.' }
    $client = Join-Path $root 'Cirno'
    if (Test-Path -LiteralPath $client) {
        if ((Get-Item -LiteralPath $client -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Unsafe GPU client directory.' }
        Get-ChildItem -LiteralPath $client -File | Where-Object { $_.Extension -in '.exe','.dll' } | Remove-Item -Force
        if (-not (Get-ChildItem -LiteralPath $client -Force)) { Remove-Item -LiteralPath $client }
    }
    if (Test-Path -LiteralPath $binary) { Remove-Item -LiteralPath $binary -Force }
    $plan9 = Join-Path $root 'nanabox-plan9-winfsp.exe'
    if (Test-Path -LiteralPath $plan9) { Remove-Item -LiteralPath $plan9 -Force }
    if (-not (Get-ChildItem -LiteralPath $root -Force)) { Remove-Item -LiteralPath $root }
}
