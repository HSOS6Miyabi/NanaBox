$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = [IO.File]::ReadAllText((Join-Path $root 'NanaBox\HostCompute.cpp'))
$match = [regex]::Match($source, 'constexpr wchar_t VmmsManagementScript\[\] = LR"PS\(([\s\S]*?)\)PS"')
if (!$match.Success) { throw 'The VMMS management script was not found.' }

# Each invocation runs the embedded production script in an isolated PowerShell
# process. Only Hyper-V commands are substituted; argument validation, command
# selection, parameter binding and result serialization run unchanged.
$prefix = @'
param([string]$InputPath,[string]$CallPath)
$ErrorActionPreference = 'Stop'
Import-Module Microsoft.PowerShell.Management,Microsoft.PowerShell.Utility
$PSModuleAutoLoadingPreference = 'None'
$a = Get-Content -LiteralPath $InputPath -Raw | ConvertFrom-Json
$script:inventory = @([pscustomobject]@{
    Id=[guid]'25b0d094-7bb4-4d22-8f31-7e7aebbd52ae'; Name='Test VM'
    State=$(if ($a.MockState) {$a.MockState} else {'Off'})
    Generation=2; Version='12.0'; Path='C:\VMs'; CPUUsage=0
    MemoryAssigned=0; Uptime=[timespan]::Zero; CheckpointType='Production'; ReplicationState='Disabled'
})
$script:checkpoint = [pscustomobject]@{Id=[guid]'038acb5a-e5e1-43b9-858e-24f1b47bb323';Name='Test checkpoint';CreationTime=[datetime]'2026-01-01';ParentSnapshotId=$null;SnapshotType='Standard'}
function Import-Module { [CmdletBinding()] param([string]$Name)
    if ($Name -ne 'Hyper-V') { throw "Unexpected module: $Name" }
}
function Record-Call($Name,$Arguments) {
    $record = @{Name=$Name;Parameters=@{}}
    foreach ($key in $Arguments.Keys) {
        $value = $Arguments[$key]
        if ($key -eq 'VM' -or $key -eq 'VMSnapshot') { $value = [string]$value.Id }
        elseif ($value -is [System.Management.Automation.SwitchParameter]) { $value = $value.IsPresent }
        $record.Parameters[$key] = $value
    }
    Add-Content -LiteralPath $CallPath -Value (ConvertTo-Json -InputObject $record -Depth 8 -Compress) -Encoding UTF8
    if ($a.MockFailure) { throw 'Injected Hyper-V operation failure.' }
}
function Get-VM { param([guid]$Id)
    if ($PSBoundParameters.ContainsKey('Id')) { $script:inventory | Where-Object Id -eq $Id }
    else { $script:inventory }
}
function Get-VMProcessor { param($VM) @{Count=2;Maximum=100;Reserve=0;RelativeWeight=100;ExposeVirtualizationExtensions=$false} }
function Get-VMMemory { param($VM) @{Startup=1GB;Minimum=512MB;Maximum=2GB;DynamicMemoryEnabled=$true;Buffer=20;Priority=50} }
function Get-VMSnapshot { param($VM) $script:checkpoint }
function Get-VMHardDiskDrive { param($VM) }
function Get-VMNetworkAdapter { param($VM) }
function Get-VMIntegrationService { param($VM) }
function Get-VMReplication { [CmdletBinding()] param($VM) }
function New-VM { param([string]$Name,[int]$Generation,[long]$MemoryStartupBytes,[switch]$NoVHD,[string]$Path)
    Record-Call 'New-VM' $PSBoundParameters
    $vm = $script:inventory[0].PSObject.Copy()
    $vm.Id = [guid]'9f4d82e6-98d8-4d7e-848b-e343208582e0'; $vm.Name = $Name; $vm.Generation = $Generation
    $script:inventory += $vm
    $vm
}
function Import-VM { [CmdletBinding(SupportsShouldProcess)] param([string]$Path,[switch]$Register,[switch]$Copy,[switch]$GenerateNewId,[string]$VirtualMachinePath,[string]$VhdDestinationPath,[string]$SnapshotFilePath,[string]$SmartPagingFilePath)
    Record-Call 'Import-VM' $PSBoundParameters
    $vm = $script:inventory[0].PSObject.Copy()
    $vm.Id = [guid]'9f4d82e6-98d8-4d7e-848b-e343208582e0'; $vm.Name = 'Imported'
    $script:inventory += $vm
    $vm
}
function Start-VM { param($VM) Record-Call 'Start-VM' $PSBoundParameters }
function Stop-VM { [CmdletBinding(SupportsShouldProcess)] param($VM,[switch]$TurnOff,[switch]$Force) Record-Call 'Stop-VM' $PSBoundParameters }
function Suspend-VM { param($VM) Record-Call 'Suspend-VM' $PSBoundParameters }
function Resume-VM { param($VM) Record-Call 'Resume-VM' $PSBoundParameters }
function Save-VM { param($VM) Record-Call 'Save-VM' $PSBoundParameters }
function Restart-VM { [CmdletBinding(SupportsShouldProcess)] param($VM) Record-Call 'Restart-VM' $PSBoundParameters }
function Rename-VM { param($VM,[string]$NewName) Record-Call 'Rename-VM' $PSBoundParameters; $VM.Name=$NewName }
function Remove-VM { [CmdletBinding(SupportsShouldProcess)] param($VM) Record-Call 'Remove-VM' $PSBoundParameters; $script:inventory=@() }
function Checkpoint-VM { param($VM,[string]$SnapshotName) Record-Call 'Checkpoint-VM' $PSBoundParameters }
function Restore-VMSnapshot { [CmdletBinding(SupportsShouldProcess)] param($VMSnapshot) Record-Call 'Restore-VMSnapshot' $PSBoundParameters }
function Remove-VMSnapshot { [CmdletBinding(SupportsShouldProcess)] param($VMSnapshot) Record-Call 'Remove-VMSnapshot' $PSBoundParameters }
function Export-VM { param($VM,[string]$Path) Record-Call 'Export-VM' $PSBoundParameters }
function Move-VMStorage { [CmdletBinding(SupportsShouldProcess)] param($VM,[string]$DestinationStoragePath) Record-Call 'Move-VMStorage' $PSBoundParameters }
function Set-VMProcessor { [CmdletBinding(SupportsShouldProcess)] param($VM,[int]$Count) Record-Call 'Set-VMProcessor' $PSBoundParameters }
function Set-VMMemory { [CmdletBinding(SupportsShouldProcess)] param($VM,[bool]$DynamicMemoryEnabled,[long]$StartupBytes,[long]$MinimumBytes,[long]$MaximumBytes,[int]$Buffer,[int]$Priority) Record-Call 'Set-VMMemory' $PSBoundParameters }
function Move-VM { [CmdletBinding(SupportsShouldProcess)] param($VM,[string]$DestinationHost,[switch]$IncludeStorage,[string]$DestinationStoragePath) Record-Call 'Move-VM' $PSBoundParameters; $script:inventory=@() }
function Enable-VMReplication { [CmdletBinding(SupportsShouldProcess)] param($VM,[string]$ReplicaServerName,[int]$ReplicaServerPort,[string]$AuthenticationType,[int]$ReplicationFrequencySec,[int]$RecoveryHistory,[bool]$CompressionEnabled,[string]$CertificateThumbprint) Record-Call 'Enable-VMReplication' $PSBoundParameters }
function Start-VMInitialReplication { [CmdletBinding(SupportsShouldProcess)] param($VM) Record-Call 'Start-VMInitialReplication' $PSBoundParameters }
function Suspend-VMReplication { [CmdletBinding(SupportsShouldProcess)] param($VM) Record-Call 'Suspend-VMReplication' $PSBoundParameters }
function Resume-VMReplication { [CmdletBinding(SupportsShouldProcess)] param($VM) Record-Call 'Resume-VMReplication' $PSBoundParameters }
function Remove-VMReplication { [CmdletBinding(SupportsShouldProcess)] param($VM) Record-Call 'Remove-VMReplication' $PSBoundParameters }
'@

