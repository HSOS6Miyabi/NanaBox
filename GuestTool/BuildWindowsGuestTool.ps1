param(
    [ValidateSet('x64','arm64')][string]$Architecture = 'x64',
    [string]$OutputDirectory = $PSScriptRoot,
    [switch]$RunTests
)
$ErrorActionPreference = 'Stop'
if ($RunTests -and $Architecture -ne 'x64') { throw 'Cross-compiled ARM64 binaries must be tested on ARM64 Windows.' }
$vswhere = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC was not found.' }
if (-not $env:VCPKG_INSTALLATION_ROOT) { throw 'Set VCPKG_INSTALLATION_ROOT to the installed vcpkg directory.' }
$vcpkg = Join-Path $env:VCPKG_INSTALLATION_ROOT 'vcpkg.exe'
& $vcpkg install "nlohmann-json:$Architecture-windows"
if ($LASTEXITCODE -ne 0) { throw 'Cannot install the JSON library.' }
$inc = Join-Path $env:VCPKG_INSTALLATION_ROOT "installed\$Architecture-windows\include"
$vc = Join-Path $vs ('VC\Auxiliary\Build\' + $(if ($Architecture -eq 'x64') {'vcvars64.bat'} else {'vcvarsamd64_arm64.bat'}))
if (-not (Test-Path -LiteralPath $vc)) { throw "MSVC target compiler is missing: $Architecture" }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$temporary = Join-Path ([IO.Path]::GetTempPath()) ('NanaBoxBuild-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $temporary | Out-Null
Push-Location $temporary
try {
    foreach ($item in @(@('GuestTool.cpp','nanabox-guest-tool.exe'),@('WddmGuest.cpp','nanabox-gpu.exe'))) {
        $source = Join-Path $PSScriptRoot $item[0]
        $target = Join-Path $OutputDirectory $item[1]
        $cmd = 'call "' + $vc + '" && cl /nologo /EHsc /std:c++17 /W4 /O2 /MT /I"' + $inc + '" "' + $source + '" /Fe:"' + $target + '"'
        cmd /c $cmd
        if ($LASTEXITCODE -ne 0) { throw "GuestTool compilation failed: $($item[0])" }
        if ($RunTests) {
            & $target --self-test
            if ($LASTEXITCODE -ne 0) { throw "GuestTool test failed: $($item[0])" }
        }
    }
} finally { Pop-Location; Remove-Item -LiteralPath $temporary -Recurse -Force }
