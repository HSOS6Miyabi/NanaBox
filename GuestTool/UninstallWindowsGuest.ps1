# Run as administrator in the Windows guest.
$ErrorActionPreference = 'Stop'
$network = Get-Service -Name 'NanaBoxGuestNetwork' -ErrorAction SilentlyContinue
if ($network) {
    if ($network.Status -ne 'Stopped') {
        Stop-Service -Name 'NanaBoxGuestNetwork'
        (Get-Service -Name 'NanaBoxGuestNetwork').WaitForStatus('Stopped',[TimeSpan]::FromSeconds(30))
    }
    & sc.exe delete NanaBoxGuestNetwork
    if ($LASTEXITCODE -ne 0) { throw 'Native network service deletion failed.' }
}
$native = Get-Service -Name 'NanaBoxGuestTool' -ErrorAction SilentlyContinue
if ($native) {
    if ($native.Status -ne 'Stopped') {
        Stop-Service -Name 'NanaBoxGuestTool'
        (Get-Service -Name 'NanaBoxGuestTool').WaitForStatus('Stopped',[TimeSpan]::FromSeconds(30))
    }
    & sc.exe delete NanaBoxGuestTool
    if ($LASTEXITCODE -ne 0) { throw 'Native GuestTool service deletion failed.' }
}
$nativeDir = Join-Path $env:ProgramFiles 'NanaBox GuestTool'
if (Test-Path -LiteralPath $nativeDir) {
    $item = Get-Item -LiteralPath $nativeDir -Force
    if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Unsafe native GuestTool directory.' }
    $nativeFile = Join-Path $nativeDir 'nanabox-guest-tool.exe'
    if (Test-Path -LiteralPath $nativeFile) { Remove-Item -LiteralPath $nativeFile -Force }
    if (-not (Get-ChildItem -LiteralPath $nativeDir -Force)) { Remove-Item -LiteralPath $nativeDir }
}
$taskName = 'NanaBox Guest Network Service'
$task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if ($null -ne $task) {
    if ($task.State -eq 'Running') {
        Stop-ScheduledTask -TaskName $taskName
        $deadline = [DateTime]::UtcNow.AddSeconds(30)
        while ((Get-ScheduledTask -TaskName $taskName).State -eq 'Running') {
            if ([DateTime]::UtcNow -ge $deadline) { throw 'Guest service did not stop' }
            Start-Sleep -Milliseconds 200
        }
    }
    Unregister-ScheduledTask -TaskName $taskName -Confirm:$false
}
$dir = Join-Path $env:ProgramFiles $taskName
if (Test-Path -LiteralPath $dir) {
    $item = Get-Item -LiteralPath $dir -Force
    if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Unsafe installation directory: $dir"
    }
    $script = Join-Path $dir 'gns.py'
    if (Test-Path -LiteralPath $script) { Remove-Item -LiteralPath $script -Force }
    if (@(Get-ChildItem -LiteralPath $dir -Force).Count -eq 0) { Remove-Item -LiteralPath $dir }
}
Write-Host 'Guest service removed. Exchanged files remain in ProgramData\NanaBox Guest Exchange.'
