param(
    [switch]$Relay,
    [string]$RelayUrl = 'http://127.0.0.1:8787'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'tools\msbuild.ps1')
$project = if ($Relay) { 'tests\RelaySmoke.vcxproj' } else { 'tests\CoreTests.vcxproj' }
$executable = if ($Relay) { 'build\relay-test\RelaySmoke.exe' } else { 'build\core-test\CoreTests.exe' }
Invoke-ModelCustomBuild -Project (Join-Path $PSScriptRoot $project)
if ($Relay) {
    & (Join-Path $PSScriptRoot $executable) $RelayUrl
} else {
    & (Join-Path $PSScriptRoot $executable)
}
if ($LASTEXITCODE -ne 0) {
    throw "Tests failed with exit code $LASTEXITCODE."
}
