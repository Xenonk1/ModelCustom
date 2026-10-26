function Invoke-ModelCustomBuild {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Project,
        [ValidateSet('Debug', 'Release')]
        [string]$Configuration = 'Release'
    )

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere -PathType Leaf)) {
        throw 'Install Visual Studio or Build Tools with Desktop development with C++, MSVC v143 and a Windows SDK.'
    }

    $msbuild = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find 'MSBuild\**\Bin\MSBuild.exe' |
        Select-Object -First 1
    if (-not $msbuild) {
        throw 'MSBuild with C++ tools was not found. Install the Desktop development with C++ workload.'
    }

    & $msbuild $Project /m /t:Build "/p:Configuration=$Configuration" /p:Platform=x64 /nologo /verbosity:minimal
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE."
    }
}
