param([string[]]$Targets = @('vlt_script_worker'), [int]$Jobs = 4,
      [string]$BuildDirectory = 'build-windows', [switch]$Configure)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $env:VCToolsInstallDir) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1).Trim()
    $dev = Join-Path $vs 'Common7/Tools/VsDevCmd.bat'
    $lines = & $env:ComSpec /d /s /c "call `"$dev`" -no_logo -arch=x64 -host_arch=x64 -vcvars_ver=14.44 -winsdk=10.0.26100.0 >nul && set"
    if ($LASTEXITCODE -ne 0) { throw 'Cannot activate the supported MSVC toolchain' }
    foreach ($line in $lines) {
        $at = $line.IndexOf('=')
        if ($at -gt 0) { Set-Item -Path ('Env:' + $line.Substring(0,$at)) -Value $line.Substring($at+1) }
    }
}
if ($Configure) {
    $configuration = @('-S', $repo, '-B', (Join-Path $repo $BuildDirectory), '-G', 'Ninja',
        "-DCMAKE_TOOLCHAIN_FILE=$repo/build-windows/_deps/vcpkg/scripts/buildsystems/vcpkg.cmake",
        "-DVCPKG_INSTALLED_DIR=$repo/build-windows/vcpkg_installed",
        "-DCMAKE_PREFIX_PATH=$repo/.cache/qt/6.8.3/msvc2022_64",
        "-DCREATOR_LLVM_ROOT=$repo/.cache/creator-tools/clang+llvm-20.1.8-x86_64-pc-windows-msvc",
        "-DCREATOR_SYSROOT=$repo/.cache/creator-tools/wasi-sysroot-27.0",
        "-DCREATOR_WAMRC=$repo/.cache/creator-tools/wamrc/wamrc.exe",
        "-DFETCHCONTENT_SOURCE_DIR_CREATOR_WAMR=$repo/build-windows/_deps/creator_wamr-src",
        "-DFETCHCONTENT_SOURCE_DIR_CREATOR_BINARYEN=$repo/build-windows/_deps/creator_binaryen-src",
        "-DFETCHCONTENT_SOURCE_DIR_VLT_ORT=$repo/build-windows/_deps/vlt_ort-src",
        '-DCMAKE_BUILD_TYPE=Release', '-DDAW_RELEASE_IPO=OFF', '-DDAW_BUILD_APP=ON',
        '-DDAW_BUILD_TESTS=ON', '-DDAW_PACKAGE=OFF', '-DDAW_ENABLE_REVERB=OFF')
    & cmake @configuration
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
& cmake --build (Join-Path $repo $BuildDirectory) --parallel $Jobs --target @Targets
exit $LASTEXITCODE
