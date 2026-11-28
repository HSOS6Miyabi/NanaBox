param([Parameter(Mandatory=$true)][string]$Executable)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
. (Join-Path $PSScriptRoot 'ModalUI.ps1')
$exe = (Resolve-Path -LiteralPath $Executable).Path
$directory = Join-Path $env:TEMP ('NanaBox-Dialogs-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $directory | Out-Null
function Invoke-TestDialog([string]$kind,[string[]]$controls,[string[]]$expected,[switch]$CloseWindow) {
    $log = Join-Path $directory ([guid]::NewGuid().ToString() + '.txt')
    $process = Start-Process -FilePath $exe -ArgumentList ('--test-dialog ' + $kind + ' "' + $log + '"') -PassThru
    try {
        foreach ($id in $controls) {
            $until = [DateTime]::UtcNow.AddSeconds(30)
            $condition = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::AutomationIdProperty,$id)
            $invoked = $false
            do {
                if ($process.HasExited) { throw "Dialog exited before $id : $($process.ExitCode)" }
                foreach ($handle in [NanaBoxNativeDialogs]::Windows($process.Id,$false)) {
                    try {
                        $window = [System.Windows.Automation.AutomationElement]::FromHandle($handle)
                        $element = $window.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$condition)
                        if ($null -eq $element -or -not $element.Current.IsEnabled -or $element.Current.IsOffscreen) { continue }
                        $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
                        $invoked = $true
                        break
                    } catch [System.Windows.Automation.ElementNotAvailableException] {}
                }
                if (-not $invoked) { Start-Sleep -Milliseconds 100 }
            } while (-not $invoked -and [DateTime]::UtcNow -lt $until)
            if (-not $invoked) { throw "Dialog control unavailable: $id" }
            Write-Host "Verified control: $id"
        }
        if ($CloseWindow) {
            # Wait for all external actions to finish before closing the sponsor dialog.
            $until = [DateTime]::UtcNow.AddSeconds(10)
            do {
                $lines = @(Get-Content -LiteralPath $log -ErrorAction SilentlyContinue | Where-Object {$_ -ne ''})
                if ($lines.Count -eq $expected.Count) { break }
                Start-Sleep -Milliseconds 100
            } while ([DateTime]::UtcNow -lt $until)
            foreach ($handle in [NanaBoxNativeDialogs]::Windows($process.Id,$false)) {
                [void][NanaBoxNativeDialogs]::RequestClose($handle)
            }
        }
        if (-not $process.WaitForExit(15000)) { throw "Dialog did not close: $kind" }
        if ($process.ExitCode -ne 0) { throw "Dialog returned $($process.ExitCode): $kind" }
        $actual = @(Get-Content -LiteralPath $log | Where-Object {$_ -ne ''})
        if (($actual -join '|') -cne ($expected -join '|')) { throw "Unexpected dialog result $kind : $($actual -join ', ')" }
    } finally {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
    }
}
try {
    Invoke-TestDialog 'exit' @('ExitSuspendButton') @('1')
    Invoke-TestDialog 'exit' @('ExitPowerOffButton') @('2')
    Invoke-TestDialog 'exit' @('ExitCancelButton') @('0')
    Invoke-TestDialog 'about' @('AboutGitHubButton','AboutCancelButton') @('https://github.com/M2Team/NanaBox')
    Invoke-TestDialog 'sponsor' @('ContributeButton','BuySponsorEditionButton','SponsorEditionPolicyButton') @(
        'https://github.com/M2Team/NanaBox/blob/main/CONTRIBUTING.md',
        'ms-store-product:9P3KMWM424WK',
        'https://github.com/M2Team/NanaBox/blob/main/Documents/SponsorEdition.md') -CloseWindow
    Invoke-TestDialog 'message' @('MessageConfirmButton') @()
    Write-Host 'All dialog button actions verified.'
} finally { Remove-Item -LiteralPath $directory -Recurse -Force }
