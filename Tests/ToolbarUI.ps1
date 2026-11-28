param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [string]$OutputDirectory = (Join-Path $env:TEMP ('NanaBox-Toolbar-' + [guid]::NewGuid()))
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ToolbarMouse {
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint flags, uint x, uint y, uint data, UIntPtr extra);
}
'@
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$log = Join-Path $OutputDirectory 'toolbar-commands.txt'
$exe = (Resolve-Path -LiteralPath $Executable).Path
$process = Start-Process -FilePath $exe -ArgumentList ('--test-toolbar "' + $log + '"') -PassThru
$root = [System.Windows.Automation.AutomationElement]::RootElement
$desc = [System.Windows.Automation.TreeScope]::Descendants
$children = [System.Windows.Automation.TreeScope]::Children
$pidCondition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty,[int]$process.Id)
$expected = New-Object 'System.Collections.Generic.List[int]'
function Find-Control([string]$id) {
    $deadline = [DateTime]::UtcNow.AddSeconds(25)
    $condition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty,$id)
    do {
        if ($process.HasExited) { throw "Toolbar process exited: $($process.ExitCode)" }
        foreach ($window in $root.FindAll($children,$pidCondition)) {
            foreach ($element in $window.FindAll($desc,$condition)) {
                if ($element.Current.IsEnabled -and -not $element.Current.IsOffscreen) { return $element }
            }
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Toolbar control unavailable: $id"
}
function Click-Control([string]$id,[switch]$Physical) {
    $element = Find-Control $id
    Write-Host "Click: $id"
    $invoke = $null
    if (-not $Physical -and $element.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern,[ref]$invoke)) {
        $invoke.Invoke()
        return
    }
    # TogglePattern may change IsChecked without raising the Click event.
    # Use a real pointer click for toggle buttons.
    $point = $element.GetClickablePoint()
    if (-not [ToolbarMouse]::SetCursorPos([int]$point.X,[int]$point.Y)) { throw "Unable to position pointer: $id" }
    [ToolbarMouse]::mouse_event(2,0,0,0,[UIntPtr]::Zero)
    [ToolbarMouse]::mouse_event(4,0,0,0,[UIntPtr]::Zero)
}
function Read-CommandLog {
    $stream = [IO.File]::Open($log,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader = New-Object IO.StreamReader($stream)
    try { $reader.ReadToEnd() -split '\r?\n' | Where-Object { $_ -ne '' } }
    finally { $reader.Dispose() }
}
function Assert-Command([string]$id,[int]$command,[switch]$Physical) {
    $expected.Add($command)
    Click-Control $id -Physical:$Physical
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    do {
        try { $actual = @(Read-CommandLog) } catch [IO.IOException] { Start-Sleep -Milliseconds 100; continue }
        if ($actual.Count -gt $expected.Count) { throw "Unexpected duplicate command from $id" }
        if ($actual.Count -eq $expected.Count) {
            if (($actual -join ',') -ne ($expected -join ',')) {
                throw "Incorrect command after $id : $($actual -join ',')"
            }
            return
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "No command from $id"
}
try {
    Assert-Command 'EnhancedSessionButton' 0x1002 -Physical
    Assert-Command 'EnhancedSessionButton' 0x1001 -Physical
    Assert-Command 'FullScreenButton' 0x1003
    Assert-Command 'PauseVirtualMachineButton' 0x1004 -Physical
    Assert-Command 'PauseVirtualMachineButton' 0x1005 -Physical
    Assert-Command 'RestartVirtualMachineButton' 0x1006
    Click-Control 'PowerMenu' -Physical
    Assert-Command 'ShutdownAction' 0x1011
    Click-Control 'PowerMenu' -Physical
    Assert-Command 'SaveAction' 0x1013
    Click-Control 'PowerMenu' -Physical
    Assert-Command 'PowerOffAction' 0x1012
    Assert-Command 'VirtualMachineSettingsButton' 0x1007
    Assert-Command 'RedirectKeyboardNavigationButton' 0x1008 -Physical
    Assert-Command 'RedirectKeyboardNavigationButton' 0x1009 -Physical
    Assert-Command 'RestoreVirtualMachineWindowSizeButton' 0x100A
    Assert-Command 'CopyScreenshotButton' 0x100B
    Assert-Command 'RecordVideoButton' 0x100C -Physical
    Assert-Command 'RecordVideoButton' 0x100C -Physical
    Assert-Command 'CreateVirtualHardDiskButton' 0x100D
    Assert-Command 'ResizeVirtualHardDiskButton' 0x100E
    Assert-Command 'CompactVirtualHardDiskButton' 0x100F
    Assert-Command 'AboutButton' 0x1010
    Assert-Command 'SponsorButton' 0x1014
    Start-Sleep -Milliseconds 300
    $actual = @(Read-CommandLog)
    if (($actual -join ',') -ne ($expected -join ',')) { throw 'Unexpected trailing command' }
    Write-Host "Verified $($expected.Count) toolbar commands."
} finally {
    if (-not $process.HasExited) {
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(5000)) { Stop-Process -Id $process.Id -Force }
    }
}
