# Native dialog discovery must not traverse the blocked XAML automation provider.
if (-not ('NanaBoxNativeDialogs' -as [type])) {
    Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public static class NanaBoxNativeDialogs {
    private delegate bool WindowCallback(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumWindows(WindowCallback callback, IntPtr data);
    [DllImport("user32.dll")] private static extern bool EnumChildWindows(IntPtr window, WindowCallback callback, IntPtr data);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] private static extern int GetClassName(IntPtr window, StringBuilder name, int count);
    [DllImport("user32.dll")] private static extern bool IsWindowVisible(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] private static extern int GetDlgCtrlID(IntPtr window);
    [DllImport("user32.dll")] private static extern IntPtr GetWindow(IntPtr window, uint command);
    [DllImport("user32.dll", SetLastError=true)] private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
    private static string ClassName(IntPtr window) {
        var name = new StringBuilder(256);
        GetClassName(window, name, name.Capacity);
        return name.ToString();
    }
    public static IntPtr[] Windows(int pid, bool ownedOnly) {
        var windows = new List<IntPtr>();
        EnumWindows(delegate(IntPtr window, IntPtr unused) {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            if (owner == (uint)pid && IsWindowVisible(window) &&
                (!ownedOnly || GetWindow(window, 4) != IntPtr.Zero))
                windows.Add(window);
            return true;
        }, IntPtr.Zero);
        return windows.ToArray();
    }
    public static bool RequestClose(IntPtr window) {
        return IsWindow(window) && PostMessage(window, 0x0010, IntPtr.Zero, IntPtr.Zero);
    }
    public static IntPtr ClickDialogButton(int pid, int id) {
        foreach (var window in Windows(pid, true)) {
            if (ClassName(window) != "#32770") continue;
            IntPtr button = IntPtr.Zero;
            EnumChildWindows(window, delegate(IntPtr child, IntPtr unused) {
                if (GetDlgCtrlID(child) == id && IsWindowVisible(child) &&
                    IsWindowEnabled(child) && ClassName(child).Equals("Button", StringComparison.OrdinalIgnoreCase)) {
                    button = child;
                    return false;
                }
                return true;
            }, IntPtr.Zero);
            if (button != IntPtr.Zero && PostMessage(window, 0x0111, new IntPtr(id), button))
                return window;
        }
        return IntPtr.Zero;
    }
}
'@
}
function Invoke-NativeDialogButton([int]$processId,[int]$buttonId) {
    $deadline = [DateTime]::UtcNow.AddSeconds(40)
    do {
        $dialog = [NanaBoxNativeDialogs]::ClickDialogButton($processId,$buttonId)
        if ($dialog -ne [IntPtr]::Zero) {
            while ([NanaBoxNativeDialogs]::IsWindow($dialog) -and [DateTime]::UtcNow -lt $deadline) {
                Start-Sleep -Milliseconds 100
            }
            if ([NanaBoxNativeDialogs]::IsWindow($dialog)) { throw "Native dialog did not close: $buttonId" }
            return
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Native dialog button unavailable: $buttonId (process $processId)"
}
function Invoke-OwnedDialogControl([int]$processId,[string]$id) {
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    $condition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty,$id)
    do {
        foreach ($handle in [NanaBoxNativeDialogs]::Windows($processId,$true)) {
            if (-not [NanaBoxNativeDialogs]::IsWindowEnabled($handle)) { continue }
            try {
                $window = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
                $element = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$condition)
                if ($null -eq $element) { continue }
                $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
                return
            } catch [System.Windows.Automation.ElementNotAvailableException] {}
        }
        Start-Sleep -Milliseconds 200
    } while ([DateTime]::UtcNow -lt $deadline)
    throw "Owned dialog control unavailable: $id"
}

# Invoke a control whose handler waits for a modal dialog, while the caller
# locates and confirms the dialog using UI Automation.
function Invoke-ModalAction([int]$processId,[string]$id,[scriptblock]$Confirm) {
    Write-Host "Invoke modal action: $id"
    $job = Start-Job -ArgumentList $processId,$id -ScriptBlock {
        param([int]$targetProcessId,[string]$automationId)
        $ErrorActionPreference = 'Stop'
        Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
        $root = [System.Windows.Automation.AutomationElement]::RootElement
        $pidCondition = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::ProcessIdProperty, $targetProcessId)
        $idCondition = New-Object System.Windows.Automation.PropertyCondition(
            [System.Windows.Automation.AutomationElement]::AutomationIdProperty, $automationId)
        $deadline = [DateTime]::UtcNow.AddSeconds(20)
        do {
            foreach ($window in $root.FindAll([System.Windows.Automation.TreeScope]::Children,$pidCondition)) {
                $element = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$idCondition)
                if ($null -eq $element) { continue }
                $scroll = $null
                if ($element.TryGetCurrentPattern([System.Windows.Automation.ScrollItemPattern]::Pattern,[ref]$scroll)) {
                    $scroll.ScrollIntoView()
                }
                $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
                return
            }
            Start-Sleep -Milliseconds 200
        } while ([DateTime]::UtcNow -lt $deadline)
        throw "Modal action control unavailable: $automationId"
    }
    try {
        & $Confirm
        if (-not (Wait-Job -Job $job -Timeout 30)) {
            throw "Modal action did not complete after confirmation: $id"
        }
        Receive-Job -Job $job -ErrorAction Stop
        if ($job.State -ne 'Completed') {
            throw "Modal action failed: $id ($($job.State))"
        }
    } finally {
        Remove-Job -Job $job -Force
    }
}
