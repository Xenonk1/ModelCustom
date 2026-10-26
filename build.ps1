param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools\msbuild.ps1')
Invoke-ModelCustomBuild -Project (Join-Path $PSScriptRoot 'ModelCustom.vcxproj') -Configuration $Configuration
Write-Host "Built: $(Join-Path $PSScriptRoot "build\$Configuration\ModelCustom.asi")"
