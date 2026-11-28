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
$embedded = [regex]::Matches($hostSource, 'LR"PS\(([\s\S]*?)\)PS"')
if ($embedded.Count -lt 2) { throw 'Host management scripts missing' }
foreach ($script in $embedded) { Assert-Parsed 'HostCompute management script' $script.Groups[1].Value }
$nativeSource = [IO.File]::ReadAllText((Join-Path $root 'GuestTool\WindowsNetworkScripts.h'))
$nativeScripts = [regex]::Matches($nativeSource, 'R"NANABOX\(([\s\S]*?)\)NANABOX"')
if ($nativeScripts.Count -ne 3) { throw 'Native Windows networking scripts missing' }
foreach ($script in $nativeScripts) { Assert-Parsed 'Native Windows guest network script' $script.Groups[1].Value }
