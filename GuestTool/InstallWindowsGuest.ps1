# Run in the Windows guest as administrator.
param(
    [switch]$EnableFileExchange,
    [switch]$AllowClockSet,
    [switch]$EnableDnsTunnel,
    [string]$NativeExecutable = (Join-Path $PSScriptRoot 'nanabox-guest-tool.exe')
)
$ErrorActionPreference = 'Stop'
$NativeExecutable = (Resolve-Path -LiteralPath $NativeExecutable).Path
& $NativeExecutable --self-test
if ($LASTEXITCODE -ne 0) { throw 'Native GuestTool self-test failed.' }

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

$networkService = Get-Service -Name 'NanaBoxGuestNetwork' -ErrorAction SilentlyContinue
if ($networkService -and $networkService.Status -ne 'Stopped') {
    Stop-Service -Name 'NanaBoxGuestNetwork'
    (Get-Service -Name 'NanaBoxGuestNetwork').WaitForStatus('Stopped',[TimeSpan]::FromSeconds(30))
}
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
if ($task) { Unregister-ScheduledTask -TaskName $taskName -Confirm:$false }
$networkImage = '"' + $nativeTarget + '" --service --network'
if ($EnableDnsTunnel) { $networkImage += ' --dns-tunnel' }
if ($networkService) {
    & sc.exe config NanaBoxGuestNetwork binPath= $networkImage start= auto
} else {
    & sc.exe create NanaBoxGuestNetwork binPath= $networkImage start= auto DisplayName= 'NanaBox Guest Network'
}
if ($LASTEXITCODE -ne 0) { throw 'Native network service registration failed.' }
& sc.exe failure NanaBoxGuestNetwork reset= 86400 actions= restart/5000/restart/10000/restart/30000
if ($LASTEXITCODE -ne 0) { throw 'Native network service recovery configuration failed.' }
Start-Service -Name 'NanaBoxGuestNetwork'
(Get-Service -Name 'NanaBoxGuestNetwork').WaitForStatus('Running',[TimeSpan]::FromSeconds(30))
Start-Service -Name 'NanaBoxGuestTool'
(Get-Service -Name 'NanaBoxGuestTool').WaitForStatus('Running',[TimeSpan]::FromSeconds(30))
Write-Host 'NanaBox guest service installed and started.'
