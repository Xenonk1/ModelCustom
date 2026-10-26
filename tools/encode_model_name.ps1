param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9_]{1,64}$')]
    [string]$Name,
    [string]$Output = 'models.txt'
)

$ErrorActionPreference = 'Stop'
$key = [Text.Encoding]::UTF8.GetBytes('ModelCustom::name::v1')
$plain = [Text.Encoding]::UTF8.GetBytes($Name.ToLowerInvariant())
$encoded = for ($i = 0; $i -lt $plain.Length; ++$i) {
    '{0:x2}' -f ($plain[$i] -bxor $key[$i % $key.Length])
}
$line = 'mcx1:' + (-join $encoded)
$outputPath = if ([IO.Path]::IsPathRooted($Output)) { $Output } else { Join-Path (Get-Location) $Output }
[IO.File]::WriteAllText($outputPath, $line + [Environment]::NewLine, [Text.UTF8Encoding]::new($false))
Write-Output "Wrote $outputPath ($line)"
