$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
function Assert-Parsed([string]$name, [string]$source) {
    $tokens = $null
    $errors = $null
    [void][System.Management.Automation.Language.Parser]::ParseInput($source, [ref]$tokens, [ref]$errors)
    if ($errors.Count) {
        $details = ($errors | ForEach-Object { "line $($_.Extent.StartLineNumber): $($_.Message)" }) -join [Environment]::NewLine
        throw "$name parsing failed: $details"
    }
    Write-Host "Parsed $name"
}
Get-ChildItem -LiteralPath $root -Filter '*.ps1' -File -Recurse | ForEach-Object {
    Assert-Parsed $_.FullName ([IO.File]::ReadAllText($_.FullName))
}
$hostSource = [IO.File]::ReadAllText((Join-Path $root 'NanaBox\HostCompute.cpp'))
$embedded = [regex]::Match($hostSource, 'LR"PS\(([\s\S]*?)\)PS"')
if (-not $embedded.Success) { throw 'Host management script missing' }
Assert-Parsed 'HostCompute ManagementScript' $embedded.Groups[1].Value
$guestSource = [IO.File]::ReadAllText((Join-Path $root 'GuestNetworkService\gns.py'))
$embedded = [regex]::Matches($guestSource, "(?s)^WINDOWS_\w+ = r'''(.*?)'''", [System.Text.RegularExpressions.RegexOptions]::Multiline)
if ($embedded.Count -lt 2) { throw 'Windows guest scripts missing' }
foreach ($script in $embedded) { Assert-Parsed 'Windows guest script' $script.Groups[1].Value }
