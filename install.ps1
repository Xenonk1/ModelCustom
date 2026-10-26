param(
    [string]$FiveMApp = "$env:LOCALAPPDATA\FiveM\FiveM.app",
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$source = Join-Path $PSScriptRoot "build\$Configuration\ModelCustom.asi"
if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
    throw "Build the plugin first: .\build.ps1 -Configuration $Configuration"
}
if (-not (Test-Path -LiteralPath $FiveMApp -PathType Container)) {
    throw "FiveM.app was not found at $FiveMApp. Pass its location with -FiveMApp."
}

$plugins = Join-Path $FiveMApp 'plugins'
New-Item -ItemType Directory -Path $plugins -Force | Out-Null
Copy-Item -LiteralPath $source -Destination (Join-Path $plugins 'ModelCustom.asi') -Force

$config = Join-Path $plugins 'ModelCustom.ini'
if (-not (Test-Path -LiteralPath $config)) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'ModelCustom.ini') -Destination $config
}
New-Item -ItemType Directory -Path (Join-Path $plugins 'models') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'third_party\imgui\LICENSE.txt') -Destination (Join-Path $plugins 'ModelCustom-ImGui-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.md') -Destination (Join-Path $plugins 'ModelCustom-THIRD-PARTY-NOTICES.md') -Force
Write-Host "Installed: $plugins"
