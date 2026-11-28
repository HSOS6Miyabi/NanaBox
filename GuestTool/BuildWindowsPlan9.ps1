param(
    [ValidateSet('x64','arm64')][string]$Architecture = 'x64',
    [string]$OutputDirectory = $PSScriptRoot
)
$ErrorActionPreference = 'Stop'
$winfsp = (Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\WOW6432Node\WinFsp' -Name InstallDir -ErrorAction Stop).InstallDir
if (-not (Test-Path -LiteralPath (Join-Path $winfsp 'inc\fuse\fuse.h'))) { throw 'Install the WinFsp Developer SDK.' }
$target = if ($Architecture -eq 'arm64') {'a64'} else {'x64'}
$library = Join-Path $winfsp "lib\winfsp-$target.lib"
if (-not (Test-Path -LiteralPath $library)) { throw "WinFsp SDK does not contain the $Architecture library." }
$vswhere = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC was not found.' }
$vc = Join-Path $vs ('VC\Auxiliary\Build\' + $(if ($Architecture -eq 'x64') {'vcvars64.bat'} else {'vcvarsamd64_arm64.bat'}))
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('NanaBoxPlan9Build-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
Push-Location $temporary
try {
    $inc = Join-Path $winfsp 'inc'
    $fuse = Join-Path $inc 'fuse'
    $src = Join-Path $PSScriptRoot 'WinFspPlan9.cpp'
    $exe = Join-Path $OutputDirectory 'nanabox-plan9-winfsp.exe'
    $cmd = 'call "' + $vc + '" && cl /nologo /EHsc /std:c++17 /W4 /O2 /MT /I"' + $inc + '" /I"' + $fuse + '" "' + $src + '" /Fe:"' + $exe + '" /link "' + $library + '" delayimp.lib /DELAYLOAD:winfsp-' + $target + '.dll'
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { throw 'WinFsp 9P compilation failed.' }
} finally { Pop-Location; Remove-Item -LiteralPath $temporary -Recurse -Force }
