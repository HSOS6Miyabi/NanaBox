# Run in a Windows guest as administrator after installing the Mile.Cirno Dokany driver.
param(
    [Parameter(Mandatory=$true)][string]$CirnoDirectory,
    [string]$Executable
)
$ErrorActionPreference = 'Stop'
if (-not [Environment]::Is64BitProcess) { throw 'Use native 64-bit PowerShell.' }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run as administrator.' }
$source = (Resolve-Path -LiteralPath $CirnoDirectory).Path
if (-not $Executable) {
    $architecture = [System.Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    $Executable = if ($architecture -eq 'Arm64') {
        Join-Path $PSScriptRoot 'arm64\nanabox-gpu.exe'
    } elseif ($architecture -eq 'X64') {
        Join-Path $PSScriptRoot 'nanabox-gpu.exe'
    } else { throw "Unsupported Windows guest architecture: $architecture" }
}
$binary = (Resolve-Path -LiteralPath $Executable).Path
if (-not (Test-Path -LiteralPath (Join-Path $source 'Mile.Cirno.exe') -PathType Leaf)) { throw 'Mile.Cirno.exe was not found.' }
$name = 'NanaBoxGuestGpu'
$root = Join-Path $env:ProgramFiles 'NanaBox Guest GPU'
$client = Join-Path $root 'Cirno'
$destination = Join-Path $root 'nanabox-gpu.exe'
function Protect-Directory([string]$Path) {
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
    Set-Acl -LiteralPath $Path -AclObject $acl
}
Protect-Directory $root
Protect-Directory $client
if (Get-Process -Name 'Mile.Cirno' -ErrorAction SilentlyContinue) {
    throw 'Disable GPU-PV devices and dismount the existing Mile.Cirno filesystem before upgrading the client.'
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
Get-ChildItem -LiteralPath $source -File | Where-Object { $_.Extension -in '.exe','.dll' } | ForEach-Object {
    Copy-ProtectedFile $_.FullName (Join-Path $client $_.Name)
}
$key = 'HKLM:\SOFTWARE\NanaBox\GuestTool\Gpu'
New-Item -Path $key -Force | Out-Null
New-ItemProperty -Path $key -Name CirnoPath -Value (Join-Path $client 'Mile.Cirno.exe') -PropertyType String -Force | Out-Null
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
Write-Host 'NanaBox GPU service is running. Use the host Guest tools to inspect the mount.'
