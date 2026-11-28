# Disable GPU-PV devices and dismount the Mile.Cirno filesystem before removing the service.
$ErrorActionPreference = 'Stop'
$name = 'NanaBoxGuestGpu'
$root = Join-Path $env:ProgramFiles 'NanaBox Guest GPU'
$binary = Join-Path $root 'nanabox-gpu.exe'
if (Get-Process -Name 'Mile.Cirno' -ErrorAction SilentlyContinue) { throw 'Disable GPU-PV devices and dismount Mile.Cirno first.' }
$service = Get-Service -Name $name -ErrorAction SilentlyContinue
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
    if (-not (Get-ChildItem -LiteralPath $root -Force)) { Remove-Item -LiteralPath $root }
}
