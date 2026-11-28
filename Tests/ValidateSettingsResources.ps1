$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'NanaBox'
[xml]$xaml = [IO.File]::ReadAllText((Join-Path $source 'VirtualMachineSettingsPage.xaml'))
[xml]$en = [IO.File]::ReadAllText((Join-Path $source 'Strings\en\VirtualMachineSettingsPage.resw'))
[xml]$zh = [IO.File]::ReadAllText((Join-Path $source 'Strings\zh-Hans\VirtualMachineSettingsPage.resw'))
$english = @($en.SelectNodes('/root/data') | ForEach-Object { $_.GetAttribute('name') })
$chinese = @($zh.SelectNodes('/root/data') | ForEach-Object { $_.GetAttribute('name') })
$header = [IO.File]::ReadAllText((Join-Path $source 'SettingsText.h'))
$strings = $header.Substring(0,$header.IndexOf('inline constexpr Binding'))
$fallback = @([regex]::Matches($strings,'\{L"([^"]+)", L"') | ForEach-Object { $_.Groups[1].Value })
foreach ($set in @(@{Name='English';Keys=$english},@{Name='Chinese';Keys=$chinese},@{Name='Fallback';Keys=$fallback})) {
    if (@($set.Keys | Group-Object | Where-Object Count -gt 1).Count) { throw "Duplicate $($set.Name) resource key" }
}
if (Compare-Object $english $chinese) { throw 'English and Chinese resource keys differ' }
if (Compare-Object $english $fallback) { throw 'Compiled fallback resource keys differ from RESW' }
$manager = New-Object System.Xml.XmlNamespaceManager($xaml.NameTable)
$manager.AddNamespace('x','http://schemas.microsoft.com/winfx/2006/xaml')
$names = @($xaml.SelectNodes('//*[@x:Name]',$manager) | ForEach-Object { $_.GetAttribute('Name','http://schemas.microsoft.com/winfx/2006/xaml') })
if (@($names | Group-Object | Where-Object Count -gt 1).Count) { throw 'Duplicate XAML element name' }
$bindings = [regex]::Matches($header,'\{L"([^"]+)", L"(Text|Header|Content|AutomationProperties\.Name|AutomationProperties\.HelpText|ToolTipService\.ToolTip)", L"([^"]+)"\},')
foreach ($binding in $bindings) {
    if ($binding.Groups[1].Value -cnotin $names) { throw "Missing XAML element: $($binding.Groups[1].Value)" }
    if ($binding.Groups[3].Value -cnotin $fallback) { throw "Missing resource: $($binding.Groups[3].Value)" }
}
$cpp = [IO.File]::ReadAllText((Join-Path $source 'VirtualMachineSettingsPage.cpp'))
foreach ($match in [regex]::Matches($cpp,'GetSettingsString\(L"([^"]+)"\)')) {
    if ($match.Groups[1].Value -cnotin $fallback) { throw "Untranslated setting: $($match.Groups[1].Value)" }
}
foreach ($node in $xaml.SelectNodes('//*[@Text or @Content or @Header]')) {
    foreach ($property in @('Text','Content','Header')) {
        if ($node.GetAttribute($property) -match '^\[[^\]]+\]$') { throw "Unresolved UI placeholder: $($node.GetAttribute($property))" }
    }
}
Write-Host "Validated $($english.Count) bilingual resource keys and $($bindings.Count) XAML bindings."
