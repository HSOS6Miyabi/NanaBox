param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [string]$OutputDirectory = (Join-Path $env:TEMP ('NanaBox-Workspace-' + [guid]::NewGuid()))
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ModalUI.ps1')
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Drawing, System.Windows.Forms
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$Executable = (Resolve-Path $Executable).Path
$testId = [guid]::NewGuid().ToString('N')
$key = "HKCU:\Software\NanaBox\WorkspaceHistoryTests\$testId"
New-Item -Path $key -Force | Out-Null
$first = Join-Path $OutputDirectory 'Windows development.7b'
$second = Join-Path $OutputDirectory 'Linux workspace.7b'
$missing = Join-Path $OutputDirectory 'Unavailable machine.7b'
$report = Join-Path $OutputDirectory 'started.txt'
foreach ($entry in @(@($first,'Windows development','Windows'),@($second,'Linux workspace','Linux'))) {
    $configuration = @{NanaBox=@{Type='VirtualMachine';Version=1;Name=$entry[1];GuestType=$entry[2];ProcessorCount=4;MemorySize=4096;Gpu=@{AssignmentMode='Disabled'}}} | ConvertTo-Json -Depth 10
    [IO.File]::WriteAllText($entry[0],$configuration,(New-Object Text.UTF8Encoding($true)))
}
New-ItemProperty -Path $key -Name Paths -PropertyType MultiString -Value @($first,$second,$missing) -Force | Out-Null
function Start-Welcome {
    Start-Process -FilePath $Executable -ArgumentList ('--test-quickstart "'+$testId+'" "'+$report+'"') -PassThru
}
$process = Start-Welcome
$root = [System.Windows.Automation.AutomationElement]::RootElement
$pidCondition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty,[int]$process.Id)
$desc = [System.Windows.Automation.TreeScope]::Descendants
$children = [System.Windows.Automation.TreeScope]::Children
function Find-Control([string]$Id,[switch]$Optional) {
    $until = [DateTime]::UtcNow.AddSeconds(30)
    $condition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty,$Id)
    do {
        if ($process.HasExited) { throw "Quick start exited: $($process.ExitCode)" }
        foreach ($handle in [NanaBoxNativeDialogs]::Windows($process.Id,$false)) {
            if (-not [NanaBoxNativeDialogs]::IsWindowEnabled($handle)) { continue }
            try {
                $w = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
                $e = $w.FindFirst($desc,$condition)
                if ($null -ne $e -and -not $e.Current.IsOffscreen) { return $e }
            } catch [System.Windows.Automation.ElementNotAvailableException] {}
        }
        if ($Optional) { return $null }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $until)
    throw "UI control unavailable: $Id"
}
function Select-Workspace([string]$Id) {
    $item = Find-Control $Id
    $item.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
}
function Search-Workspace([string]$Value) {
    $input = Find-Control 'WorkspaceSearchBox'
    $input.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($Value)
    Start-Sleep -Milliseconds 250
}
function Assert-Workspace([string]$Id,[string]$Name) {
    $entry = Find-Control $Id
    if ($entry.Current.Name -notlike "*$Name*") { throw "Wrong workspace entry: $($entry.Current.Name)" }
}
function Capture-Welcome([string]$Name) {
    Start-Sleep -Milliseconds 400
    $window = $root.FindAll($children,$pidCondition) | Where-Object { $_.Current.NativeWindowHandle -ne 0 -and -not $_.Current.IsOffscreen } | Select-Object -First 1
    $bounds = $window.Current.BoundingRectangle
    $screen = [System.Windows.Forms.SystemInformation]::VirtualScreen
    $area = [System.Drawing.Rectangle]::Intersect($screen, (New-Object System.Drawing.Rectangle([int]$bounds.Left,[int]$bounds.Top,[int]$bounds.Width,[int]$bounds.Height)))
    if ($area.Width -lt 1 -or $area.Height -lt 1) { throw 'Cannot capture welcome window' }
    $bitmap = New-Object System.Drawing.Bitmap($area.Width,$area.Height)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($area.Location,[System.Drawing.Point]::Empty,$area.Size)
        $bitmap.Save((Join-Path $OutputDirectory ($Name+'.png')))
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
function Invoke-Control([string]$Id) {
    Write-Host "Invoke: $Id"
    (Find-Control $Id).GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}
function Find-OwnedWindowContaining([string]$Id) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    $condition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty,$Id)
    do {
        foreach ($handle in [NanaBoxNativeDialogs]::Windows($process.Id,$true)) {
            if (-not [NanaBoxNativeDialogs]::IsWindowEnabled($handle)) { continue }
            try {
                $window = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
                $element = $window.FindFirst($desc,$condition)
                if ($null -ne $element) { return $handle }
            } catch [System.Windows.Automation.ElementNotAvailableException] {}
        }
        Start-Sleep -Milliseconds 150
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Owned window not found: $Id"
}
function Wait-NativeWindowClosed([IntPtr]$handle,[string]$Id) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([NanaBoxNativeDialogs]::IsWindow($handle)) {
        if ([DateTime]::UtcNow -ge $deadline) { throw "Dialog did not close: $Id ($handle)" }
        Start-Sleep -Milliseconds 100
    }
}
function Close-WindowContaining([string]$Id) {
    $handle = Find-OwnedWindowContaining $Id
    if (-not [NanaBoxNativeDialogs]::RequestClose($handle)) { throw "Cannot close owned window: $Id" }
    Wait-NativeWindowClosed $handle $Id
}
try {
    Assert-Workspace 'RecentWorkspace0' 'Windows development'
    Assert-Workspace 'RecentWorkspace1' 'Linux workspace'
    Assert-Workspace 'RecentWorkspace2' 'Unavailable machine'
    Capture-Welcome 'welcome-recent'
    Select-Workspace 'RecentWorkspace2'
    if ((Find-Control 'WorkspaceStartButton').Current.IsEnabled) { throw 'Unavailable workspace must not be startable' }
    if ((Find-Control 'WorkspaceSettingsButton').Current.IsEnabled) { throw 'Unavailable workspace must not be editable' }
    Search-Workspace 'Linux workspace'
    Assert-Workspace 'RecentWorkspace0' 'Linux workspace'
    if (Find-Control 'RecentWorkspace1' -Optional) { throw 'Search did not filter the workspace list' }
    Search-Workspace 'no matching machine'
    $null = Find-Control 'WorkspaceEmptyText'
    Capture-Welcome 'welcome-search-empty'
    Search-Workspace ''
    Select-Workspace 'RecentWorkspace0'
    if (-not (Find-Control 'WorkspaceStartButton').Current.IsEnabled) { throw 'Valid workspace cannot be started' }
    $window = $root.FindAll($children,$pidCondition) | Where-Object { $_.Current.NativeWindowHandle -ne 0 -and -not $_.Current.IsOffscreen } | Select-Object -First 1
    $transform = $window.GetCurrentPattern([System.Windows.Automation.TransformPattern]::Pattern)
    $transform.Resize(680,520)
    Start-Sleep -Milliseconds 350
    $start = Find-Control 'WorkspaceStartButton'
    $remove = Find-Control 'WorkspaceRemoveButton'
    if ($start.Current.IsOffscreen -or $remove.Current.IsOffscreen) { throw 'Workspace actions clipped in a small window' }
    Capture-Welcome 'welcome-small-window'
    $transform.Resize(900,660)
    Start-Sleep -Milliseconds 350
    Invoke-ModalAction $process.Id 'QuickStartLaunchVirtualMachineButton' { Invoke-NativeDialogButton $process.Id 2 }
    Invoke-ModalAction $process.Id 'QuickStartVirtualMachineSettingsButton' { Invoke-NativeDialogButton $process.Id 2 }
    Invoke-Control 'QuickStartCreateVirtualMachineButton'
    Close-WindowContaining 'NameTextBox'
    Invoke-Control 'QuickStartCreateVirtualHardDiskButton'
    $null = Find-Control 'CreateButton'
    Invoke-ModalAction $process.Id 'FileNameBrowseButton' { Invoke-NativeDialogButton $process.Id 2 }
    Invoke-Control 'CreateButton'
    $message = Find-Control 'DiskValidationMessage'
    if ([string]::IsNullOrWhiteSpace($message.Current.Name)) { throw 'Missing disk validation message' }
    $diskWindow = Find-OwnedWindowContaining 'CreateButton'
    Invoke-Control 'CancelButton'
    Wait-NativeWindowClosed $diskWindow 'CreateButton'
    Invoke-Control 'QuickStartResizeVirtualHardDiskButton'
    $null = Find-Control 'ResizeButton'
    Invoke-ModalAction $process.Id 'FileNameBrowseButton' { Invoke-NativeDialogButton $process.Id 2 }
    Invoke-Control 'ResizeButton'
    $message = Find-Control 'DiskValidationMessage'
    if ([string]::IsNullOrWhiteSpace($message.Current.Name)) { throw 'Missing disk validation message' }
    $diskWindow = Find-OwnedWindowContaining 'ResizeButton'
    Invoke-Control 'CancelButton'
    Wait-NativeWindowClosed $diskWindow 'ResizeButton'
    Invoke-ModalAction $process.Id 'QuickStartCompactVirtualHardDiskButton' { Invoke-NativeDialogButton $process.Id 2 }
    Invoke-Control 'QuickStartAboutButton'
    $null = Find-Control 'AboutGitHubButton'
    $aboutWindow = Find-OwnedWindowContaining 'AboutCancelButton'
    Invoke-Control 'AboutCancelButton'
    Wait-NativeWindowClosed $aboutWindow 'AboutCancelButton'
    Select-Workspace 'RecentWorkspace0'
    Invoke-Control 'WorkspaceSettingsButton'
    Close-WindowContaining 'NameTextBox'
    Assert-Workspace 'RecentWorkspace0' 'Windows development'
    Select-Workspace 'RecentWorkspace0'
    Invoke-Control 'WorkspaceRemoveButton'
    Start-Sleep -Milliseconds 500
    Assert-Workspace 'RecentWorkspace0' 'Linux workspace'
    if (-not (Test-Path -LiteralPath $first)) { throw 'Removing a workspace deleted its configuration file' }
    if ((Get-ItemProperty -Path $key -Name Paths).Paths -contains $first) { throw 'Removed workspace remains in persistent history' }
    Select-Workspace 'RecentWorkspace0'
    Invoke-Control 'WorkspaceStartButton'
    if (-not $process.WaitForExit(15000)) { throw 'Starting a workspace did not close the welcome page' }
    if (-not (Test-Path -LiteralPath $report) -or (Get-Content -LiteralPath $report -Raw) -ne $second) { throw 'Incorrect workspace selected for launch' }
    $process = Start-Welcome
    $pidCondition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty,[int]$process.Id)
    Assert-Workspace 'RecentWorkspace0' 'Linux workspace'
    Assert-Workspace 'RecentWorkspace1' 'Unavailable machine'
    Invoke-ModalAction $process.Id 'WorkspaceClearButton' { Invoke-NativeDialogButton $process.Id 6 }
    $null = Find-Control 'WorkspaceEmptyText'
    if (Find-Control 'RecentWorkspace0' -Optional) { throw 'Clearing history left a workspace in the list' }
    if (-not (Test-Path -LiteralPath $second)) { throw 'Clearing history removed a configuration file' }
    Capture-Welcome 'welcome-empty'
    Write-Host 'Verified workspace history, filtering, unavailable files, launch, settings, safe removal and persistent history, and all quick-start actions.'
} finally {
    if (-not $process.HasExited) {
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force }
    }
    Remove-Item -LiteralPath $key -Recurse -Force -ErrorAction SilentlyContinue
}
