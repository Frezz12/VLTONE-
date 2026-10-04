[CmdletBinding()]
param([string] $Destination = "")
$ErrorActionPreference = "Stop"
if (-not $Destination) { $Destination = Join-Path $PSScriptRoot '..\.cache\creator-tools' }
$Destination = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $Destination | Out-Null
function Get-Archive([string] $Name, [string] $Url, [string] $Hash) {
    $path = Join-Path $Destination $Name
    if (-not (Test-Path -LiteralPath $path)) {
        & curl.exe -L --fail --retry 3 --silent --show-error -o $path $Url
        if ($LASTEXITCODE) { throw "Download failed: $Name" }
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $Hash) {
        throw "Checksum mismatch: $Name"
    }
    return $path
}
$llvmName = 'clang+llvm-20.1.8-x86_64-pc-windows-msvc'
if (-not (Test-Path -LiteralPath (Join-Path $Destination "$llvmName\lib\cmake\clang\ClangConfig.cmake"))) {
    $archive = Get-Archive "$llvmName.tar.xz" 'https://github.com/llvm/llvm-project/releases/download/llvmorg-20.1.8/clang%2Bllvm-20.1.8-x86_64-pc-windows-msvc.tar.xz' 'f229769f11d6a6edc8ada599c0cda964b7dee6ab1a08c6cf9dd7f513e85b107f'
    & tar.exe -xf $archive -C $Destination
    if ($LASTEXITCODE) { throw 'LLVM extraction failed' }
}
if (-not (Test-Path -LiteralPath (Join-Path $Destination 'wamrc\wamrc.exe'))) {
    $archive = Get-Archive 'wamrc.zip' 'https://github.com/wasm-micro-runtime/wasm-micro-runtime/releases/download/WAMR-2.4.5/wamrc-2.4.5-x86_64-windows-2022.zip' 'a913cda13f7d8aa6c3d77229d3f9c268a8714c5ce86be3e13b39cedb07ea958e'
    Expand-Archive -LiteralPath $archive -DestinationPath (Join-Path $Destination 'wamrc') -Force
}
if (-not (Test-Path -LiteralPath (Join-Path $Destination 'wasi-sysroot-27.0\include\c++\v1\array'))) {
    $archive = Get-Archive 'wasi-sysroot.tar.gz' 'https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-27/wasi-sysroot-27.0.tar.gz' '7110ac48f5d0b1f6ab67d57aecf52450540dddd790cafdc45f0fdfb429bdab84'
    & tar.exe -xf $archive -C $Destination
    if ($LASTEXITCODE) { throw 'WASI sysroot extraction failed' }
}
Write-Output "Creator LLVM: $Destination\$llvmName"
