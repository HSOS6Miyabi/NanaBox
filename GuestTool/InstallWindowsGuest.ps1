# Run in the Windows guest as administrator with Python 3.12+ installed for all users.
param(
    [Parameter(Mandatory=$true)][string]$PythonPath,
    [switch]$EnableFileExchange,
    [switch]$AllowClockSet,
    [switch]$EnableDnsTunnel,
    [string]$NativeExecutable = (Join-Path $PSScriptRoot 'nanabox-guest-tool.exe')
)
$ErrorActionPreference = 'Stop'
$NativeExecutable = (Resolve-Path -LiteralPath $NativeExecutable).Path
& $NativeExecutable --self-test
if ($LASTEXITCODE -ne 0) { throw 'Native GuestTool self-test failed.' }
$PythonPath = (Resolve-Path -LiteralPath $PythonPath).Path
$programDirs = @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:ProgramW6432) | Where-Object { $_ }
if (-not @($programDirs | Where-Object { $PythonPath.StartsWith($_.TrimEnd('\')+'\', [StringComparison]::OrdinalIgnoreCase) }).Count) {
    throw 'Install Python for all users under Program Files before registering the SYSTEM service.'
}
& $PythonPath -I -c "import socket; assert hasattr(socket, 'AF_HYPERV')"
if ($LASTEXITCODE -ne 0) { throw 'Python 3.12+ with Hyper-V socket support is required' }

function Initialize-ProtectedDirectory([string]$Path) {
    $admins = [System.Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
    $system = [System.Security.Principal.SecurityIdentifier]::new('S-1-5-18')
    $exists = Test-Path -LiteralPath $Path
    if ($exists) {
        $item = Get-Item -LiteralPath $Path -Force
        if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Unsafe service directory: $Path"
        }
        $owner = (Get-Acl -LiteralPath $Path).GetOwner([System.Security.Principal.SecurityIdentifier])
        if ($owner -ne $admins -and $owner -ne $system) {
            throw "Directory must be owned by Administrators or SYSTEM: $Path"
        }
    } else {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
    $acl = [System.Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true,$false)
    $acl.SetOwner($admins)
    foreach ($sid in @($admins,$system)) {
        $rule = [System.Security.AccessControl.FileSystemAccessRule]::new(
            $sid,'FullControl','ContainerInherit, ObjectInherit','None','Allow')
        $acl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
}

$taskName = 'NanaBox Guest Network Service'
$dir = Join-Path $env:ProgramFiles $taskName
if (Test-Path -LiteralPath $dir) {
    $item = Get-Item -LiteralPath $dir -Force
    if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw "Unsafe installation directory: $dir"
    }
} else { New-Item -ItemType Directory -Path $dir | Out-Null }
$arguments = '-I "{0}" serve' -f (Join-Path $dir 'gns.py')
if ($EnableDnsTunnel) { $arguments += ' --dns-tunnel' }
$nativeDir = Join-Path $env:ProgramFiles 'NanaBox GuestTool'
Initialize-ProtectedDirectory $nativeDir
$nativeTarget = Join-Path $nativeDir 'nanabox-guest-tool.exe'
$nativeArgs = '--service'
if ($EnableFileExchange) {
    $exchange = Join-Path $env:ProgramData 'NanaBox Guest Exchange'
    Initialize-ProtectedDirectory $exchange
    $nativeArgs += ' --exchange-root "{0}"' -f $exchange
}
if ($AllowClockSet) { $nativeArgs += ' --allow-clock-set' }

$task = Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
if ($null -ne $task -and $task.State -eq 'Running') {
    Stop-ScheduledTask -TaskName $taskName
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ((Get-ScheduledTask -TaskName $taskName).State -eq 'Running') {
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Previous guest service did not stop' }
        Start-Sleep -Milliseconds 200
    }
}
$nativeService = Get-Service -Name 'NanaBoxGuestTool' -ErrorAction SilentlyContinue
if ($nativeService -and $nativeService.Status -ne 'Stopped') {
    Stop-Service -Name 'NanaBoxGuestTool'
    (Get-Service -Name 'NanaBoxGuestTool').WaitForStatus('Stopped',[TimeSpan]::FromSeconds(30))
}
if (Test-Path -LiteralPath $nativeTarget) {
    if ((Get-Item -LiteralPath $nativeTarget -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Unsafe native GuestTool path.'
    }
}
Copy-Item -LiteralPath $NativeExecutable -Destination $nativeTarget -Force
$serviceImage = '"' + $nativeTarget + '" ' + $nativeArgs
if ($nativeService) {
    & sc.exe config NanaBoxGuestTool binPath= $serviceImage start= auto
} else {
    & sc.exe create NanaBoxGuestTool binPath= $serviceImage start= auto DisplayName= 'NanaBox GuestTool'
}
if ($LASTEXITCODE -ne 0) { throw 'Native GuestTool service registration failed.' }
& sc.exe failure NanaBoxGuestTool reset= 86400 actions= restart/5000/restart/10000/restart/30000
if ($LASTEXITCODE -ne 0) { throw 'Native GuestTool recovery configuration failed.' }
$script = Join-Path $dir 'gns.py'
$temporary = Join-Path $dir ('gns-'+[guid]::NewGuid().ToString('N')+'.tmp')
try {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'gns.py') -Destination $temporary
    if (Test-Path -LiteralPath $script) {
        $item = Get-Item -LiteralPath $script -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Unsafe guest service script' }
        [IO.File]::Replace($temporary,$script,$null)
    } else { [IO.File]::Move($temporary,$script) }
} finally { Remove-Item -LiteralPath $temporary -Force -ErrorAction SilentlyContinue }
$action = New-ScheduledTaskAction -Execute $PythonPath -Argument $arguments -WorkingDirectory $dir
$trigger = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
$settings = New-ScheduledTaskSettingsSet -RestartCount 3 -RestartInterval (New-TimeSpan -Minutes 1) -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
Start-ScheduledTask -TaskName $taskName
Start-Service -Name 'NanaBoxGuestTool'
(Get-Service -Name 'NanaBoxGuestTool').WaitForStatus('Running',[TimeSpan]::FromSeconds(30))
Write-Host 'NanaBox guest service installed and started.'
