# Run in a Windows guest as administrator.
param(
    [ValidateSet('VirtualSmb','Plan9')][string]$Backend = 'VirtualSmb',
    [string]$CirnoDirectory,
    [string]$DokanyDriver,
    [string]$CirnoArchive,
    [string]$Executable,
    [switch]$UseExistingDokany,
    [switch]$DisableAutomaticGpuRestart,
    [switch]$EnableWinFspPlan9,
    [string]$WinFspPlan9Executable
)
$ErrorActionPreference = 'Stop'
if (-not [Environment]::Is64BitProcess) { throw 'Use native 64-bit PowerShell.' }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run as administrator.' }
. (Join-Path $PSScriptRoot 'CirnoPackage.ps1')
$architecture = Get-GuestArchitecture
if (-not $Executable) {
    $Executable = if ($architecture -eq 'arm64') {
        Join-Path $PSScriptRoot 'arm64\nanabox-gpu.exe'
    } elseif ($architecture -eq 'x64') {
        Join-Path $PSScriptRoot 'nanabox-gpu.exe'
    } else { throw "Unsupported Windows guest architecture: $architecture" }
}
$binary = (Resolve-Path -LiteralPath $Executable).Path
$name = 'NanaBoxGuestGpu'
$root = Join-Path $env:ProgramFiles 'NanaBox Guest GPU'
$client = Join-Path $root 'Cirno'
$shared = Join-Path $env:ProgramData 'NanaBox Guest Shares'
$destination = Join-Path $root 'nanabox-gpu.exe'
$plan9Root = Join-Path $env:ProgramData 'NanaBox Guest 9P'
$plan9Binary = Join-Path $root 'nanabox-plan9-winfsp.exe'
function Protect-Directory([string]$Path,[switch]$UserRead) {
    $admins = [System.Security.Principal.SecurityIdentifier]::new('S-1-5-32-544')
    $system = [System.Security.Principal.SecurityIdentifier]::new('S-1-5-18')
    if (Test-Path -LiteralPath $Path) {
        $item = Get-Item -LiteralPath $Path -Force
        if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw "Unsafe directory: $Path" }
        $owner = (Get-Acl -LiteralPath $Path).GetOwner([System.Security.Principal.SecurityIdentifier])
        if ($owner -ne $admins -and $owner -ne $system) { throw "Unsafe directory owner: $Path" }
    } else { New-Item -ItemType Directory -Path $Path | Out-Null }
    $acl = [System.Security.AccessControl.DirectorySecurity]::new()
    $acl.SetAccessRuleProtection($true,$false)
    $acl.SetOwner($admins)
    foreach ($sid in @($admins,$system)) {
        $acl.AddAccessRule([System.Security.AccessControl.FileSystemAccessRule]::new(
            $sid,'FullControl','ContainerInherit, ObjectInherit','None','Allow'))
    }
    if ($UserRead) {
        $users = [System.Security.Principal.SecurityIdentifier]::new('S-1-5-32-545')
        $acl.AddAccessRule([System.Security.AccessControl.FileSystemAccessRule]::new(
            $users,'ReadAndExecute','ContainerInherit, ObjectInherit','None','Allow'))
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
}
$service = Get-Service -Name $name -ErrorAction SilentlyContinue
if ($service -and $service.Status -ne 'Stopped' -and -not $service.CanStop) {
    throw 'A native share transport is active. Unmount managed Plan 9 shares, or run UninstallWindowsGpuTool.ps1 -PrepareMaintenance and restart Windows before updating the service.'
}
if ($Backend -eq 'VirtualSmb' -and ($CirnoDirectory -or $CirnoArchive -or $DokanyDriver -or $UseExistingDokany)) {
    throw 'Mile.Cirno and Dokany options require -Backend Plan9.'
}
$windowsBuild = [int](Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -Name CurrentBuildNumber).CurrentBuildNumber
if ($Backend -eq 'VirtualSmb' -and $windowsBuild -lt 18362) {
    throw 'Native Virtual SMB requires Windows build 18362 or later.'
}
if ($EnableWinFspPlan9) {
    $winfsp = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\WOW6432Node\WinFsp' -Name InstallDir -ErrorAction Stop).InstallDir
    $runtime = Join-Path $winfsp ('bin\winfsp-' + $(if ($architecture -eq 'arm64') {'a64'} else {'x64'}) + '.dll')
    if (-not (Test-Path -LiteralPath $runtime -PathType Leaf)) { throw 'Install the native WinFsp runtime before enabling Plan 9.' }
    if (-not $WinFspPlan9Executable) {
        $WinFspPlan9Executable = if ($architecture -eq 'arm64') { Join-Path $PSScriptRoot 'arm64\nanabox-plan9-winfsp.exe' }
        else { Join-Path $PSScriptRoot 'nanabox-plan9-winfsp.exe' }
    }
    $WinFspPlan9Executable = (Resolve-Path -LiteralPath $WinFspPlan9Executable).Path
    if ((Get-PeMachine $WinFspPlan9Executable) -ne (Get-ArchitectureMachine $architecture)) { throw 'Wrong WinFsp Plan 9 client architecture.' }
    Protect-Directory $plan9Root -UserRead
} elseif ($WinFspPlan9Executable) { throw 'WinFspPlan9Executable requires -EnableWinFspPlan9.' }
Protect-Directory $root
if ($Backend -eq 'VirtualSmb') { Protect-Directory $shared -UserRead }
else { Protect-Directory $client }
if ((Get-PeMachine $binary) -ne (Get-ArchitectureMachine $architecture)) { throw 'Wrong GuestTool architecture.' }
$staging = Join-Path $root ('package-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $staging | Out-Null
try {
if (Get-Process -Name 'Mile.Cirno' -ErrorAction SilentlyContinue) {
    throw 'Disable GPU-PV devices and dismount the existing Mile.Cirno filesystem before upgrading the client.'
}
if ($Backend -eq 'Plan9') {
if ($CirnoDirectory) {
    $source = (Resolve-Path -LiteralPath $CirnoDirectory).Path
    $cirno = Join-Path $source 'Mile.Cirno.exe'
    if (-not (Test-Path -LiteralPath $cirno -PathType Leaf) -or
        (Get-PeMachine $cirno) -ne (Get-ArchitectureMachine $architecture)) { throw 'Mile.Cirno executable is missing or has the wrong architecture.' }
    if ($DokanyDriver) { $DokanyDriver = (Resolve-Path -LiteralPath $DokanyDriver).Path }
    elseif (-not $UseExistingDokany) {
        $candidate = Join-Path $source ('Driver\' + $architecture + '\Dokan2.sys')
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            $candidate = Join-Path (Split-Path -Parent $source) ('Driver\' + $architecture + '\Dokan2.sys')
        }
        if (Test-Path -LiteralPath $candidate -PathType Leaf) { $DokanyDriver = $candidate }
    }
} else {
    $archive = Join-Path $staging 'Cirno.zip'
    if (-not $CirnoArchive) {
        $bundled = Join-Path $PSScriptRoot 'Cirno.zip'
        if (Test-Path -LiteralPath $bundled -PathType Leaf) { $CirnoArchive = $bundled }
    }
    if ($CirnoArchive) { Copy-Item -LiteralPath $CirnoArchive -Destination $archive }
    Get-CirnoArchive $archive
    $expanded = Join-Path $staging 'Extracted'
    Expand-CirnoPackage $archive $expanded
    $files = Resolve-CirnoFiles $expanded $architecture
    $source = $files.ClientDirectory
    $DokanyDriver = $files.Driver
}
Install-CirnoDriver $DokanyDriver $architecture -UseExistingDriver:$UseExistingDokany
}
$service = Get-Service -Name $name -ErrorAction SilentlyContinue
if ($service -and $service.Status -ne 'Stopped') {
    Stop-Service -Name $name
    (Get-Service -Name $name).WaitForStatus('Stopped',[TimeSpan]::FromSeconds(30))
}
function Copy-ProtectedFile([string]$Source,[string]$Destination) {
    if ((Get-Item -LiteralPath $Source -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Unsafe source: $Source" }
    if (Test-Path -LiteralPath $Destination) {
        if ((Get-Item -LiteralPath $Destination -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Unsafe destination: $Destination" }
    }
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}
Copy-ProtectedFile $binary $destination
if ($EnableWinFspPlan9) { Copy-ProtectedFile $WinFspPlan9Executable $plan9Binary }
if ($Backend -eq 'Plan9') {
    Get-ChildItem -LiteralPath $source -File | Where-Object { $_.Extension -in '.exe','.dll' } | ForEach-Object {
        Copy-ProtectedFile $_.FullName (Join-Path $client $_.Name)
    }
}
$key = 'HKLM:\SOFTWARE\NanaBox\GuestTool\Gpu'
New-Item -Path $key -Force | Out-Null
New-ItemProperty -Path $key -Name Backend -Value $Backend -PropertyType String -Force | Out-Null
if ($Backend -eq 'Plan9') {
    New-ItemProperty -Path $key -Name CirnoPath -Value (Join-Path $client 'Mile.Cirno.exe') -PropertyType String -Force | Out-Null
} else {
    Remove-ItemProperty -Path $key -Name CirnoPath -ErrorAction SilentlyContinue
}
if ($EnableWinFspPlan9) {
    New-ItemProperty -Path $key -Name WinFspPlan9Path -Value $plan9Binary -PropertyType String -Force | Out-Null
} else {
    Remove-ItemProperty -Path $key -Name WinFspPlan9Path -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $plan9Binary) { Remove-Item -LiteralPath $plan9Binary -Force }
}
New-ItemProperty -Path $key -Name AutomaticRestart -Value $(if ($DisableAutomaticGpuRestart) {'0'} else {'1'}) -PropertyType String -Force | Out-Null
$image = '"' + $destination + '"'
if ($service) {
    & sc.exe config $name binPath= $image start= auto
} else {
    & sc.exe create $name binPath= $image start= auto DisplayName= 'NanaBox Guest GPU'
}
if ($LASTEXITCODE -ne 0) { throw 'Failed to register GPU service.' }
& sc.exe failure $name reset= 86400 actions= restart/5000/restart/10000/restart/30000
if ($LASTEXITCODE -ne 0) { throw 'Failed to configure GPU service recovery.' }
Start-Service -Name $name
(Get-Service -Name $name).WaitForStatus('Running',[TimeSpan]::FromSeconds(30))
Write-Host "NanaBox Windows share and GPU service is running ($Backend). Use the host Guest tools to inspect the mounts."
} finally {
    if (Test-Path -LiteralPath $staging) { Remove-Item -LiteralPath $staging -Recurse -Force }
}
