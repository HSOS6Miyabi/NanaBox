param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [string]$OutputDirectory = (Join-Path $env:TEMP ('NanaBox-UI-' + [guid]::NewGuid()))
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
$Executable = (Resolve-Path $Executable).Path
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$config = Join-Path $OutputDirectory 'ui-test.7b'
@{NanaBox=@{Type='VirtualMachine';Version=1;Name='UI automation';GuestType='Windows';ProcessorCount=2;MemorySize=2048;Gpu=@{AssignmentMode='Disabled'}}} |
    ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $config -Encoding UTF8

$process = Start-Process -FilePath $Executable -ArgumentList ('--edit-config "' + $config + '"') -PassThru
$root = [System.Windows.Automation.AutomationElement]::RootElement
$pidCondition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty, [int]$process.Id)
$desc = [System.Windows.Automation.TreeScope]::Descendants
$children = [System.Windows.Automation.TreeScope]::Children
function Find-Control([string]$Id, [switch]$Optional) {
    $until = [DateTime]::UtcNow.AddSeconds(25)
    do {
        if ($process.HasExited) { throw "NanaBox exited: $($process.ExitCode)" }
        $condition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty, $Id)
        foreach ($w in $root.FindAll($children,$pidCondition)) {
            $e = $w.FindFirst($desc,$condition)
            if ($null -ne $e) { return $e }
        }
        if ($Optional) { return $null }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $until)
    throw "UI control unavailable: $Id"
}
function Pattern($element, $pattern) {
    $result = $null
    if (-not $element.TryGetCurrentPattern($pattern,[ref]$result)) {
        throw "Automation pattern unavailable: $($element.Current.AutomationId) / $pattern"
    }
    return $result
}
function Scroll($e) {
    $s = $null
    if ($e.TryGetCurrentPattern([System.Windows.Automation.ScrollItemPattern]::Pattern,[ref]$s)) { $s.ScrollIntoView() }
}
function Invoke-Control([string]$id) {
    $e = Find-Control $id
    Scroll $e
    (Pattern $e ([System.Windows.Automation.InvokePattern]::Pattern)).Invoke()
}
function Set-Text([string]$id,[string]$value) {
    $e = Find-Control $id
    Scroll $e
    (Pattern $e ([System.Windows.Automation.ValuePattern]::Pattern)).SetValue($value)
    if ((Get-Text $id) -cne $value) { throw "Value not accepted: $id" }
}
function Get-Text([string]$id) {
    return (Pattern (Find-Control $id) ([System.Windows.Automation.ValuePattern]::Pattern)).Current.Value
}
function Set-Check([string]$id,[bool]$value) {
    $p = Pattern (Find-Control $id) ([System.Windows.Automation.TogglePattern]::Pattern)
    for ($n=0; $n -lt 3; $n++) {
        if (($p.Current.ToggleState -eq [System.Windows.Automation.ToggleState]::On) -eq $value) { return }
        $p.Toggle()
    }
    throw "Toggle failed: $id"
}
function Select-Tab([string]$id) {
    $e = Find-Control $id
    (Pattern $e ([System.Windows.Automation.SelectionItemPattern]::Pattern)).Select()
}
function Select-Choice([string]$id,[string]$choice) {
    $e = Find-Control $id
    Scroll $e
    $expand = Pattern $e ([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    $expand.Expand()
    $item = Find-Control $choice
    (Pattern $item ([System.Windows.Automation.SelectionItemPattern]::Pattern)).Select()
    if ($expand.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Expanded) { $expand.Collapse() }
}
function Assert-Equal($actual,$expected,[string]$description) {
    if ($actual -ne $expected) { throw "$description : expected [$expected], got [$actual]" }
}
function Assert-Missing([string]$id) {
    if ($null -ne (Find-Control $id -Optional)) { throw "Unexpected control: $id" }
}
function Save-Config {
    $old = (Get-Item -LiteralPath $config).LastWriteTimeUtc
    Invoke-Control 'ApplyButton'
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        if ((Get-Item -LiteralPath $config).LastWriteTimeUtc -gt $old) {
            try { return (Get-Content -LiteralPath $config -Raw | ConvertFrom-Json).NanaBox } catch {}
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw 'Configuration was not saved'
}
function Capture([string]$name) {
    $bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bitmap = New-Object System.Drawing.Bitmap($bounds.Width,$bounds.Height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($bounds.Location,[System.Drawing.Point]::Empty,$bounds.Size)
        $bitmap.Save((Join-Path $OutputDirectory ($name+'.png')))
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
try {
    Select-Tab 'TabGeneral'
    Set-Text 'NameTextBox' 'UI automation saved'
    Set-Text 'ProcessorCountTextBox' '4'
    Set-Text 'MemorySizeTextBox' '4096'
    Set-Check 'SecureBootCheckBox' $true
    Set-Check 'TpmCheckBox' $true
    Set-Check 'VirtualizationExtensionsCheckBox' $true
    Select-Tab 'TabSerial'
    Invoke-Control 'GenerateCom1'
    $pipe1 = Get-Text 'ComPort1TextBox'
    if ($pipe1 -notlike '\\.\pipe\NanaBox-*') { throw "Invalid pipe: $pipe1" }
    Invoke-Control 'GenerateCom2'
    $pipe2 = Get-Text 'ComPort2TextBox'
    if ($pipe2 -notlike '\\.\pipe\NanaBox-*' -or $pipe1 -eq $pipe2) { throw 'COM2 pipe must be unique' }
    Invoke-Control 'ClearCom2'
    Assert-Equal (Get-Text 'ComPort2TextBox') '' 'COM2 disconnected'
    Invoke-Control 'GenerateCom2'
    $pipe2 = Get-Text 'ComPort2TextBox'

    Select-Tab 'TabGpu'
    Invoke-Control 'AddGpuDeviceButton'
    Set-Text 'GpuDeviceDeviceInterfaceLabel' 'UI-test-GPU'
    Set-Text 'GpuDevicePartitionIdLabel' '65535'
    $null = Find-Control 'HostGpuLabel'
    Set-Check 'EnableHostDriverStoreCheckBox' $true

    Select-Tab 'TabNetwork'
    $null = Find-Control 'HostNetworksComboBox'
    $null = Find-Control 'InspectNetwork'
    $null = Find-Control 'DeleteNetwork'
    $null = Find-Control 'CreateNetwork'
    Set-Check 'MirroredNetworkingCheckBox' $true
    Set-Text 'MirroredFirewallCreatorTextBox' '06b34f94-82ae-4ad9-9f35-3a2a7da8508c'
    Set-Text 'MirroredTcpPortsTextBox' '8080'
    Set-Text 'MirroredUdpPortsTextBox' '5353'
    Set-Text 'NetworkNameTextBox' 'UI-test'
    Set-Check 'NetworkFirewallCheckBox' $true
    Invoke-Control 'AddNetworkAdapterButton'
    Set-Check 'NetworkAdapterConnectedLabel' $true
    Set-Text 'NetworkAdapterMacAddressLabel' '00-15-5D-12-34-56'
    Set-Text 'NetworkIdLabel' '85c8668e-36fe-447f-aae0-ba159a0142a7'
    Set-Text 'FirewallCreatorLabel' '06b34f94-82ae-4ad9-9f35-3a2a7da8508c'
    Set-Text 'NetworkAdapterSuggestedSwitchNameLabel' 'UI-test-legacy'
    Set-Text 'NetworkAdapterSuggestedSwitchSubnetLabel' '192.0.2.0/24'
    $null = Find-Control 'HostNetworkLabel'

    Select-Tab 'TabStorage'
    $null = Find-Control 'DiskCreate'
    $null = Find-Control 'DiskResize'
    $null = Find-Control 'DiskCompact'
    Invoke-Control 'AddScsiDeviceButton'
    Set-Text 'ScsiDevicePathLabel' 'test.vhdx'
    $null = Find-Control 'ScsiDeviceTypeLabel'
    $null = Find-Control 'ScsiBrowse'
    Set-Text 'GuestStateFileTextBox' 'test.vmgs'
    Set-Text 'RuntimeStateFileTextBox' 'test.vmrs'

    Select-Tab 'TabSession'
    Set-Check 'RedirectKeyCombinationsCheckBox' $false
    foreach ($id in @('FullScreen','CtrlEsc','AltEsc','AltTab','AltShiftTab','AltSpace','CtrlAltDel','FocusReleaseLeft','FocusReleaseRight')) {
        Set-Text ($id+'HotkeyTextBox') '65'
    }
    foreach ($id in @('Audio','AudioCapture','Drives','Printers','Ports','SmartCards','Clipboard','Devices','POSDevices','DynamicDrives','DynamicDevices')) {
        Set-Check ('Redirect'+$id+'CheckBox') $true
    }

    Select-Tab 'TabAdvanced'
    foreach ($id in @('BaseBoardSerialNumber','ChassisSerialNumber','ChassisAssetTag','Manufacturer','ProductName','ChipsetVersion','SerialNumber','SKUNumber','Family')) {
        Set-Text ($id+'TextBox') ('Test-'+$id)
    }
    Set-Text 'UUIDTextBox' '12345678-1234-1234-1234-123456789abc'
    Set-Text 'HorizontalResolutionTextBox' '1280'
    Set-Text 'VerticalResolutionTextBox' '720'
    Set-Check 'DisableDpiScalingCheckBox' $true
    Set-Text 'PoliciesTextBox' 'test-policy'
    Invoke-Control 'AddPlan9ShareButton'
    Set-Text 'Plan9SharePortLabel' '564'
    Set-Text 'Plan9SharePathLabel' 'C:\Temp'
    Set-Text 'Plan9ShareNameLabel' 'test-share'
    Set-Check 'Plan9ShareReadOnlyLabel' $true
    Invoke-Control 'AddVirtualSmbShareButton'
    Set-Text 'VirtualSmbSharePathLabel' 'C:\Temp'
    Set-Text 'VirtualSmbShareNameLabel' 'test-smb'
    Set-Check 'VirtualSmbShareReadOnlyLabel' $true
    $saved = Save-Config
    Assert-Equal $saved.Name 'UI automation saved' 'Name'
    Assert-Equal $saved.ProcessorCount 4 'CPU'
    Assert-Equal $saved.MemorySize 4096 'Memory'
    Assert-Equal $saved.ComPorts.ComPort1 $pipe1 'COM1'
    Assert-Equal $saved.ComPorts.ComPort2 $pipe2 'COM2'
    Assert-Equal $saved.Gpu.AssignmentMode 'List' 'GPU mode'
    Assert-Equal $saved.Gpu.SelectedDevices[0] 'UI-test-GPU' 'Selected GPU'
    Assert-Equal $saved.MirroredNetworking $true 'Mirrored mode'
    Assert-Equal $saved.MirroredTcpPorts[0] '8080' 'Mirrored TCP port'
    Assert-Equal $saved.MirroredUdpPorts[0] '5353' 'Mirrored UDP port'
    Assert-Equal $saved.NetworkAdapters[0].NetworkId '85c8668e-36fe-447f-aae0-ba159a0142a7' 'Network'
    Assert-Equal $saved.NetworkAdapters[0].FirewallCreatorId '06b34f94-82ae-4ad9-9f35-3a2a7da8508c' 'Firewall'
    Assert-Equal $saved.ScsiDevices[0].Path 'test.vhdx' 'Disk'
    Assert-Equal $saved.Plan9Shares[0].Name 'test-share' 'Plan9'
    Assert-Equal $saved.VirtualSmbShares[0].Name 'test-smb' 'SMB'
    Assert-Equal $saved.VideoMonitor.HorizontalResolution 1280 'Resolution'
    Invoke-Control 'ReloadButton'
    Select-Tab 'TabSerial'
    Assert-Equal (Get-Text 'ComPort1TextBox') $pipe1 'COM1 reloaded'
    Invoke-Control 'ClearCom1'
    Invoke-Control 'ClearCom2'
    Select-Tab 'TabGpu'
    Select-Choice 'AssignmentModeComboBox' 'ChoiceMirrorItem'
    Invoke-Control 'GpuRemove'
    Select-Tab 'TabNetwork'
    Invoke-Control 'NetworkRemove'
    Select-Tab 'TabStorage'
    Invoke-Control 'ScsiRemove'
    Select-Tab 'TabAdvanced'
    Invoke-Control 'Plan9Remove'
    Invoke-Control 'SmbRemove'
    $saved = Save-Config
    Assert-Equal $saved.Gpu.AssignmentMode 'Mirror' 'All GPUs'
    Assert-Equal @($saved.NetworkAdapters | Where-Object {$_}).Count 0 'Network removed'
    Assert-Equal @($saved.ScsiDevices | Where-Object {$_}).Count 0 'Disk removed'
    Assert-Equal @($saved.Plan9Shares | Where-Object {$_}).Count 0 'Share removed'
    Assert-Equal @($saved.VirtualSmbShares | Where-Object {$_}).Count 0 'SMB removed'
    Select-Tab 'TabGeneral'
    Assert-Equal (Get-Text 'NameTextBox') 'UI automation saved' 'Reloaded name'
    Write-Host 'Settings UI round trip passed'
} catch {
    try { Capture 'failure' } catch { Write-Warning $_ }
    throw
} finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
