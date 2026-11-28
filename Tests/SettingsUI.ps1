param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [string]$OutputDirectory = (Join-Path $env:TEMP ('NanaBox-UI-' + [guid]::NewGuid())),
    [switch]$HostIntegration
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ModalUI.ps1')
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type -AssemblyName System.Drawing, System.Windows.Forms
$Executable = (Resolve-Path $Executable).Path
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$config = Join-Path $OutputDirectory 'ui-test.7b'
$initial = @{NanaBox=@{Type='VirtualMachine';Version=1;Name='UI automation';GuestType='Windows';ProcessorCount=2;MemorySize=2048;Gpu=@{AssignmentMode='Disabled'}}} | ConvertTo-Json -Depth 10
[IO.File]::WriteAllText($config,$initial,(New-Object Text.UTF8Encoding($true)))

$mode = if ($HostIntegration) { '--edit-config' } else { '--edit-config-test' }
$process = Start-Process -FilePath $Executable -ArgumentList ($mode + ' "' + $config + '"') -PassThru
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
    Write-Host "Invoke: $id"
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
    if ($id -in @('NetworkPropertiesTextBox','NetworkDiagnosticsTextBox','HostManagementResult','HcnResultTextBox','DiskResultTextBox','GuestToolsResult','VmmsResultTextBox')) {
        Set-Check ('Details'+$id) $true
    }
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
    Scroll $e
    $expectedHeading = $e.Current.Name
    (Pattern $e ([System.Windows.Automation.SelectionItemPattern]::Pattern)).Select()
    $until = [DateTime]::UtcNow.AddSeconds(8)
    do {
        $heading = Find-Control 'SectionHeading' -Optional
        if ($null -ne $heading -and $heading.Current.Name -eq $expectedHeading) { return }
        Start-Sleep -Milliseconds 80
    } while ([DateTime]::UtcNow -lt $until)
    throw "Selected settings section did not appear: $id"
}
function Select-Choice([string]$id,[string]$choice) {
    $e = Find-Control $id
    Scroll $e
    $expand = Pattern $e ([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    $expand.Expand()
    $item = Find-Control $choice
    Scroll $item
    (Pattern $item ([System.Windows.Automation.SelectionItemPattern]::Pattern)).Select()
    if ($expand.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Expanded) { $expand.Collapse() }
}
function Wait-Result([string]$id,[scriptblock]$predicate,[int]$seconds=90) {
    $until = [DateTime]::UtcNow.AddSeconds($seconds)
    do {
        try {
            $result = Get-Text $id | ConvertFrom-Json
            if ($null -ne $result -and (& $predicate $result)) { return $result }
        } catch {}
        Start-Sleep -Milliseconds 250
    } while ([DateTime]::UtcNow -lt $until)
    throw "Operation did not return the expected result: $id"
}
function Select-ByName([string]$id,[string]$prefix,[switch]$Optional) {
    $e = Find-Control $id
    Scroll $e
    $p = Pattern $e ([System.Windows.Automation.ExpandCollapsePattern]::Pattern)
    try { $p.Expand() } catch { if ($Optional) { return $false }; throw }
    $until = [DateTime]::UtcNow.AddSeconds(15)
    $type = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::ListItem)
    do {
        foreach ($w in $root.FindAll($children,$pidCondition)) {
            foreach ($item in $w.FindAll($desc,$type)) {
                if ($item.Current.AutomationId.StartsWith(($id+'_'),[StringComparison]::Ordinal) -and
                    $item.Current.Name.StartsWith($prefix,[StringComparison]::OrdinalIgnoreCase)) {
                    Scroll $item
                    (Pattern $item ([System.Windows.Automation.SelectionItemPattern]::Pattern)).Select()
                    if ($p.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Expanded) { $p.Collapse() }
                    return $true
                }
            }
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $until)
    if ($p.Current.ExpandCollapseState -eq [System.Windows.Automation.ExpandCollapseState]::Expanded) { $p.Collapse() }
    if ($Optional) { return $false }
    throw "ComboBox item unavailable: $id / $prefix"
}
function Confirm-Change([string]$id) {
    Invoke-ModalAction $process.Id $id { Invoke-NativeDialogButton $process.Id 6 }
}
function Assert-Equal($actual,$expected,[string]$description) {
    if ($actual -ne $expected) { throw "$description : expected [$expected], got [$actual]" }
}
function Assert-Hidden([string]$id) {
    $e = Find-Control $id -Optional
    if ($null -ne $e -and -not $e.Current.IsOffscreen) { throw "Irrelevant field is visible: $id" }
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
    Start-Sleep -Milliseconds 300
    $bounds = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $bitmap = New-Object System.Drawing.Bitmap($bounds.Width,$bounds.Height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($bounds.Location,[System.Drawing.Point]::Empty,$bounds.Size)
        $bitmap.Save((Join-Path $OutputDirectory ($name+'.png')))
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
try {
    $status = Get-Text 'ConfigurationStatusTextBox'
    if ($status -match 'VirtualMachineSettingsPage/|^\[') { throw "Unresolved settings resource: $status" }
    $window = $root.FindAll($children,$pidCondition) | Select-Object -First 1
    $transform = Pattern $window ([System.Windows.Automation.TransformPattern]::Pattern)
    if (-not $transform.Current.CanResize) { throw 'Settings window is not resizable' }
    $transform.Resize(680,520)
    Start-Sleep -Milliseconds 400
    foreach ($section in @('TabGeneral','TabGpu','TabStorage','TabNetwork','TabShares','TabSession','TabSerial','TabGuestTools','TabHostManagement','TabVmms','TabHcn','TabAdvanced')) {
        Select-Tab $section
        $item = Find-Control $section
        $bounds = $item.Current.BoundingRectangle
        $navigationBounds = (Find-Control 'SettingsNavigation').Current.BoundingRectangle
        if ($item.Current.IsOffscreen -or $bounds.Left -lt ($navigationBounds.Left-2) -or $bounds.Right -gt ($navigationBounds.Right+2)) {
            throw "Navigation item is clipped in a small window: $section"
        }
        if ([string]::IsNullOrWhiteSpace($item.Current.Name) -or $item.Current.Name -match 'VirtualMachineSettingsPage/|^\[') { throw "Unresolved navigation label: $section" }
    }
    Select-Tab 'TabNetwork'
    Capture 'network-small-window'
    $transform.Resize(900,700)
    Start-Sleep -Milliseconds 400
    foreach ($section in @('TabGeneral','TabGpu','TabStorage','TabNetwork','TabShares','TabHostManagement')) {
        Select-Tab $section
        Capture $section
    }
    Select-Tab 'TabGeneral'
    foreach ($id in @('ProcessorCountTextBox','MemorySizeTextBox')) {
        $field = Find-Control $id
        if ($field.Current.BoundingRectangle.Width -lt 96) { throw "Numeric field is too narrow: $id" }
    }
    if (-not $HostIntegration) {
        Invoke-Control 'OpenButton'
        $opened = Wait-Result 'ConfigurationStatusTextBox' {param($v) $v.Success -and $v.Operation -eq 'OpenConfiguration'}
        Assert-Equal $opened.Path $config 'Opened configuration path'
        Invoke-Control 'ReferenceButton'
        $null = Wait-Result 'ConfigurationStatusTextBox' {param($v) $v.Success -and $v.Operation -eq 'Reference'}
    }
    Select-Tab 'TabSession'
    Assert-Equal (Find-Control 'ClipboardWindowsUnicodeCheckBox').Current.IsEnabled $true 'Windows Unicode control enabled'
    Set-Check 'ClipboardWindowsUnicodeCheckBox' $true
    $windowsSaved = Save-Config
    Assert-Equal $windowsSaved.Keyboard.ClipboardWindowsUnicode $true 'Windows Unicode saved'
    Invoke-Control 'ReloadButton'
    Select-Tab 'TabSession'
    Assert-Equal (Pattern (Find-Control 'ClipboardWindowsUnicodeCheckBox') ([System.Windows.Automation.TogglePattern]::Pattern)).Current.ToggleState ([System.Windows.Automation.ToggleState]::On) 'Windows Unicode reloaded'
    Select-Tab 'TabGeneral'
    Select-Choice 'GuestTypeComboBox' 'GuestLinux'
    Set-Text 'NameTextBox' 'UI automation saved'
    Set-Text 'ProcessorCountTextBox' '4'
    Set-Text 'MemorySizeTextBox' '4096'
    Set-Text 'ProcessorLimitTextBox' '75'
    Set-Text 'ProcessorWeightTextBox' '250'
    Set-Check 'MemoryOvercommitCheckBox' $true
    Set-Check 'DeferredCommitCheckBox' $true
    Set-Text 'StorageIopsMaximumTextBox' '1000'
    Set-Text 'StorageBandwidthMaximumTextBox' '10485760'
    Set-Check 'SecureBootCheckBox' $true
    Set-Check 'TpmCheckBox' $true
    Set-Check 'VirtualizationExtensionsCheckBox' $true
    Select-Tab 'TabGuestTools'
    if (-not $HostIntegration) {
        Invoke-Control 'GuestHeartbeatButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'Heartbeat' -and $v.Data.Alive}
        Invoke-Control 'GuestPlan9Button'
        $result = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'Plan9Shares'}
        Assert-Equal $result.Data.Shares[0].MountPoint '/media/Projects' 'Plan9 guest mount point'
        Assert-Equal $result.Data.Shares[0].Mounted $true 'Plan9 mount status'
        Set-Text 'GuestPlan9NameTextBox' 'Projects'
        Invoke-Control 'GuestPlan9UnmountButton'
        $result = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'Plan9Unmount'}
        Assert-Equal $result.Data.Shares[0].Mounted $false 'Plan9 unmounted'
        Set-Text 'GuestPlan9PathTextBox' '/mnt/Projects'
        Invoke-Control 'GuestPlan9MountButton'
        $result = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'Plan9Mount'}
        Assert-Equal $result.Data.Shares[0].MountPoint '/mnt/Projects' 'Plan9 custom mount point'
        Assert-Equal $result.Data.Shares[0].Mounted $true 'Plan9 mounted'
        Select-Tab 'TabGeneral'
        Select-Choice 'GuestTypeComboBox' 'GuestWindows'
        Select-Tab 'TabGuestTools'
        Invoke-Control 'GuestPlan9Button'
        $result = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'VirtualSmbShares'}
        Assert-Equal $result.Data.Backend 'VirtualSmb' 'Native Windows share backend'
        Assert-Equal $result.Data.VirtualSmbShares[0].Target 'C:\ProgramData\NanaBox Guest Shares\Projects' 'Windows share mount point'
        Assert-Equal $result.Data.Plan9Enabled $true 'Windows Plan 9 capability'
        Set-Text 'GuestPlan9NameTextBox' 'Projects'
        Invoke-Control 'GuestPlan9UnmountButton'
        $result = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'Plan9Unmount'}
        Assert-Equal $result.Data.Shares[0].Mounted $false 'Windows Plan 9 unmounted'
        Set-Text 'GuestPlan9PathTextBox' 'C:\ProgramData\NanaBox Guest 9P\Custom'
        Invoke-Control 'GuestPlan9MountButton'
        $result = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'Plan9Mount'}
        Assert-Equal $result.Data.Shares[0].MountPoint 'C:\ProgramData\NanaBox Guest 9P\Custom' 'Windows Plan 9 custom mount point'
        Assert-Equal $result.Data.Shares[0].Mounted $true 'Windows Plan 9 mounted'
        Invoke-Control 'GuestVsmbUnmountButton'
        $result = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'VirtualSmbUnmount'}
        Assert-Equal $result.Data.VirtualSmbShares[0].Mounted $false 'VSMB unmounted'
        Assert-Equal $result.Data.VirtualSmbShares[0].Suppressed $true 'VSMB automatic mounting suppressed'
        Invoke-Control 'GuestVsmbMountButton'
        $result = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'VirtualSmbMount'}
        Assert-Equal $result.Data.VirtualSmbShares[0].Mounted $true 'VSMB remounted'
        Select-Tab 'TabGeneral'
        Select-Choice 'GuestTypeComboBox' 'GuestLinux'
        Select-Tab 'TabGuestTools'
        Assert-Missing 'GuestVsmbMountButton'
        Assert-Missing 'GuestVsmbUnmountButton'
        Invoke-Control 'GuestInformationButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'GuestInformation' -and $v.Data.HostName -eq 'mock-guest'}
        Invoke-Control 'GuestDiagnosticsButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) -not $v.Success -and $v.ResourceType -eq 'Diagnostics' -and $v.Error -like '*Injected guest query failure*'}
        Invoke-Control 'GuestHeartbeatButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'Heartbeat' -and $v.Data.Alive}
        Invoke-Control 'GuestClockQueryButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'TimeQuery' -and $v.Data.GuestOffsetMilliseconds -eq 123}
        Invoke-ModalAction $process.Id 'GuestClockSyncButton' { Invoke-NativeDialogButton $process.Id 6 }
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'TimeSync' -and $v.Data.UnixMilliseconds -eq 1800000000123}
        $uploadPath = Join-Path $OutputDirectory 'guest-upload.bin'
        $downloadPath = Join-Path $OutputDirectory 'guest-download.bin'
        [IO.File]::WriteAllBytes($uploadPath, [byte[]](0,255,128,32,0,42))
        Set-Text 'GuestFileNameTextBox' 'exchange.bin'
        Set-Text 'GuestFilePathTextBox' $uploadPath
        Set-Check 'GuestFileOverwriteCheckBox' $false
        Invoke-ModalAction $process.Id 'GuestBrowseUploadButton' { Invoke-NativeDialogButton $process.Id 2 }
        Invoke-Control 'GuestUploadButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'FileUpload' -and $v.Data.Size -eq 6}
        Invoke-Control 'GuestUploadButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -eq $false -and $v.ResourceType -eq 'FileUpload'}
        Set-Text 'GuestFilePathTextBox' $downloadPath
        Invoke-ModalAction $process.Id 'GuestBrowseDownloadButton' { Invoke-NativeDialogButton $process.Id 2 }
        Invoke-Control 'GuestDownloadButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'FileDownload' -and $v.Data.Size -eq 6}
        Assert-Equal (Get-FileHash -LiteralPath $downloadPath).Hash (Get-FileHash -LiteralPath $uploadPath).Hash 'Guest file transfer content'
        Invoke-Control 'GuestDownloadButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -eq $false -and $v.ResourceType -eq 'FileDownload'}
        Set-Check 'GuestFileOverwriteCheckBox' $true
        Invoke-ModalAction $process.Id 'GuestDownloadButton' { Invoke-NativeDialogButton $process.Id 6 }
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -and $v.ResourceType -eq 'FileDownload'}
        Assert-Equal (Get-FileHash -LiteralPath $downloadPath).Hash (Get-FileHash -LiteralPath $uploadPath).Hash 'Guest file replacement'
        Set-Check 'GuestFileOverwriteCheckBox' $false
        Set-Text 'GuestFileNameTextBox' 'cancel-test.bin'
        Set-Text 'GuestFilePathTextBox' $uploadPath
        Invoke-Control 'GuestUploadButton'
        Invoke-Control 'GuestCancelButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -eq $false -and $v.ResourceType -eq 'FileUpload' -and $v.Cancelled}
    } else {
        Invoke-Control 'GuestHeartbeatButton'
        $null = Wait-Result 'GuestToolsResult' {param($v) $v.Success -eq $false -and $v.Error}
    }
    Select-Tab 'TabSerial'
    Invoke-Control 'GenerateCom1'
    $pipe1 = Get-Text 'ComPort1TextBox'
    if ($pipe1 -notlike '\\.\pipe\NanaBox-*') { throw "Invalid pipe: $pipe1" }
    Invoke-Control 'CheckCom1'
    if (-not (Get-Text 'SerialPipeStatusTextBox')) { throw 'COM1 status was not reported' }
    Invoke-Control 'GenerateCom2'
    $pipe2 = Get-Text 'ComPort2TextBox'
    if ($pipe2 -notlike '\\.\pipe\NanaBox-*' -or $pipe1 -eq $pipe2) { throw 'COM2 pipe must be unique' }
    Invoke-Control 'CheckCom2'
    if (-not (Get-Text 'SerialPipeStatusTextBox')) { throw 'COM2 status was not reported' }
    Invoke-Control 'ClearCom2'
    Assert-Equal (Get-Text 'ComPort2TextBox') '' 'COM2 disconnected'
    Invoke-Control 'GenerateCom2'
    $pipe2 = Get-Text 'ComPort2TextBox'

    Select-Tab 'TabGpu'
    Select-Choice 'AssignmentModeComboBox' 'ChoiceListItem'
    Invoke-Control 'AddGpuDeviceButton'
    Set-Check 'GpuAdapterExpertCheckBox' $true
    Set-Text 'GpuDeviceDeviceInterfaceLabel' 'UI-test-GPU'
    Set-Text 'GpuDevicePartitionIdLabel' '65535'
    $null = Find-Control 'HostGpuLabel'
    Set-Check 'EnableHostDriverStoreCheckBox' $true

    Select-Tab 'TabHostManagement'
    $null = Find-Control 'HostNetworksComboBox'
    $null = Find-Control 'InspectNetwork'
    $null = Find-Control 'DeleteNetwork'
    $null = Find-Control 'CreateNetwork'
    $null = Find-Control 'SummaryNetworkPropertiesTextBox'
    if ((Find-Control 'DetailsNetworkPropertiesTextBox').Current.IsOffscreen) { Scroll (Find-Control 'DetailsNetworkPropertiesTextBox') }
    Assert-Equal ((Pattern (Find-Control 'DetailsNetworkPropertiesTextBox') ([System.Windows.Automation.TogglePattern]::Pattern)).Current.ToggleState) ([System.Windows.Automation.ToggleState]::Off) 'Technical details hidden by default'
    Select-Tab 'TabNetwork'
    Invoke-Control 'NetworkPreflightButton'
    $diagnostics = Wait-Result 'NetworkDiagnosticsTextBox' {param($v) $v.Mode -eq 'HostPreflight'}
    if ($null -eq $diagnostics.ApiAvailable -or $null -eq $diagnostics.WslDependencyAvailable -or $null -eq $diagnostics.HcnReachable) { throw 'Incomplete host diagnostic result' }
    Invoke-Control 'NetworkCopyButton'
    if (-not $HostIntegration) {
        Invoke-Control 'NetworkStatusButton'
        $null = Wait-Result 'NetworkDiagnosticsTextBox' {param($v) $v.Success -and $v.Mode -eq 'VmNetwork' -and -not $v.RetryRequested}
        Invoke-Control 'NetworkRetryButton'
        $null = Wait-Result 'NetworkDiagnosticsTextBox' {param($v) $v.Success -and $v.Mode -eq 'VmNetwork' -and $v.RetryRequested}
    } else {
        if ((Find-Control 'NetworkStatusButton').Current.IsEnabled -or (Find-Control 'NetworkRetryButton').Current.IsEnabled) { throw 'Offline network controls must be disabled' }
    }
    Set-Check 'MirroredNetworkingCheckBox' $true
    Set-Check 'MirroredAutoReservePortsCheckBox' $false
    Set-Check 'MirroredDnsTunnelingCheckBox' $true
    Set-Check 'MirroredBindCoordinationCheckBox' $true
    Set-Check 'MirroredLoopbackCheckBox' $true
    Set-Check 'MirroredHostAddressLoopbackCheckBox' $true
    Set-Text 'MirroredFirewallCreatorTextBox' '06b34f94-82ae-4ad9-9f35-3a2a7da8508c'
    Set-Text 'MirroredTcpPortsTextBox' '8080'
    Set-Text 'MirroredUdpPortsTextBox' '5353'
    Set-Text 'MirroredIgnoredPortsTextBox' '53'
    Invoke-Control 'AddNetworkAdapterButton'
    Set-Check 'NetworkAdapterExpertCheckBox' $true
    Set-Check 'NetworkAdapterConnectedLabel' $true
    Set-Text 'NetworkAdapterMacAddressLabel' '00-15-5D-12-34-56'
    Set-Text 'NetworkIdLabel' '85c8668e-36fe-447f-aae0-ba159a0142a7'
    Set-Text 'FirewallCreatorLabel' '06b34f94-82ae-4ad9-9f35-3a2a7da8508c'
    Set-Text 'NetworkAdapterSuggestedSwitchNameLabel' 'UI-test-legacy'
    Set-Text 'NetworkAdapterSuggestedSwitchSubnetLabel' '192.0.2.0/24'
    $null = Find-Control 'HostNetworkLabel'

    Select-Tab 'TabStorage'
    $diskPath = Join-Path $OutputDirectory 'ui-disk.vhdx'
    Set-Text 'DiskPathTextBox' $diskPath
    Set-Text 'DiskSizeTextBox' '32'
    Invoke-Control 'DiskCreate'
    $disk = Wait-Result 'DiskResultTextBox' { param($v) $v.Success -and $v.Operation -eq 'Create' }
    Assert-Equal $disk.VirtualSize (32MB) 'Created VHDX capacity'
    if (-not (Test-Path $diskPath -PathType Leaf)) { throw 'Virtual disk file missing' }
    Set-Text 'DiskSizeTextBox' '64'
    Invoke-Control 'DiskResize'
    $disk = Wait-Result 'DiskResultTextBox' { param($v) $v.Success -and $v.Operation -eq 'Resize' }
    Assert-Equal $disk.VirtualSize (64MB) 'Expanded VHDX capacity'
    Invoke-Control 'DiskCompact'
    $disk = Wait-Result 'DiskResultTextBox' { param($v) $v.Success -and $v.Operation -eq 'Compact' }
    Assert-Equal $disk.VirtualSize (64MB) 'Compacted VHDX capacity'
    Invoke-Control 'DiskInspect'
    $disk = Wait-Result 'DiskResultTextBox' { param($v) $v.Success -and $v.Operation -eq 'Inspect' }
    Assert-Equal $disk.VirtualSize (64MB) 'Inspected VHDX capacity'
    Remove-Item -LiteralPath $diskPath -Force
    $previousDiskPath = Get-Text 'DiskPathTextBox'
    Invoke-ModalAction $process.Id 'DiskBrowse' { Invoke-NativeDialogButton $process.Id 2 }
    Assert-Equal (Get-Text 'DiskPathTextBox') $previousDiskPath 'Canceled disk selection'
    Invoke-Control 'AddScsiDeviceButton'
    Set-Text 'ScsiDevicePathLabel' 'test.vhdx'
    $null = Find-Control 'ScsiDeviceTypeLabel'
    Invoke-ModalAction $process.Id 'ScsiBrowse' { Invoke-NativeDialogButton $process.Id 2 }
    Assert-Equal (Get-Text 'ScsiDevicePathLabel') 'test.vhdx' 'Canceled SCSI selection'
    Set-Text 'GuestStateFileTextBox' 'test.vmgs'
    Set-Text 'RuntimeStateFileTextBox' 'test.vmrs'

    Select-Tab 'TabHostManagement'
    Set-Text 'ManagedSwitchNameTextBox' 'UI-test'
    Select-Choice 'ManagedSwitchTypeComboBox' 'ManagedSwitchTypeComboBoxPrivate'
    Set-Text 'FirewallCreatorNameTextBox' 'UI-test'
    Select-Choice 'FirewallInboundComboBox' 'FirewallInboundComboBoxBlock'
    Select-Choice 'FirewallOutboundComboBox' 'FirewallOutboundComboBoxAllow'
    Set-Check 'FirewallLoopbackCheckBox' $true
    Set-Check 'FirewallMergeCheckBox' $true
    Set-Text 'FirewallRuleNameTextBox' 'UI-test-rule'
    Select-Choice 'FirewallDirectionComboBox' 'FirewallDirectionComboBoxInbound'
    Select-Choice 'FirewallActionComboBox' 'FirewallActionComboBoxAllow'
    Select-Choice 'FirewallProtocolComboBox' 'FirewallProtocolComboBoxTCP'
    Set-Text 'FirewallPortsTextBox' '18080'
    foreach ($id in @('SwitchListButton','SwitchCreateButton','SwitchDeleteButton','AdapterListButton','CreatorListButton','CreatorRegisterButton','FirewallInspectButton','UseCreatorButton','FirewallConfigureButton','RuleCreateButton','RuleEnableButton','RuleDisableButton','RuleDeleteButton')) {
        $null = Find-Control $id
    }
    & {
        $name = 'CI-' + [guid]::NewGuid().ToString('N')
        $managed = 'NanaBox.Managed.' + $name
        $creator = $null
        $networkCreated = $false
        try {
            Set-Text 'ManagedSwitchNameTextBox' $name
            Invoke-Control 'AdapterListButton'
            $null = Wait-Result 'HostManagementResult' {param($v) $v.Success -and $v.Operation -eq 'AdapterList'}
            Invoke-Control 'SwitchListButton'
            $null = Wait-Result 'HostManagementResult' {param($v) $v.Success -and $v.Operation -eq 'SwitchList'}
            Invoke-Control 'SwitchCreateButton'
            $null = Wait-Result 'HostManagementResult' {param($v) $v.Success -and $v.Name -eq $managed}
            if ($HostIntegration) {
                $switch = Get-VMSwitch | Where-Object Name -eq $managed
                if ($null -eq $switch -or [string]$switch.SwitchType -ne 'Private') { throw 'Switch was not created' }
            } else {
                Invoke-Control 'SwitchCreateButton'
                $null = Wait-Result 'HostManagementResult' {param($v) $v.Success -eq $false -and $v.Error -like '*already exists*'}
            }
            $null = Select-ByName 'ManagedSwitchesComboBox' ($managed+' (')
            Confirm-Change 'SwitchDeleteButton'
            $null = Wait-Result 'HostManagementResult' {param($v) $v.Success -and $v.Operation -eq 'SwitchDelete'}
            if ($HostIntegration -and (Get-VMSwitch | Where-Object Name -eq $managed)) { throw 'Switch was not removed' }
            if (Select-ByName 'ManagedSwitchesComboBox' ($managed+' (') -Optional) { throw 'Deleted switch still displayed' }

            Invoke-Control 'CreatorListButton'
            $null = Wait-Result 'HostManagementResult' {param($v) $v.Success -and $v.Operation -eq 'CreatorList'}
            Set-Text 'FirewallCreatorNameTextBox' $name
            Invoke-Control 'CreatorRegisterButton'
            $registered = Wait-Result 'HostManagementResult' {param($v) $v.Name -eq $managed -and $v.Id}
            $creator = [string]$registered.Id
            if ($HostIntegration) {
                $registeredCreator = Get-NetFirewallHyperVVMCreator | Where-Object { [guid]$_.VMCreatorId -eq [guid]$creator }
                if ($null -eq $registeredCreator) { throw 'Firewall creator registration missing' }
            }
            $null = Select-ByName 'FirewallCreatorsComboBox' ($managed+' (')
            Invoke-Control 'FirewallInspectButton'
            $null = Wait-Result 'HostManagementResult' {param($v) $null -ne $v.Settings}
            Invoke-Control 'UseCreatorButton'
            Select-Tab 'TabNetwork'
            Assert-Equal (Get-Text 'MirroredFirewallCreatorTextBox') $creator 'Selected firewall creator'
            Set-Text 'MirroredFirewallCreatorTextBox' '06b34f94-82ae-4ad9-9f35-3a2a7da8508c'
            Select-Tab 'TabHostManagement'
            Select-Choice 'FirewallInboundComboBox' 'FirewallInboundComboBoxAllow'
            Select-Choice 'FirewallOutboundComboBox' 'FirewallOutboundComboBoxBlock'
            Confirm-Change 'FirewallConfigureButton'
            $report = Wait-Result 'HostManagementResult' {param($v) @($v.Settings | Where-Object { $_.DefaultInboundAction -eq 'Allow' -and $_.DefaultOutboundAction -eq 'Block' }).Count -eq 1}
            $setting = $report.Settings[0]
            if ($HostIntegration) { $setting = Get-NetFirewallHyperVVMSetting -Name $creator }
            Assert-Equal ([string]$setting.DefaultInboundAction) 'Allow' 'Firewall inbound default'
            Assert-Equal ([string]$setting.DefaultOutboundAction) 'Block' 'Firewall outbound default'
            Invoke-Control 'RuleCreateButton'
            $report = Wait-Result 'HostManagementResult' {param($v) @($v.Rules | Where-Object Name -eq 'UI-test-rule').Count -eq 1}
            $rule = $report.Rules | Where-Object Name -eq 'UI-test-rule'
            if ($HostIntegration) { $rule = Get-NetFirewallHyperVRule -VMCreatorId $creator | Where-Object DisplayName -eq 'UI-test-rule' }
            if ($null -eq $rule -or $rule.LocalPorts -notcontains '18080') { throw 'Firewall rule missing or incorrect' }
            $null = Select-ByName 'FirewallRulesComboBox' 'UI-test-rule ('
            Invoke-Control 'RuleDisableButton'
            $null = Wait-Result 'HostManagementResult' {param($v) @($v.Rules | Where-Object {$_.Name -eq 'UI-test-rule' -and $_.Enabled -eq 'False'}).Count -eq 1}
            $null = Select-ByName 'FirewallRulesComboBox' 'UI-test-rule ('
            Invoke-Control 'RuleEnableButton'
            $null = Wait-Result 'HostManagementResult' {param($v) @($v.Rules | Where-Object {$_.Name -eq 'UI-test-rule' -and $_.Enabled -eq 'True'}).Count -eq 1}
            $null = Select-ByName 'FirewallRulesComboBox' 'UI-test-rule ('
            Confirm-Change 'RuleDeleteButton'
            $null = Wait-Result 'HostManagementResult' {param($v) $null -ne $v.Rules -and @($v.Rules | Where-Object Name -eq 'UI-test-rule').Count -eq 0}
            if ($HostIntegration -and (Get-NetFirewallHyperVRule -VMCreatorId $creator | Where-Object DisplayName -eq 'UI-test-rule')) { throw 'Firewall rule was not deleted' }

            Select-Tab 'TabHostManagement'
            Set-Text 'NetworkNameTextBox' $name
            Set-Check 'NetworkFirewallCheckBox' $false
            $networkCreated = $true
            Invoke-Control 'CreateNetwork'
            $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Create'}
            $null = Select-ByName 'HostNetworksComboBox' ($managed+' (')
            Invoke-Control 'InspectNetwork'
            $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Inspect' -and $v.Name -eq $managed}
            Confirm-Change 'DeleteNetwork'
            $report = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Delete'}
            Assert-Equal @($report.Networks | Where-Object Name -eq $managed).Count 0 'Deleted network absent from host enumeration'
            Invoke-Control 'RefreshNetworks'
            $report = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Refresh'}
            Assert-Equal @($report.Networks | Where-Object Name -eq $managed).Count 0 'Deleted network absent after refresh'
            if (Select-ByName 'HostNetworksComboBox' ($managed+' (') -Optional) { throw 'Deleted HCN network still appears in the network selector' }
            $networkCreated = $false
            if (-not $HostIntegration) {
                Set-Text 'NetworkNameTextBox' $name
                Select-Choice 'NetworkModeComboBox' 'NetworkModeNat'
                Set-Text 'NetworkNameTextBox' ($name+'-nat')
                Set-Text 'NetworkSubnetTextBox' '192.0.2.1/24'
                Invoke-Control 'CreateNetwork'
                $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) -not $v.Success -and $v.Operation -eq 'Create'}
                Set-Text 'NetworkSubnetTextBox' '192.0.2.0/24'
                Set-Text 'NetworkGatewayTextBox' '192.0.2.1'
                Invoke-Control 'CreateNetwork'
                $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Create'}
                $null = Select-ByName 'HostNetworksComboBox' ($managed+'-nat (')
                Invoke-Control 'InspectNetwork'
                $nat = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Inspect' -and $v.Type -eq 'NAT'}
                Assert-Equal $nat.Ipams[0].Subnets[0].IpAddressPrefix '192.0.2.0/24' 'NAT subnet generated from form'
                Assert-Equal $nat.Ipams[0].Subnets[0].Routes[0].NextHop '192.0.2.1' 'NAT gateway generated from form'
                Confirm-Change 'DeleteNetwork'
                $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Delete'}
                Select-Choice 'NetworkModeComboBox' 'NetworkModeAutomatic'
                Set-Text 'NetworkNameTextBox' $name
                Set-Check 'NetworkExpertCheckBox' $true
                Set-Text 'NetworkAdvancedSettingsTextBox' '{"Type":"NAT","Name":"unauthorized"}'
                Invoke-Control 'CreateNetwork'
                $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -eq $false -and $v.Operation -eq 'Create'}
                Set-Text 'NetworkAdvancedSettingsTextBox' '{"Type":"NAT","Flags":64}'
                Invoke-Control 'CreateNetwork'
                $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -eq $false -and $v.Operation -eq 'Create'}
                Set-Text 'NetworkAdvancedSettingsTextBox' '{"Type":"NAT","SchemaVersion":{"Major":2,"Minor":0},"Subnets":[{"IpAddressPrefix":"192.0.2.0/24"}]}'
                Set-Check 'NetworkFirewallCheckBox' $true
                Invoke-Control 'CreateNetwork'
                $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Create'}
                $null = Select-ByName 'HostNetworksComboBox' ($managed+' (')
                Invoke-Control 'InspectNetwork'
                $created = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Inspect' -and $v.Type -eq 'NAT'}
                if ($created.Subnets[0].IpAddressPrefix -ne '192.0.2.0/24' -or
                    $created.SchemaVersion.Major -ne 2 -or ($created.Flags -band 272) -ne 272) {
                    throw 'Custom HCN network settings were not preserved'
                }
                Confirm-Change 'DeleteNetwork'
                $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Delete'}
            }
        } finally {
            if ($HostIntegration -and $networkCreated -and -not $process.HasExited) {
                try {
                    Select-Tab 'TabHostManagement'
                    Invoke-Control 'RefreshNetworks'
                    $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Refresh'}
                    if (Select-ByName 'HostNetworksComboBox' ($managed+' (') -Optional) {
                        Confirm-Change 'DeleteNetwork'
                        $null = Wait-Result 'NetworkPropertiesTextBox' {param($v) $v.Success -and $v.Operation -eq 'Delete'}
                    }
                } catch { Write-Warning "HCN test network cleanup: $_" }
            }
            if ($HostIntegration) { Get-VMSwitch | Where-Object Name -eq $managed | Remove-VMSwitch -Confirm:$false }
            if ($HostIntegration -and $creator) {
                Get-NetFirewallHyperVRule -VMCreatorId $creator | Where-Object Name -like 'NanaBox.Managed.*' | Remove-NetFirewallHyperVRule -Confirm:$false
                $null = Invoke-CimMethod -Namespace root/standardcimv2 -ClassName MSFT_NetFirewallHyperVVMCreator -MethodName UnregisterHyperVVMCreator -Arguments @{VMCreatorId=$creator}
            }
        }
    }

    if (-not $HostIntegration) {
        Select-Tab 'TabHcn'
        Set-Check 'HcnExpertCheckBox' $false
        Select-Choice 'HcnKindComboBox' 'HcnKindNetwork'
        Set-Text 'HcnNameTextBox' 'Guided network'
        Invoke-Control 'HcnCreate'
        $guidedNetwork = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnCreate'}
        Invoke-Control 'HcnInspect'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnInspect' -and $v.Properties.Type -eq 'ICS'}
        Select-Choice 'HcnKindComboBox' 'HcnKindEndpoint'
        Invoke-Control 'HcnList'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnList' -and $v.Items[0].Kind -eq 'Endpoint'}
        $null = Select-ByName 'HcnParentNetworkComboBox' 'Guided network [managed]'
        Invoke-Control 'HcnCreate'
        $guidedEndpoint = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnCreate' -and $v.Kind -eq 'Endpoint'}
        Select-Choice 'HcnKindComboBox' 'HcnKindNamespace'
        Select-Choice 'HcnNamespaceTypeComboBox' 'HcnNamespaceGuest'
        Invoke-Control 'HcnCreate'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnCreate' -and $v.Kind -eq 'Namespace'}
        Invoke-Control 'HcnInspect'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnInspect' -and $v.Properties.Type -eq 'Guest'}
        Confirm-Change 'HcnDelete'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnDelete'}
        Select-Choice 'HcnKindComboBox' 'HcnKindLoadBalancer'
        Invoke-Control 'HcnList'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnList' -and $v.Items[0].Kind -eq 'LoadBalancer'}
        (Pattern (Find-Control 'HcnEndpointReference') ([System.Windows.Automation.SelectionItemPattern]::Pattern)).Select()
        Set-Text 'HcnFrontendIpTextBox' '192.0.2.100'
        Set-Text 'HcnExternalPortTextBox' '8080'
        Set-Text 'HcnInternalPortTextBox' '80'
        Invoke-Control 'HcnCreate'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnCreate' -and $v.Kind -eq 'LoadBalancer'}
        Invoke-Control 'HcnInspect'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnInspect' -and $v.Properties.PortMappings[0].ExternalPort -eq 8080}
        Confirm-Change 'HcnDelete'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnDelete'}
        Select-Choice 'HcnKindComboBox' 'HcnKindRoute'
        Invoke-Control 'HcnList'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnList' -and $v.Items[0].Kind -eq 'Route'}
        (Pattern (Find-Control 'HcnEndpointReference') ([System.Windows.Automation.SelectionItemPattern]::Pattern)).Select()
        Set-Text 'HcnDestinationTextBox' '192.0.2.0/24'
        Set-Text 'HcnNextHopTextBox' '192.0.2.1'
        Invoke-Control 'HcnCreate'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnCreate' -and $v.Kind -eq 'Route'}
        Invoke-Control 'HcnInspect'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnInspect' -and $v.Properties.Setting[0].DestinationPrefix -eq '192.0.2.0/24'}
        Confirm-Change 'HcnDelete'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnDelete'}
        Select-Choice 'HcnKindComboBox' 'HcnKindEndpoint'
        Invoke-Control 'HcnList'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnList' -and $v.Items[0].Kind -eq 'Endpoint'}
        $null = Select-ByName 'HcnResourceComboBox' ('Endpoint [managed] ('+$guidedEndpoint.Id)
        Confirm-Change 'HcnDelete'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnDelete'}
        Select-Choice 'HcnKindComboBox' 'HcnKindNetwork'
        Invoke-Control 'HcnList'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnList' -and $v.Items[0].Kind -eq 'Network'}
        $null = Select-ByName 'HcnResourceComboBox' ('Guided network [managed] ('+$guidedNetwork.Id)
        Confirm-Change 'HcnDelete'
        $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnDelete'}
        foreach ($kind in @('Network','Endpoint','Namespace','LoadBalancer','Route')) {
            Select-Choice 'HcnKindComboBox' "HcnKind$kind"
            Invoke-Control 'HcnList'
            $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnList'}
            $null = Select-ByName 'HcnResourceComboBox' 'System resource [read-only] ('
            Invoke-Control 'HcnInspect'
            $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnInspect' -and -not $v.Managed}
            Confirm-Change 'HcnDelete'
            $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -eq $false -and $v.Operation -eq 'HcnDelete'}
            Set-Check 'HcnExpertCheckBox' $true
            Set-Text 'HcnNameTextBox' 'Test resource'
            Set-Text 'HcnNetworkIdTextBox' '85c8668e-36fe-447f-aae0-ba159a0142a7'
            $settings = switch ($kind) {
                'Network' { '{"Type":"ICS","SchemaVersion":{"Major":2,"Minor":0}}' }
                'Namespace' { '{"Type":"Host","SchemaVersion":{"Major":2,"Minor":0}}' }
                'LoadBalancer' { '{"HostComputeEndpoints":["85c8668e-36fe-447f-aae0-ba159a0142a7"],"FrontendVIPs":["192.0.2.100"],"PortMappings":[{"Protocol":6,"InternalPort":80,"ExternalPort":80}],"SchemaVersion":{"Major":2,"Minor":0}}' }
                'Route' { '{"HostComputeEndpoints":["85c8668e-36fe-447f-aae0-ba159a0142a7"],"Setting":[{"DestinationPrefix":"192.0.2.0/24","NextHop":"192.0.2.1","NeedEncap":false}],"SchemaVersion":{"Major":2,"Minor":0}}' }
                default { '{"SchemaVersion":{"Major":2,"Minor":0}}' }
            }
            Set-Text 'HcnSettingsTextBox' $settings
            Invoke-Control 'HcnCreate'
            $created = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnCreate'}
            Invoke-Control 'HcnInspect'
            $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnInspect' -and $v.Managed -and $v.Id -eq $created.Id}
            if ($kind -ne 'Route') {
                $modification = switch ($kind) {
                    'Namespace' { '{"ResourceType":"Endpoint","RequestType":"Add","Settings":{"EndpointId":"85c8668e-36fe-447f-aae0-ba159a0142a7"}}' }
                    'LoadBalancer' { '{"HostComputeEndpoints":["85c8668e-36fe-447f-aae0-ba159a0142a7"],"FrontendVIPs":["192.0.2.101"],"PortMappings":[{"Protocol":6,"InternalPort":80,"ExternalPort":80}],"SchemaVersion":{"Major":2,"Minor":0}}' }
                    default { '{"ResourceType":"Policy","RequestType":"Add","Settings":{"Policies":[]}}' }
                }
                Set-Text 'HcnSettingsTextBox' $modification
                Confirm-Change 'HcnModify'
                $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnModify'}
                Invoke-Control 'HcnInspect'
                if ($kind -eq 'LoadBalancer') {
                    $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnInspect' -and $v.Properties.FrontendVIPs[0] -eq '192.0.2.101'}
                } else {
                    $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnInspect' -and $v.Properties.RequestType -eq 'Add'}
                }
            } elseif ((Find-Control 'HcnModify').Current.IsEnabled) { throw 'SDN route modification must be disabled' }
            Confirm-Change 'HcnDelete'
            $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnDelete'}
            Invoke-Control 'HcnList'
            $null = Wait-Result 'HcnResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcnList' -and @($v.Items).Count -eq 1}
        }
        Select-Tab 'TabVmms'
        Invoke-Control 'HcsInventoryButton'
        $null = Wait-Result 'VmmsResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcsInventory' -and $v.Items[0].Id -eq 'mock-hcs'}
        Select-Choice 'HcsQueryModeComboBox' 'HcsQueryBasic'
        Invoke-Control 'HcsServicePropertiesButton'
        $null = Wait-Result 'VmmsResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcsServiceProperties' -and $v.Properties.Query.PropertyTypes[0] -eq 'Basic'}
        $null = Select-ByName 'HcsSystemsComboBox' 'mock-hcs'
        Select-Choice 'HcsQueryModeComboBox' 'HcsQueryStatistics'
        Invoke-Control 'HcsSystemPropertiesButton'
        $null = Wait-Result 'VmmsResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'HcsSystemProperties' -and $v.Properties.Id -eq 'mock-hcs' -and $v.Properties.Query.PropertyTypes[0] -eq 'Statistics'}
        Invoke-Control 'VmmsListButton'
        $null = Wait-Result 'VmmsResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'VmmsList'}
        function Invoke-Vmms([string]$action) {
            Select-Choice 'VmmsActionComboBox' $action
            Confirm-Change 'VmmsExecuteButton'
            $result = Wait-Result 'VmmsResultTextBox' {param($v) $v.Operation -eq $action}
            if (-not $result.Success) { throw "VMMS $action failed: $($result.Error)" }
            return $result
        }
        Select-Choice 'VmmsActionComboBox' 'VmmsCreate'
        Assert-Hidden 'VmmsRemoteHostTextBox'
        Assert-Hidden 'VmmsGpuMinVRAMTextBox'
        Set-Text 'VmmsNameTextBox' 'Mock VM'
        Select-Choice 'VmmsGenerationComboBox' 'VmmsGeneration2'
        Set-Text 'VmmsMemoryTextBox' '1024'
        $created = Invoke-Vmms 'VmmsCreate'
        Assert-Equal $created.Details.VM.Name 'Mock VM' 'Created VM'
        $null = Select-ByName 'VmmsMachinesComboBox' 'Mock VM ('
        Invoke-Control 'VmmsInspectButton'
        $null = Wait-Result 'VmmsResultTextBox' {param($v) $v.Success -and $v.Operation -eq 'VmmsInspect'}
        foreach ($operation in @(@('VmmsStart','Running'),@('VmmsPause','Paused'),@('VmmsResume','Running'),@('VmmsSave','Saved'),@('VmmsStart','Running'),@('VmmsRestart','Running'),@('VmmsShutdown','Off'),@('VmmsStart','Running'),@('VmmsPowerOff','Off'))) {
            $result = Invoke-Vmms $operation[0]
            Assert-Equal $result.Details.VM.State $operation[1] $operation[0]
        }
        Select-Choice 'VmmsActionComboBox' 'VmmsRename'
        Set-Text 'VmmsNameTextBox' 'Renamed VM'
        $result = Invoke-Vmms 'VmmsRename'
        Assert-Equal $result.Details.VM.Name 'Renamed VM' 'VM rename'
        Select-Choice 'VmmsActionComboBox' 'VmmsSetProcessor'
        Set-Text 'VmmsProcessorTextBox' '4'
        $result = Invoke-Vmms 'VmmsSetProcessor'
        Assert-Equal $result.Details.VM.ProcessorCount 4 'VM processor count'
        Select-Choice 'VmmsActionComboBox' 'VmmsDynamicMemory'
        Assert-Hidden 'VmmsGenerationComboBox'
        Assert-Hidden 'VmmsGpuMinVRAMTextBox'
        Set-Text 'VmmsMinimumMemoryTextBox' '512'
        Set-Text 'VmmsMemoryTextBox' '1024'
        Set-Text 'VmmsMaximumMemoryTextBox' '4096'
        Set-Text 'VmmsMemoryBufferTextBox' '25'
        Set-Text 'VmmsMemoryPriorityTextBox' '80'
        $result = Invoke-Vmms 'VmmsDynamicMemory'
        Assert-Equal $result.Details.VM.DynamicMemoryEnabled $true 'Dynamic memory'
        Assert-Equal $result.Details.VM.MinimumMiB 512 'Minimum memory'
        Assert-Equal $result.Details.VM.MaximumMiB 4096 'Maximum memory'
        Assert-Equal $result.Details.VM.Buffer 25 'Memory buffer'
        Assert-Equal $result.Details.VM.Priority 80 'Memory priority'
        $result = Invoke-Vmms 'VmmsStaticMemory'
        Assert-Equal $result.Details.VM.DynamicMemoryEnabled $false 'Static memory'
        Select-Choice 'VmmsActionComboBox' 'VmmsReplicationEnable'
        Set-Text 'VmmsRemoteHostTextBox' 'replica.example.test'
        Set-Text 'VmmsReplicaPortTextBox' '443'
        Set-Text 'VmmsReplicaFrequencyTextBox' '300'
        Set-Text 'VmmsReplicaHistoryTextBox' '4'
        Select-Choice 'VmmsReplicaAuthComboBox' 'VmmsReplicaCertificate'
        Set-Text 'VmmsReplicaCertificateTextBox' ('A' * 40)
        Set-Check 'VmmsReplicaCompressionCheckBox' $false
        $result = Invoke-Vmms 'VmmsReplicationEnable'
        Assert-Equal $result.Details.VM.Replication.Host 'replica.example.test' 'Replica host'
        Assert-Equal $result.Details.VM.Replication.Port 443 'Replica port'
        Assert-Equal $result.Details.VM.Replication.Authentication 'Certificate' 'Replica authentication'
        Assert-Equal $result.Details.VM.Replication.Thumbprint ('A' * 40) 'Certificate'
        Assert-Equal $result.Details.VM.Replication.Frequency 300 'Replica interval'
        Assert-Equal $result.Details.VM.Replication.RecoveryHistory 4 'Replica recovery points'
        Assert-Equal $result.Details.VM.Replication.Compression $false 'Replica compression'
        foreach ($op in @(@('VmmsReplicationStart','Replicating'),@('VmmsReplicationSuspend','Suspended'),@('VmmsReplicationResume','Replicating'))) {
            $result = Invoke-Vmms $op[0]
            Assert-Equal $result.Details.VM.Replication.State $op[1] $op[0]
        }
        $result = Invoke-Vmms 'VmmsReplicationRemove'
        if ($result.Details.VM.Replication) { throw 'Replication relationship still exists' }
        Select-Choice 'VmmsActionComboBox' 'VmmsCpuPolicy'
        Set-Text 'VmmsCpuReserveTextBox' '10'
        Set-Text 'VmmsCpuMaximumTextBox' '75'
        Set-Text 'VmmsCpuWeightTextBox' '200'
        Set-Check 'VmmsCpuNestedCheckBox' $true
        $result = Invoke-Vmms 'VmmsCpuPolicy'
        Assert-Equal $result.Details.VM.CpuPolicy.Maximum 75 'CPU limit'
        Assert-Equal $result.Details.VM.CpuPolicy.Nested $true 'Nested virtualization'
        Select-Choice 'VmmsActionComboBox' 'VmmsGpuAdd'
        $null = Select-ByName 'VmmsGpuSourceComboBox' 'Mock partitionable GPU ('
        Set-Text 'VmmsGpuMinVRAMTextBox' '100'
        Set-Text 'VmmsGpuOptimalVRAMTextBox' '200'
        Set-Text 'VmmsGpuMaxVRAMTextBox' '300'
        Set-Text 'VmmsGpuMinEncodeTextBox' '100'
        Set-Text 'VmmsGpuOptimalEncodeTextBox' '200'
        Set-Text 'VmmsGpuMaxEncodeTextBox' '300'
        Set-Text 'VmmsGpuMinDecodeTextBox' '100'
        Set-Text 'VmmsGpuOptimalDecodeTextBox' '200'
        Set-Text 'VmmsGpuMaxDecodeTextBox' '300'
        Set-Text 'VmmsGpuMinComputeTextBox' '100'
        Set-Text 'VmmsGpuOptimalComputeTextBox' '200'
        Set-Text 'VmmsGpuMaxComputeTextBox' '300'
        $result = Invoke-Vmms 'VmmsGpuAdd'
        $gpu = @($result.Details.Devices | Where-Object Kind -eq Gpu)[0]
        Assert-Equal $gpu.InstancePath 'mock-gpu-path' 'GPU selection'
        Assert-Equal $gpu.Quotas.MinPartitionVRAM '100' 'MinPartitionVRAM'
        Assert-Equal $gpu.Quotas.OptimalPartitionVRAM '200' 'OptimalPartitionVRAM'
        Assert-Equal $gpu.Quotas.MaxPartitionVRAM '300' 'MaxPartitionVRAM'
        Assert-Equal $gpu.Quotas.MinPartitionEncode '100' 'MinPartitionEncode'
        Assert-Equal $gpu.Quotas.OptimalPartitionEncode '200' 'OptimalPartitionEncode'
        Assert-Equal $gpu.Quotas.MaxPartitionEncode '300' 'MaxPartitionEncode'
        Assert-Equal $gpu.Quotas.MinPartitionDecode '100' 'MinPartitionDecode'
        Assert-Equal $gpu.Quotas.OptimalPartitionDecode '200' 'OptimalPartitionDecode'
        Assert-Equal $gpu.Quotas.MaxPartitionDecode '300' 'MaxPartitionDecode'
        Assert-Equal $gpu.Quotas.MinPartitionCompute '100' 'MinPartitionCompute'
        Assert-Equal $gpu.Quotas.OptimalPartitionCompute '200' 'OptimalPartitionCompute'
        Assert-Equal $gpu.Quotas.MaxPartitionCompute '300' 'MaxPartitionCompute'
        Select-Choice 'VmmsActionComboBox' 'VmmsGpuQuotas'
        $null = Select-ByName 'VmmsDevicesComboBox' 'GPU partition adapter ('
        Set-Text 'VmmsGpuMinVRAMTextBox' ''
        Set-Text 'VmmsGpuOptimalVRAMTextBox' '250'
        Set-Text 'VmmsGpuMaxVRAMTextBox' ''
        Set-Text 'VmmsGpuMinEncodeTextBox' ''
        Set-Text 'VmmsGpuOptimalEncodeTextBox' ''
        Set-Text 'VmmsGpuMaxEncodeTextBox' ''
        Set-Text 'VmmsGpuMinDecodeTextBox' ''
        Set-Text 'VmmsGpuOptimalDecodeTextBox' ''
        Set-Text 'VmmsGpuMaxDecodeTextBox' ''
        Set-Text 'VmmsGpuMinComputeTextBox' ''
        Set-Text 'VmmsGpuOptimalComputeTextBox' ''
        Set-Text 'VmmsGpuMaxComputeTextBox' ''
        $result = Invoke-Vmms 'VmmsGpuQuotas'
        $gpu = @($result.Details.Devices | Where-Object Kind -eq Gpu)[0]
        Assert-Equal $gpu.Quotas.OptimalPartitionVRAM '250' 'GPU quota update'
        Assert-Equal $gpu.Quotas.MaxPartitionVRAM '300' 'Preserved GPU quota'
        Select-Choice 'VmmsActionComboBox' 'VmmsGpuRemove'
        $null = Select-ByName 'VmmsDevicesComboBox' 'GPU partition adapter ('
        $result = Invoke-Vmms 'VmmsGpuRemove'
        Assert-Equal @($result.Details.Devices | Where-Object Kind -eq Gpu).Count 0 'GPU partition removed'
        Select-Choice 'VmmsActionComboBox' 'VmmsDiskAttach'
        Set-Text 'VmmsDevicePathTextBox' 'C:\Test.vhdx'
        Set-Text 'VmmsControllerLocationTextBox' '2'
        $result = Invoke-Vmms 'VmmsDiskAttach'
        $disk = @($result.Details.Devices | Where-Object Kind -eq Disk)[0]
        Assert-Equal $disk.Path 'C:\Test.vhdx' 'Attached disk'
        Select-Choice 'VmmsActionComboBox' 'VmmsDiskQoS'
        $null = Select-ByName 'VmmsDevicesComboBox' 'C:\Test.vhdx ('
        Set-Text 'VmmsDiskMinIopsTextBox' '100'
        Set-Text 'VmmsDiskMaxIopsTextBox' '1000'
        $result = Invoke-Vmms 'VmmsDiskQoS'
        Assert-Equal @($result.Details.Devices | Where-Object Kind -eq Disk)[0].MaximumIOPS 1000 'Disk QoS'
        Select-Choice 'VmmsActionComboBox' 'VmmsDiskDetach'
        $null = Select-ByName 'VmmsDevicesComboBox' 'C:\Test.vhdx ('
        $result = Invoke-Vmms 'VmmsDiskDetach'
        Assert-Equal @($result.Details.Devices | Where-Object Kind -eq Disk).Count 0 'Disk detached'
        Select-Choice 'VmmsActionComboBox' 'VmmsDvdAdd'
        Set-Text 'VmmsDevicePathTextBox' 'C:\Test.iso'
        $result = Invoke-Vmms 'VmmsDvdAdd'
        Assert-Equal @($result.Details.Devices | Where-Object Kind -eq Dvd)[0].Path 'C:\Test.iso' 'DVD added'
        Select-Choice 'VmmsActionComboBox' 'VmmsDvdMount'
        $null = Select-ByName 'VmmsDevicesComboBox' 'DVD ('
        Set-Text 'VmmsDevicePathTextBox' ''
        $result = Invoke-Vmms 'VmmsDvdMount'
        Assert-Equal @($result.Details.Devices | Where-Object Kind -eq Dvd)[0].Path '' 'DVD ejected'
        Select-Choice 'VmmsActionComboBox' 'VmmsDvdRemove'
        $null = Select-ByName 'VmmsDevicesComboBox' 'DVD ('
        $result = Invoke-Vmms 'VmmsDvdRemove'
        Assert-Equal @($result.Details.Devices | Where-Object Kind -eq Dvd).Count 0 'DVD removed'
        Select-Choice 'VmmsActionComboBox' 'VmmsNicAdd'
        Set-Text 'VmmsDeviceNameTextBox' 'Test NIC'
        $result = Invoke-Vmms 'VmmsNicAdd'
        Select-Choice 'VmmsActionComboBox' 'VmmsNicConnect'
        $null = Select-ByName 'VmmsDevicesComboBox' 'Test NIC ('
        $null = Select-ByName 'VmmsDeviceSwitchComboBox' 'Test switch ('
        $result = Invoke-Vmms 'VmmsNicConnect'
        Assert-Equal @($result.Details.Devices | Where-Object Kind -eq Nic)[0].SwitchName 'Test switch' 'NIC connected'
        Select-Choice 'VmmsActionComboBox' 'VmmsNicMac'
        $null = Select-ByName 'VmmsDevicesComboBox' 'Test NIC ('
        Set-Text 'VmmsNicMacTextBox' '00155D123456'
        $result = Invoke-Vmms 'VmmsNicMac'
        Assert-Equal @($result.Details.Devices | Where-Object Kind -eq Nic)[0].MacAddress '00155D123456' 'NIC MAC'
        Select-Choice 'VmmsActionComboBox' 'VmmsNicVlan'
        $null = Select-ByName 'VmmsDevicesComboBox' 'Test NIC ('
        Set-Text 'VmmsNicVlanTextBox' '42'
        $result = Invoke-Vmms 'VmmsNicVlan'
        Assert-Equal @($result.Details.Devices | Where-Object Kind -eq Nic)[0].Vlan 42 'NIC VLAN'
        Select-Choice 'VmmsActionComboBox' 'VmmsNicGuard'
        $null = Select-ByName 'VmmsDevicesComboBox' 'Test NIC ('
        Set-Check 'VmmsNicDhcpGuardCheckBox' $true
        Set-Check 'VmmsNicRouterGuardCheckBox' $true
        Set-Check 'VmmsNicSpoofCheckBox' $false
        $result = Invoke-Vmms 'VmmsNicGuard'
        Assert-Equal @($result.Details.Devices | Where-Object Kind -eq Nic)[0].DhcpGuard $true 'NIC DHCP guard'
        Assert-Equal @($result.Details.Devices | Where-Object Kind -eq Nic)[0].RouterGuard $true 'NIC router guard'
        Select-Choice 'VmmsActionComboBox' 'VmmsNicDisconnect'
        $null = Select-ByName 'VmmsDevicesComboBox' 'Test NIC ('
        $result = Invoke-Vmms 'VmmsNicDisconnect'
        Assert-Equal @($result.Details.Devices | Where-Object Kind -eq Nic)[0].SwitchName '' 'NIC disconnected'
        Select-Choice 'VmmsActionComboBox' 'VmmsNicRemove'
        $null = Select-ByName 'VmmsDevicesComboBox' 'Test NIC ('
        $result = Invoke-Vmms 'VmmsNicRemove'
        Assert-Equal @($result.Details.Devices | Where-Object Kind -eq Nic).Count 0 'NIC removed'
        Select-Choice 'VmmsActionComboBox' 'VmmsIntegration'
        $null = Select-ByName 'VmmsDevicesComboBox' 'Heartbeat ('
        Set-Check 'VmmsIntegrationEnabledCheckBox' $false
        $result = Invoke-Vmms 'VmmsIntegration'
        Assert-Equal @($result.Details.Devices | Where-Object Kind -eq Service)[0].Enabled $false 'Integration service disabled'
        Select-Choice 'VmmsActionComboBox' 'VmmsSecureBoot'
        Set-Check 'VmmsSecureBootCheckBox' $true
        Select-Choice 'VmmsSecureBootTemplateComboBox' 'VmmsMicrosoftUEFICertificateAuthority'
        $result = Invoke-Vmms 'VmmsSecureBoot'
        Assert-Equal $result.Details.VM.SecureBoot.Template 'MicrosoftUEFICertificateAuthority' 'Secure Boot template'
        Assert-Equal $result.Details.VM.SecureBoot.Enabled $true 'Secure Boot enabled'
        Select-Choice 'VmmsActionComboBox' 'VmmsAutomaticActions'
        Select-Choice 'VmmsAutomaticStartComboBox' 'VmmsAutomaticStartComboBoxStart'
        Select-Choice 'VmmsAutomaticStopComboBox' 'VmmsAutomaticStopComboBoxShutDown'
        Set-Text 'VmmsAutomaticDelayTextBox' '30'
        $result = Invoke-Vmms 'VmmsAutomaticActions'
        Assert-Equal $result.Details.VM.AutomaticStartAction 'Start' 'Automatic start'
        Assert-Equal $result.Details.VM.AutomaticStartDelay 30 'Start delay'
        Assert-Equal $result.Details.VM.AutomaticStopAction 'ShutDown' 'Automatic stop'
        Select-Choice 'VmmsActionComboBox' 'VmmsCheckpointPolicy'
        Select-Choice 'VmmsCheckpointTypeComboBox' 'VmmsCheckpointTypeComboBoxProductionOnly'
        Set-Check 'VmmsAutomaticCheckpointsCheckBox' $true
        $result = Invoke-Vmms 'VmmsCheckpointPolicy'
        Assert-Equal $result.Details.VM.CheckpointType 'ProductionOnly' 'Checkpoint type'
        Assert-Equal $result.Details.VM.AutomaticCheckpointsEnabled $true 'Automatic checkpoints'
        $result = Invoke-Vmms 'VmmsLocalKeyProtector'
        Assert-Equal $result.Details.VM.HasKeyProtector $true 'Local key protector'
        $result = Invoke-Vmms 'VmmsTpmEnable'
        Assert-Equal $result.Details.VM.TpmEnabled $true 'Virtual TPM enabled'
        $result = Invoke-Vmms 'VmmsTpmDisable'
        Assert-Equal $result.Details.VM.TpmEnabled $false 'Virtual TPM disabled'
        $null = Invoke-Vmms 'VmmsStart'
        $null = Invoke-Vmms 'VmmsSave'
        $result = Invoke-Vmms 'VmmsDeleteSavedState'
        Assert-Equal $result.Details.VM.State 'Off' 'Saved state discarded'
        Select-Choice 'VmmsActionComboBox' 'VmmsCheckpointCreate'
        Set-Text 'VmmsNameTextBox' 'Before changes'
        $result = Invoke-Vmms 'VmmsCheckpointCreate'
        Assert-Equal @($result.Details.Checkpoints).Count 1 'Checkpoint created'
        Select-Choice 'VmmsActionComboBox' 'VmmsCheckpointRename'
        $null = Select-ByName 'VmmsCheckpointsComboBox' 'Before changes ('
        Set-Text 'VmmsNameTextBox' 'Renamed checkpoint'
        $result = Invoke-Vmms 'VmmsCheckpointRename'
        Assert-Equal $result.Details.Checkpoints[0].Name 'Renamed checkpoint' 'Checkpoint renamed'
        Select-Choice 'VmmsActionComboBox' 'VmmsCheckpointExport'
        $null = Select-ByName 'VmmsCheckpointsComboBox' 'Renamed checkpoint ('
        Set-Text 'VmmsPathTextBox' 'C:\Export'
        $result = Invoke-Vmms 'VmmsCheckpointExport'
        Assert-Equal $result.ExportedTo 'C:\Export' 'Checkpoint exported'
        Select-Choice 'VmmsActionComboBox' 'VmmsSetMemory'
        Set-Text 'VmmsMemoryTextBox' '4096'
        $result = Invoke-Vmms 'VmmsSetMemory'
        Assert-Equal $result.Details.VM.MemoryMiB 4096 'VM memory'
        Select-Choice 'VmmsActionComboBox' 'VmmsCheckpointRestore'
        $null = Select-ByName 'VmmsCheckpointsComboBox' 'Renamed checkpoint ('
        $result = Invoke-Vmms 'VmmsCheckpointRestore'
        Assert-Equal $result.Details.VM.MemoryMiB 1024 'Checkpoint restored'
        Select-Choice 'VmmsActionComboBox' 'VmmsCheckpointRemove'
        $null = Select-ByName 'VmmsCheckpointsComboBox' 'Renamed checkpoint ('
        $result = Invoke-Vmms 'VmmsCheckpointRemove'
        Assert-Equal @($result.Details.Checkpoints).Count 0 'Checkpoint removed'
        Select-Choice 'VmmsActionComboBox' 'VmmsExport'
        Set-Text 'VmmsPathTextBox' 'C:\Export'
        $result = Invoke-Vmms 'VmmsExport'
        Assert-Equal $result.ExportedTo 'C:\Export' 'VM export'
        Select-Choice 'VmmsActionComboBox' 'VmmsMoveStorage'
        Set-Text 'VmmsPathTextBox' 'C:\Moved'
        $result = Invoke-Vmms 'VmmsMoveStorage'
        Assert-Equal $result.Details.VM.Path 'C:\Moved' 'VM storage move'
        Select-Choice 'VmmsActionComboBox' 'VmmsMigrate'
        Set-Text 'VmmsRemoteHostTextBox' 'destination.example.test'
        Set-Text 'VmmsRemotePathTextBox' 'D:\VMs\Migrated'
        $result = Invoke-Vmms 'VmmsMigrate'
        Assert-Equal $result.MigratedTo 'destination.example.test' 'Migration destination'
        Assert-Equal $result.MigratedId $created.SelectedId 'Migrated VM'
        Assert-Equal @($result.Items).Count 0 'Migrated VM removed from local inventory'
        Select-Choice 'VmmsActionComboBox' 'VmmsImportRegister'
        Set-Text 'VmmsPathTextBox' 'C:\Export\machine.vmcx'
        $result = Invoke-Vmms 'VmmsImportRegister'
        $firstId = $result.SelectedId
        Select-Choice 'VmmsActionComboBox' 'VmmsImportCopy'
        Set-Text 'VmmsDestinationTextBox' 'C:\Copy'
        $result = Invoke-Vmms 'VmmsImportCopy'
        if ($result.SelectedId -eq $firstId -or @($result.Items).Count -ne 2) { throw 'Imported copy must receive a new VM ID' }
        $null = Invoke-Vmms 'VmmsRemove'
        $null = Select-ByName 'VmmsMachinesComboBox' 'Imported VM ('
        $result = Invoke-Vmms 'VmmsRemove'
        Assert-Equal @($result.Items).Count 0 'Imported VMs removed'
    }
    Select-Tab 'TabSession'
    Invoke-Control 'RefreshDriveButton'
    Invoke-Control 'RefreshDeviceButton'
    Set-Check 'RedirectKeyCombinationsCheckBox' $false
    Select-Choice 'ClipboardKeyboardLayoutComboBox' 'ClipboardKeyboardLayoutComboBox_00000407'
    Assert-Equal (Find-Control 'ClipboardWindowsUnicodeCheckBox').Current.IsEnabled $false 'Windows Unicode control disabled for Linux'
    Set-Check 'ClipboardGtkUnicodeCheckBox' $true
    foreach ($id in @('FullScreen','CtrlEsc','AltEsc','AltTab','AltShiftTab','AltSpace','CtrlAltDel','FocusReleaseLeft','FocusReleaseRight')) {
        Select-Choice ($id+'HotkeyComboBox') ($id+'HotkeyComboBox_65')
    }
    foreach ($id in @('Audio','AudioCapture','Drives','Printers','Ports','SmartCards','Clipboard','Devices','POSDevices','DynamicDrives','DynamicDevices')) {
        Set-Check ('Redirect'+$id+'CheckBox') $true
    }

    Select-Tab 'TabAdvanced'
    Set-Text 'HcsSchemaMinorTextBox' '5'
    Set-Check 'HcsExtensionsExpertCheckBox' $true
    Set-Text 'AdditionalHcsSettingsTextBox' '{"Chipset":{"UseUtc":true}}'
    foreach ($id in @('BaseBoardSerialNumber','ChassisSerialNumber','ChassisAssetTag','Manufacturer','ProductName','ChipsetVersion','SerialNumber','SKUNumber','Family')) {
        Set-Text ($id+'TextBox') ('Test-'+$id)
    }
    Set-Text 'UUIDTextBox' '12345678-1234-1234-1234-123456789abc'
    Set-Text 'HorizontalResolutionTextBox' '1280'
    Set-Text 'VerticalResolutionTextBox' '720'
    Set-Check 'DisableDpiScalingCheckBox' $true
    Set-Check 'DpiOverrideCheckBox' $true
    Set-Text 'DpiScaleTextBox' '175'
    Set-Check 'ContentResizingCheckBox' $false
    Set-Check 'ConnectionBarCheckBox' $false
    Set-Text 'PoliciesTextBox' 'test-policy'
    Select-Tab 'TabShares'
    Invoke-Control 'AddPlan9ShareButton'
    Invoke-ModalAction $process.Id 'Plan9Browse' { Invoke-NativeDialogButton $process.Id 2 }
    Set-Text 'Plan9SharePortLabel' '564'
    Set-Text 'Plan9SharePathLabel' 'C:\Temp'
    Set-Text 'Plan9ShareNameLabel' 'test-share'
    Set-Check 'Plan9ShareReadOnlyLabel' $true
    Set-Check 'Plan9AutoMountLabel' $false
    Set-Check 'Plan9LinuxMetadataLabel' $true
    Set-Check 'Plan9CaseSensitiveLabel' $true
    Set-Check 'Plan9UseShareRootIdentityLabel' $true
    Set-Check 'Plan9AllowOptionsLabel' $true
    Set-Check 'Plan9AllowSubPathsLabel' $true
    Set-Check 'Plan9UnlimitedConnectionsLabel' $true
    Set-Text 'Plan9AllowedFilesLabel' 'example.txt'
    Invoke-Control 'AddVirtualSmbShareButton'
    Set-Text 'VirtualSmbSharePathLabel' 'C:\Temp'
    Set-Text 'VirtualSmbShareNameLabel' 'test-smb'
    Set-Check 'VirtualSmbShareReadOnlyLabel' $true
    Set-Check 'VirtualSmbAutoMountLabel' $false
    Set-Check 'VirtualSmbNoDirectmapLabel' $true
    Set-Check 'VirtualSmbShowOptions' $true
    Select-Choice 'VirtualSmbOptionShareReadLabel' 'VirtualSmbOptionShareReadDisabled'
    Select-Choice 'VirtualSmbOptionCacheIoLabel' 'VirtualSmbOptionCacheIoDisabled'
    Select-Choice 'VirtualSmbOptionNonCacheIoLabel' 'VirtualSmbOptionNonCacheIoEnabled'
    Select-Choice 'VirtualSmbOptionNoOplocksLabel' 'VirtualSmbOptionNoOplocksEnabled'
    Select-Choice 'VirtualSmbOptionForceLevelIIOplocksLabel' 'VirtualSmbOptionForceLevelIIOplocksDisabled'
    Select-Choice 'VirtualSmbOptionPseudoOplocksLabel' 'VirtualSmbOptionPseudoOplocksDisabled'
    Select-Choice 'VirtualSmbOptionNoLocksLabel' 'VirtualSmbOptionNoLocksEnabled'
    Select-Choice 'VirtualSmbOptionNoDirnotifyLabel' 'VirtualSmbOptionNoDirnotifyDisabled'
    Select-Choice 'VirtualSmbOptionPseudoDirnotifyLabel' 'VirtualSmbOptionPseudoDirnotifyEnabled'
    Select-Choice 'VirtualSmbOptionUseShareRootIdentityLabel' 'VirtualSmbOptionUseShareRootIdentityDisabled'
    Select-Choice 'VirtualSmbOptionTakeBackupPrivilegeLabel' 'VirtualSmbOptionTakeBackupPrivilegeDisabled'
    Select-Choice 'VirtualSmbOptionSupportCloudFilesLabel' 'VirtualSmbOptionSupportCloudFilesDisabled'
    $saved = Save-Config
    Assert-Equal $saved.Name 'UI automation saved' 'Name'
    Assert-Equal $saved.HcsSchemaMinor 5 'HCS schema minor version'
    Assert-Equal $saved.AdditionalHcsSettings.Chipset.UseUtc $true 'Advanced HCS properties'
    Assert-Equal $saved.ProcessorCount 4 'CPU'
    Assert-Equal $saved.MemorySize 4096 'Memory'
    Assert-Equal $saved.ProcessorLimit 75000 'CPU limit'
    Assert-Equal $saved.ProcessorWeight 250 'CPU weight'
    Assert-Equal $saved.EnableDeferredCommit $true 'Deferred memory commit'
    Assert-Equal $saved.StorageIopsMaximum 1000 'IOPS limit'
    Assert-Equal $saved.StorageBandwidthMaximum 10485760 'Storage bandwidth limit'
    Assert-Equal $saved.ComPorts.ComPort1 $pipe1 'COM1'
    Assert-Equal $saved.ComPorts.ComPort2 $pipe2 'COM2'
    Assert-Equal $saved.Gpu.AssignmentMode 'List' 'GPU mode'
    Assert-Equal $saved.Gpu.SelectedDevices[0] 'UI-test-GPU' 'Selected GPU'
    Assert-Equal $saved.MirroredNetworking $true 'Mirrored mode'
    Assert-Equal $saved.MirroredAutoReservePorts $false 'Automatic mirrored port reservations'
    Assert-Equal $saved.MirroredDnsTunneling $true 'Mirrored DNS tunneling'
    Assert-Equal $saved.GuestType 'Linux' 'Guest operating system'
    Assert-Equal $saved.MirroredBindCoordination $true 'Synchronous bind coordination'
    Assert-Equal $saved.MirroredLoopback $true 'GELNIC loopback'
    Assert-Equal $saved.MirroredHostAddressLoopback $true 'Host address loopback'
    Assert-Equal $saved.MirroredTcpPorts[0] '8080' 'Mirrored TCP port'
    Assert-Equal $saved.MirroredUdpPorts[0] '5353' 'Mirrored UDP port'
    Assert-Equal $saved.MirroredIgnoredPorts[0] '53' 'Excluded automatic port'
    Assert-Equal $saved.NetworkAdapters[0].NetworkId '85c8668e-36fe-447f-aae0-ba159a0142a7' 'Network'
    Assert-Equal $saved.NetworkAdapters[0].FirewallCreatorId '06b34f94-82ae-4ad9-9f35-3a2a7da8508c' 'Firewall'
    Assert-Equal $saved.ScsiDevices[0].Path 'test.vhdx' 'Disk'
    Assert-Equal $saved.Plan9Shares[0].Name 'test-share' 'Plan9'
    Assert-Equal $saved.Plan9Shares[0].AutoMount $false 'Manual Plan9'
    foreach ($key in @('LinuxMetadata','CaseSensitive','UseShareRootIdentity','AllowOptions','AllowSubPaths','UnlimitedConnections','ReadOnly')) {
        Assert-Equal $saved.Plan9Shares[0].$key $true "Plan9 $key"
    }
    Assert-Equal $saved.Plan9Shares[0].AllowedFiles[0] 'example.txt' 'Plan9 allowed file'
    Assert-Equal $saved.VirtualSmbShares[0].Name 'test-smb' 'SMB'
    Assert-Equal $saved.VirtualSmbShares[0].AutoMount $false 'VSMB manual mounting'
    Assert-Equal $saved.VirtualSmbShares[0].NoDirectmap $true 'VSMB direct mapping disabled'
    Assert-Equal $saved.VirtualSmbShares[0].Options.ShareRead $false 'VSMB ShareRead'
    Assert-Equal $saved.VirtualSmbShares[0].Options.CacheIo $false 'VSMB CacheIo'
    Assert-Equal $saved.VirtualSmbShares[0].Options.NonCacheIo $true 'VSMB NonCacheIo'
    Assert-Equal $saved.VirtualSmbShares[0].Options.NoOplocks $true 'VSMB NoOplocks'
    Assert-Equal $saved.VirtualSmbShares[0].Options.ForceLevelIIOplocks $false 'VSMB ForceLevelIIOplocks'
    Assert-Equal $saved.VirtualSmbShares[0].Options.PseudoOplocks $false 'VSMB PseudoOplocks'
    Assert-Equal $saved.VirtualSmbShares[0].Options.NoLocks $true 'VSMB NoLocks'
    Assert-Equal $saved.VirtualSmbShares[0].Options.NoDirnotify $false 'VSMB NoDirnotify'
    Assert-Equal $saved.VirtualSmbShares[0].Options.PseudoDirnotify $true 'VSMB PseudoDirnotify'
    Assert-Equal $saved.VirtualSmbShares[0].Options.UseShareRootIdentity $false 'VSMB UseShareRootIdentity'
    Assert-Equal $saved.VirtualSmbShares[0].Options.TakeBackupPrivilege $false 'VSMB TakeBackupPrivilege'
    Assert-Equal $saved.VirtualSmbShares[0].Options.SupportCloudFiles $false 'VSMB SupportCloudFiles'
    Assert-Equal $saved.Keyboard.ClipboardKeyboardLayout '00000407' 'Clipboard keyboard layout'
    Assert-Equal $saved.Keyboard.ClipboardGtkUnicode $true 'GTK Unicode input'
    Assert-Equal ([bool]$saved.Keyboard.ClipboardWindowsUnicode) $false 'Windows Unicode disabled for Linux'
    Assert-Equal $saved.VideoMonitor.HorizontalResolution 1280 'Resolution'
    Assert-Equal $saved.VideoMonitor.EnableDpiScalingValueOverride $true 'Display scaling override'
    Assert-Equal $saved.VideoMonitor.OverriddenDpiScalingValue 175 'Display scale'
    Assert-Equal $saved.VideoMonitor.EnableContentResizing $false 'Enhanced-session resizing'
    Assert-Equal $saved.VideoMonitor.ShowFullScreenModeConnectionBar $false 'Full-screen connection bar'
    Invoke-Control 'ReloadButton'
    Select-Tab 'TabSerial'
    Assert-Equal (Get-Text 'ComPort1TextBox') $pipe1 'COM1 reloaded'
    Invoke-Control 'ClearCom1'
    Invoke-Control 'ClearCom2'
    Select-Tab 'TabGpu'
    Invoke-Control 'GpuRemove'
    Select-Choice 'AssignmentModeComboBox' 'ChoiceMirrorItem'
    Select-Tab 'TabNetwork'
    Set-Check 'MirroredBindCoordinationCheckBox' $false
    Set-Check 'MirroredLoopbackCheckBox' $false
    Set-Check 'MirroredHostAddressLoopbackCheckBox' $false
    Invoke-Control 'NetworkRemove'
    Select-Tab 'TabStorage'
    Invoke-Control 'ScsiRemove'
    Select-Tab 'TabShares'
    Invoke-Control 'Plan9Remove'
    Invoke-Control 'SmbRemove'
    $saved = Save-Config
    Assert-Equal $saved.Gpu.AssignmentMode 'Mirror' 'All GPUs'
    Assert-Equal $saved.MirroredBindCoordination $null 'Bind coordination disabled'
    Assert-Equal $saved.MirroredLoopback $null 'Loopback disabled'
    Assert-Equal $saved.MirroredHostAddressLoopback $null 'Host address loopback disabled'
    Assert-Equal @($saved.NetworkAdapters | Where-Object {$_}).Count 0 'Network removed'
    Assert-Equal @($saved.ScsiDevices | Where-Object {$_}).Count 0 'Disk removed'
    Assert-Equal @($saved.Plan9Shares | Where-Object {$_}).Count 0 'Share removed'
    Assert-Equal @($saved.VirtualSmbShares | Where-Object {$_}).Count 0 'SMB removed'
    Select-Tab 'TabGeneral'
    Assert-Equal (Get-Text 'NameTextBox') 'UI automation saved' 'Reloaded name'
    Assert-Equal (Get-Text 'ProcessorLimitTextBox') '75' 'Reloaded CPU limit'
    Assert-Equal (Get-Text 'ProcessorWeightTextBox') '250' 'Reloaded CPU weight'
    Assert-Equal (Get-Text 'StorageIopsMaximumTextBox') '1000' 'Reloaded IOPS limit'
    Assert-Equal (Get-Text 'StorageBandwidthMaximumTextBox') '10485760' 'Reloaded bandwidth limit'
    $external = Get-Content -LiteralPath $config -Raw | ConvertFrom-Json
    $external.NanaBox.Name = 'External edit'
    [IO.File]::WriteAllText($config,($external | ConvertTo-Json -Depth 30),(New-Object Text.UTF8Encoding($true)))
    Set-Text 'NameTextBox' 'Stale edit'
    Invoke-Control 'ApplyButton'
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    do {
        $status = Get-Text 'ConfigurationStatusTextBox'
        if ($status -like '*changed by another editor*') { break }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    if ($status -notlike '*changed by another editor*') { throw "Conflict was not reported: $status" }
    Assert-Equal (Get-Content -LiteralPath $config -Raw | ConvertFrom-Json).NanaBox.Name 'External edit' 'Concurrent change preserved'
    Invoke-Control 'ReloadButton'
    Assert-Equal (Get-Text 'NameTextBox') 'External edit' 'External change reloaded'
    Set-Text 'NameTextBox' 'Recovered edit'
    Invoke-Control 'ValidateConfigurationButton'
    if ((Get-Text 'ConfigurationStatusTextBox') -notlike '*No changes have been saved*') { throw 'Validation result missing' }
    Assert-Equal (Get-Content -LiteralPath $config -Raw | ConvertFrom-Json).NanaBox.Name 'External edit' 'Validation does not change file'
    $saved = Save-Config
    Assert-Equal $saved.Name 'Recovered edit' 'Save after conflict recovery'
    Write-Host 'Settings UI round trip passed'
} catch {
    try { Capture 'failure' } catch { Write-Warning $_ }
    throw
} finally {
    if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
}
