[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $DistributionDirectory,
    [string] $DumpbinPath = "dumpbin.exe"
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$distribution = [IO.Path]::GetFullPath($DistributionDirectory)
if (-not (Test-Path -LiteralPath $distribution -PathType Container)) {
    throw "Distribution directory does not exist: $distribution"
}

$binaries = @(Get-ChildItem -LiteralPath $distribution -Recurse -File |
    Where-Object { $_.Extension -in ".exe", ".dll" })
if ($binaries.Count -eq 0) {
    throw "No PE binaries found under $distribution"
}

$shipped = [Collections.Generic.HashSet[string]]::new(
    [StringComparer]::OrdinalIgnoreCase)
foreach ($binary in $binaries) {
    [void] $shipped.Add($binary.Name)
}

$systemDirectories = @([Environment]::SystemDirectory) |
    Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Container) }

$missing = [Collections.Generic.List[string]]::new()
foreach ($binary in $binaries) {
    $dump = & $DumpbinPath /nologo /dependents $binary.FullName 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "dumpbin failed for $($binary.FullName):`n$($dump -join [Environment]::NewLine)"
    }

    foreach ($line in $dump) {
        $dependency = $line.Trim()
        if ($dependency -notmatch '^[A-Za-z0-9_.+-]+\.dll$') { continue }
        if ($dependency.StartsWith("api-ms-win-", [StringComparison]::OrdinalIgnoreCase) -or
            $dependency.StartsWith("ext-ms-win-", [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        if ($shipped.Contains($dependency)) { continue }

        $resolvedByWindows = $false
        foreach ($systemDirectory in $systemDirectories) {
            if (Test-Path -LiteralPath (Join-Path $systemDirectory $dependency) -PathType Leaf) {
                $resolvedByWindows = $true
                break
            }
        }
        if (-not $resolvedByWindows) {
            $missing.Add("$($binary.FullName) -> $dependency")
        }
    }
}

if ($missing.Count -gt 0) {
    throw "Unresolved runtime dependencies:`n$($missing -join [Environment]::NewLine)"
}

Write-Host "Runtime dependency check passed for $($binaries.Count) PE files."
