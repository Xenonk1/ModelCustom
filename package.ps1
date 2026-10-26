param(
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.-]+)?$')]
    [string]$Version = '1.0.0'
)

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1') -Configuration Release

$dist = Join-Path $PSScriptRoot 'dist'
New-Item -ItemType Directory -Path $dist -Force | Out-Null
$staging = Join-Path $dist ('.stage-' + [Guid]::NewGuid().ToString('N'))
$plugins = Join-Path $staging 'plugins'
$models = Join-Path $plugins 'models'
New-Item -ItemType Directory -Path $models -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'build\Release\ModelCustom.asi') -Destination $plugins
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'ModelCustom.ini') -Destination $plugins
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'models\README.md') -Destination $models
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'third_party\imgui\LICENSE.txt') -Destination (Join-Path $plugins 'ModelCustom-ImGui-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'THIRD_PARTY_NOTICES.md') -Destination $plugins
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'docs\INSTALL.txt') -Destination (Join-Path $staging 'README.txt')

$archive = Join-Path $dist "ModelCustom-v$Version-win64.zip"
Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $archive -Force
$hash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
$checksum = "$hash  $([IO.Path]::GetFileName($archive))`n"
[IO.File]::WriteAllText("$archive.sha256", $checksum, [Text.Encoding]::ASCII)


$resolvedStaging = (Resolve-Path -LiteralPath $staging).Path
$resolvedDist = (Resolve-Path -LiteralPath $dist).Path
if ([IO.Path]::GetDirectoryName($resolvedStaging) -ne $resolvedDist -or
    -not [IO.Path]::GetFileName($resolvedStaging).StartsWith('.stage-')) {
    throw 'Unexpected staging path; refusing cleanup.'
}
Remove-Item -LiteralPath $resolvedStaging -Recurse -Force
Write-Host "Packaged: $archive"
Write-Host "SHA-256: $hash"