$temp = Join-Path ([IO.Path]::GetTempPath()) ('NanaBox-Vmms-' + [guid]::NewGuid())
[void](New-Item -ItemType Directory -Path $temp)
try {
    $child = Join-Path $temp 'Production.ps1'
    [IO.File]::WriteAllText($child, ($prefix + [Environment]::NewLine + $match.Groups[1].Value), [Text.UTF8Encoding]::new($true))
    $vm = '25b0d094-7bb4-4d22-8f31-7e7aebbd52ae'
    $checkpoint = '038acb5a-e5e1-43b9-858e-24f1b47bb323'
    $dest = Join-Path $temp 'destination'
    [void](New-Item -ItemType Directory -Path $dest)
    $configuration = Join-Path $temp 'test.vmcx'
    [IO.File]::WriteAllText($configuration,'test')
    $script:count = 0
    function Test-Command([string]$Action,[hashtable]$Arguments,[string]$Command,[hashtable]$Expected,[bool]$Fails=$false,[bool]$CalledOnFailure=$false) {
        $script:count++
        $inputFile = Join-Path $temp "input-$script:count.json"
        $callFile = Join-Path $temp "calls-$script:count.jsonl"
        $stdout = Join-Path $temp "out-$script:count.txt"
        $stderr = Join-Path $temp "err-$script:count.txt"
        $request = @{Action=$Action;Id=$vm} + $Arguments
        [IO.File]::WriteAllText($inputFile,(ConvertTo-Json -InputObject $request -Depth 8 -Compress),[Text.UTF8Encoding]::new($true))
        $process = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -ArgumentList @('-NoProfile','-NonInteractive','-ExecutionPolicy','Bypass','-File',('"' + $child + '"'),('"' + $inputFile + '"'),('"' + $callFile + '"')) -RedirectStandardOutput $stdout -RedirectStandardError $stderr -Wait -PassThru
        $output = [IO.File]::ReadAllText($stdout)
        $errorText = [IO.File]::ReadAllText($stderr)
        if (($process.ExitCode -ne 0) -ne $Fails) { throw "$Action unexpected exit $($process.ExitCode): $errorText $output" }
        $calls = @()
        if (Test-Path -LiteralPath $callFile) { $calls = @(Get-Content -LiteralPath $callFile | ForEach-Object { ConvertFrom-Json $_ }) }
        if (!$Command -or ($Fails -and !$CalledOnFailure)) {
            if ($calls.Count) { throw "$Action unexpectedly called $($calls[0].Name)" }
        } else {
            if ($calls.Count -ne 1 -or $calls[0].Name -ne $Command) { throw "$Action expected $Command; observed: $($calls.Name -join ', ')" }
            foreach ($key in $Expected.Keys) {
                $property = $calls[0].Parameters.PSObject.Properties[$key]
                if ($null -eq $property -or $property.Value -ne $Expected[$key]) { throw "$Action parameter $key expected $($Expected[$key]); observed $($property.Value)" }
            }
        }
        if (!$Fails) {
            $result = ConvertFrom-Json $output
            if (!$result.Success -or $result.RefreshError) { throw "$Action returned an incomplete result: $output" }
            if ($Action -in @('VmmsRemove','VmmsMigrate') -and ($result.SelectedId -or @($result.Items).Count)) { throw "$Action retained the removed VM." }
            if ($Action -eq 'VmmsMigrate' -and ($result.MigratedTo -ne $Arguments.Host -or $result.MigratedId -ne $vm)) { throw 'Migration result is incorrect.' }
            if ($Action -eq 'VmmsInspect' -and ($result.Details.VM.Id -ne $vm -or $result.Details.Checkpoints[0].Id -ne $checkpoint)) { throw 'Inspection did not return the requested VM and checkpoint.' }
        }
        Write-Host "VMMS $Action passed (expected failure: $Fails)"
    }
    Test-Command 'VmmsList' @{} '' @{}
    Test-Command 'VmmsInspect' @{} '' @{}
    Test-Command 'VmmsCreate' @{Name='New VM';Generation=2;MemoryMiB=1024;Path=$dest} 'New-VM' @{Name='New VM';Generation=2;MemoryStartupBytes=1GB;NoVHD=$true;Path=$dest}
    Test-Command 'VmmsImportRegister' @{Path=$configuration} 'Import-VM' @{Path=$configuration;Register=$true;Confirm=$false}
    Test-Command 'VmmsImportCopy' @{Path=$configuration;Destination=$dest} 'Import-VM' @{Copy=$true;GenerateNewId=$true;VirtualMachinePath=$dest;VhdDestinationPath=$dest;SnapshotFilePath=$dest;SmartPagingFilePath=$dest}
    foreach ($operation in @(
        @('VmmsStart','Start-VM'),@('VmmsShutdown','Stop-VM'),@('VmmsPause','Suspend-VM'),
        @('VmmsResume','Resume-VM'),@('VmmsSave','Save-VM'),@('VmmsRestart','Restart-VM'),
        @('VmmsRemove','Remove-VM'),@('VmmsReplicationStart','Start-VMInitialReplication'),
        @('VmmsReplicationSuspend','Suspend-VMReplication'),@('VmmsReplicationResume','Resume-VMReplication'),
        @('VmmsReplicationRemove','Remove-VMReplication')
    )) { Test-Command $operation[0] @{} $operation[1] @{VM=$vm} }
    Test-Command 'VmmsPowerOff' @{} 'Stop-VM' @{VM=$vm;TurnOff=$true;Force=$true;Confirm=$false}
    Test-Command 'VmmsRename' @{Name='Renamed VM'} 'Rename-VM' @{VM=$vm;NewName='Renamed VM'}
    Test-Command 'VmmsCheckpointCreate' @{Name='Checkpoint'} 'Checkpoint-VM' @{VM=$vm;SnapshotName='Checkpoint'}
    Test-Command 'VmmsCheckpointRestore' @{CheckpointId=$checkpoint} 'Restore-VMSnapshot' @{VMSnapshot=$checkpoint;Confirm=$false}
    Test-Command 'VmmsCheckpointRemove' @{CheckpointId=$checkpoint} 'Remove-VMSnapshot' @{VMSnapshot=$checkpoint;Confirm=$false}
    Test-Command 'VmmsExport' @{Path=$dest} 'Export-VM' @{VM=$vm;Path=$dest}
    Test-Command 'VmmsMoveStorage' @{Path=$dest} 'Move-VMStorage' @{VM=$vm;DestinationStoragePath=$dest}
    Test-Command 'VmmsSetProcessor' @{Count=4} 'Set-VMProcessor' @{VM=$vm;Count=4}
    Test-Command 'VmmsSetMemory' @{MemoryMiB=1024} 'Set-VMMemory' @{VM=$vm;StartupBytes=1GB}
    Test-Command 'VmmsDynamicMemory' @{MinimumMiB=512;MemoryMiB=1024;MaximumMiB=4096;Buffer=20;Priority=50} 'Set-VMMemory' @{VM=$vm;DynamicMemoryEnabled=$true;MinimumBytes=512MB;StartupBytes=1GB;MaximumBytes=4GB;Buffer=20;Priority=50}
    Test-Command 'VmmsStaticMemory' @{MemoryMiB=2048} 'Set-VMMemory' @{VM=$vm;DynamicMemoryEnabled=$false;StartupBytes=2GB}
    Test-Command 'VmmsMigrate' @{Host='host.example.test';Path='D:\VMs'} 'Move-VM' @{VM=$vm;DestinationHost='host.example.test';IncludeStorage=$true;DestinationStoragePath='D:\VMs'}
    Test-Command 'VmmsMigrate' @{Host='host.example.test'} 'Move-VM' @{VM=$vm;DestinationHost='host.example.test'}
    $replica = @{Host='replica.example.test';Port=80;Frequency=300;RecoveryHistory=4;Authentication='Kerberos';Compression=$true}
    Test-Command 'VmmsReplicationEnable' $replica 'Enable-VMReplication' @{VM=$vm;ReplicaServerName=$replica.Host;ReplicaServerPort=80;AuthenticationType='Kerberos';ReplicationFrequencySec=300;RecoveryHistory=4;CompressionEnabled=$true}
    $replica.Authentication='Certificate'; $replica.Port=443; $replica.Thumbprint='a' * 40
    Test-Command 'VmmsReplicationEnable' $replica 'Enable-VMReplication' @{AuthenticationType='Certificate';ReplicaServerPort=443;CertificateThumbprint=$replica.Thumbprint}
    Test-Command 'VmmsCreate' @{Name='Test VM';Generation=2;MemoryMiB=1024} '' @{} $true
    Test-Command 'VmmsRemove' @{MockState='Running'} '' @{} $true
    Test-Command 'VmmsSetProcessor' @{Count=0} '' @{} $true
    Test-Command 'VmmsDynamicMemory' @{MinimumMiB=2048;MemoryMiB=1024;MaximumMiB=4096;Buffer=20;Priority=50} '' @{} $true
    Test-Command 'VmmsMigrate' @{Host='host;invalid'} '' @{} $true
    Test-Command 'VmmsMigrate' @{Host='host.example.test';Path='relative\path'} '' @{} $true
    $replica.Thumbprint='invalid'
    Test-Command 'VmmsReplicationEnable' $replica '' @{} $true
    Test-Command 'VmmsStart' @{MockFailure=$true} 'Start-VM' @{VM=$vm} $true $true
    Write-Host "$script:count VMMS production command cases passed."
} finally { Remove-Item -LiteralPath $temp -Recurse -Force }
