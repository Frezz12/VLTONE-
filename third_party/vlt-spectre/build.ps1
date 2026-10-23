param(
    [string]$QtPath = $env:QTDIR,
    [string]$BuildDirectory = 'build',
    [switch]$CoreOnly,
    [int]$Jobs = 4
)
$ErrorActionPreference = 'Stop'
$spectreRoot = $PSScriptRoot
# Import the compiler environment without requiring a Developer PowerShell window.
if (-not $env:VCToolsInstallDir) {
    $spectreVsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $spectreVsWhere)) { throw 'Install Visual Studio C++ Build Tools first.' }
    $spectreVs = (& $spectreVsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
    if (-not $spectreVs) { throw 'No Visual Studio C++ toolchain found.' }
    $spectreDev = Join-Path $spectreVs 'Common7\Tools\VsDevCmd.bat'
    $spectreLines = & $env:ComSpec /d /s /c "call `"$spectreDev`" -no_logo -arch=x64 -host_arch=x64 >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'Cannot initialize the compiler.' }
    foreach ($line in $spectreLines) {
        $at = $line.IndexOf('=')
        if ($at -gt 0) { Set-Item -Path ('Env:' + $line.Substring(0, $at)) -Value $line.Substring($at + 1) }
    }
}
$spectreBuild = Join-Path $spectreRoot $BuildDirectory
$spectreArguments = @('-S', $spectreRoot, '-B', $spectreBuild, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release')
if ($CoreOnly) { $spectreArguments += '-DVLT_SPECTRE_BUILD_WIDGETS=OFF' }
else {
    if (-not $QtPath) { throw 'Pass -QtPath C:\Qt\6.x\msvc2022_64, set QTDIR, or use -CoreOnly.' }
    $spectreArguments += @('-DVLT_SPECTRE_BUILD_WIDGETS=ON', "-DCMAKE_PREFIX_PATH=$QtPath")
    $env:PATH = (Join-Path $QtPath 'bin') + ';' + $env:PATH
    $env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $QtPath 'plugins\platforms'
}
& cmake @spectreArguments
if ($LASTEXITCODE -ne 0) { throw 'Configuration failed.' }
& cmake --build $spectreBuild --parallel $Jobs
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
& ctest --test-dir $spectreBuild --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
if (-not $CoreOnly) {
    & (Join-Path $QtPath 'bin\windeployqt.exe') --no-translations --include-plugins qoffscreen (Join-Path $spectreBuild 'spectre_demo.exe')
    if ($LASTEXITCODE -ne 0) { throw 'Qt runtime deployment failed.' }
    Write-Output "Run: $spectreBuild\spectre_demo.exe"
}