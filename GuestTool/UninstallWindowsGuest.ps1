# Run as administrator in the Windows guest.
$ErrorActionPreference = 'Stop'
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
