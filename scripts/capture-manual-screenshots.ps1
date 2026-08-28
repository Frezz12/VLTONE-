param(
    [string]$Executable = "",
    [string]$OutputRoot = "",
    [switch]$SkipExisting
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot ".."))
if (-not $Executable) {
    $Executable = Join-Path $repoRoot "build-windows\stage\bin\VLT Studio Pro.exe"
}
if (-not $OutputRoot) {
    $OutputRoot = Join-Path $repoRoot "web\public\manual"
}
$Executable = [IO.Path]::GetFullPath($Executable)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)

if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
    throw "VLT Studio Pro executable was not found: $Executable"
}

$tempBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$tempRoot = Join-Path $tempBase ("vlt-manual-" + [Guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $tempRoot | Out-Null

function New-DemoWave([string]$Path) {
    $sampleRate = 44100
    $sampleCount = [int]($sampleRate * 0.8)
    $dataBytes = $sampleCount * 2
    $stream = [IO.File]::Open($Path, [IO.FileMode]::Create, [IO.FileAccess]::Write)
    $writer = [IO.BinaryWriter]::new($stream)
    try {
        $writer.Write([Text.Encoding]::ASCII.GetBytes("RIFF"))
        $writer.Write([int](36 + $dataBytes))
        $writer.Write([Text.Encoding]::ASCII.GetBytes("WAVEfmt "))
        $writer.Write([int]16)
        $writer.Write([int16]1)
        $writer.Write([int16]1)
        $writer.Write([int]$sampleRate)
        $writer.Write([int]($sampleRate * 2))
        $writer.Write([int16]2)
        $writer.Write([int16]16)
        $writer.Write([Text.Encoding]::ASCII.GetBytes("data"))
        $writer.Write([int]$dataBytes)
        for ($index = 0; $index -lt $sampleCount; $index++) {
            $fade = [Math]::Min(1.0, [Math]::Min($index / 1800.0, ($sampleCount - $index) / 5000.0))
            $sample = [int16](12000 * $fade * [Math]::Sin(2.0 * [Math]::PI * 220.0 * $index / $sampleRate))
            $writer.Write($sample)
        }
    } finally {
        $writer.Dispose()
        $stream.Dispose()
    }
}

$sampleDir = Join-Path $tempRoot "samples"
New-Item -ItemType Directory -Path $sampleDir | Out-Null
$samplePath = Join-Path $sampleDir "VLT Manual Tone.wav"
New-DemoWave $samplePath

$states = @(
    @{ Name = "startup"; Env = @{ DAW_SHOT_STARTUP = "1" } },
    @{ Name = "arrangement"; Env = @{} },
    @{ Name = "track-folders"; Env = @{ DAW_SHOT_TRACKS = "4"; DAW_SHOT_NEST = "3"; DAW_SHOT_SELECT = "Audio" } },
    @{ Name = "cycle"; Env = @{ DAW_SHOT_CYCLE = "2,8" } },
    @{ Name = "context-panel"; Env = @{ DAW_SHOT_CONTEXT = "0,1" } },
    @{ Name = "browser-samples"; Env = @{ DAW_SHOT_BROWSER = $sampleDir; DAW_SHOT_BROWSER_FILE = $samplePath; DAW_SHOT_DELAY = "1200" } },
    @{ Name = "browser-plugins"; Env = @{ DAW_SHOT_BROWSER_PLUGINS = "1" } },
    @{ Name = "audio-editor"; Env = @{ DAW_SHOT_CLIP_EDITOR = "1" } },
    @{ Name = "recording"; Env = @{ DAW_SHOT_RECORD = "1" } },
    @{ Name = "takes"; Env = @{ DAW_SHOT_TAKE = "layers"; DAW_SHOT_DELAY = "900" } },
    @{ Name = "piano-roll"; Env = @{ DAW_SHOT_PIANOROLL = "selected" } },
    @{ Name = "pattern"; Env = @{ DAW_SHOT_PATTERN = "editor" } },
    @{ Name = "sampler"; Env = @{ DAW_SHOT_SAMPLER = $samplePath } },
    @{ Name = "mixer"; Env = @{ DAW_SHOT_MIXER = "420" } },
    @{ Name = "plugin-picker"; Env = @{ DAW_SHOT_MENU = "plugins" } },
    @{ Name = "plugin-search"; Env = @{ DAW_SHOT_PLUGIN_SEARCH = "open" } },
    @{ Name = "plugin-manager"; Env = @{ DAW_SHOT_PLUGINS = "0" } },
    @{ Name = "plugin-paths"; Env = @{ DAW_SHOT_PLUGINS = "1" } },
    @{ Name = "plugin-blacklist"; Env = @{ DAW_SHOT_PLUGINS = "2" } },
    @{ Name = "automation-lane"; Env = @{ DAW_SHOT_AUTOMATION = "1" } },
    @{ Name = "automation-editor"; Env = @{ DAW_SHOT_AUTOMATION_EDITOR = "select" } },
    @{ Name = "ai-chat"; Env = @{ DAW_SHOT_AI = "complete" } },
    @{ Name = "ai-music"; Env = @{ DAW_SHOT_AI = "music" } },
    @{ Name = "web"; Env = @{ DAW_SHOT_WEB = "1"; DAW_SHOT_DELAY = "1000" } },
    @{ Name = "export-mix"; Env = @{ DAW_SHOT_EXPORT = "mix" } },
    @{ Name = "export-stems"; Env = @{ DAW_SHOT_EXPORT = "stems" } },
    @{ Name = "recovery"; Env = @{ DAW_SHOT_RECOVERY = "1" } },
    @{ Name = "settings-audio"; Env = @{ DAW_SHOT_SETTINGS = "0" } },
    @{ Name = "settings-transport"; Env = @{ DAW_SHOT_SETTINGS = "1" } },
    @{ Name = "settings-recording"; Env = @{ DAW_SHOT_SETTINGS = "2" } },
    @{ Name = "settings-context"; Env = @{ DAW_SHOT_SETTINGS = "3" } },
    @{ Name = "settings-browser"; Env = @{ DAW_SHOT_SETTINGS = "4" } },
    @{ Name = "settings-ai"; Env = @{ DAW_SHOT_SETTINGS = "5" } },
    @{ Name = "settings-account"; Env = @{ DAW_SHOT_SETTINGS = "6" } },
    @{ Name = "settings-language"; Env = @{ DAW_SHOT_SETTINGS = "7" } },
    @{ Name = "settings-recovery"; Env = @{ DAW_SHOT_SETTINGS = "8" } },
    @{ Name = "settings-themes"; Env = @{ DAW_SHOT_SETTINGS = "9" } },
    @{ Name = "settings-theme-editor"; Env = @{ DAW_SHOT_SETTINGS = "10" } },
    @{ Name = "settings-shortcuts"; Env = @{ DAW_SHOT_SETTINGS = "11" } }
)

$shotKeys = @(
    "DAW_SHOT_STARTUP", "DAW_SHOT_TRACKS", "DAW_SHOT_NEST", "DAW_SHOT_SELECT",
    "DAW_SHOT_CYCLE", "DAW_SHOT_CONTEXT", "DAW_SHOT_BROWSER", "DAW_SHOT_BROWSER_FILE",
    "DAW_SHOT_BROWSER_PLUGINS", "DAW_SHOT_CLIP_EDITOR", "DAW_SHOT_RECORD", "DAW_SHOT_TAKE",
    "DAW_SHOT_PIANOROLL", "DAW_SHOT_PATTERN", "DAW_SHOT_SAMPLER", "DAW_SHOT_MIXER",
    "DAW_SHOT_MENU", "DAW_SHOT_PLUGIN_SEARCH", "DAW_SHOT_PLUGINS", "DAW_SHOT_AUTOMATION",
    "DAW_SHOT_AUTOMATION_EDITOR", "DAW_SHOT_AI", "DAW_SHOT_WEB", "DAW_SHOT_EXPORT",
    "DAW_SHOT_RECOVERY", "DAW_SHOT_SETTINGS", "DAW_SHOT_DELAY"
)

try {
    foreach ($locale in @("ru", "en")) {
        $localeDir = Join-Path $OutputRoot $locale
        New-Item -ItemType Directory -Force -Path $localeDir | Out-Null

        foreach ($state in $states) {
            $output = Join-Path $localeDir ($state.Name + ".png")
            if ($SkipExisting -and (Test-Path -LiteralPath $output -PathType Leaf) -and (Get-Item -LiteralPath $output).Length -gt 100) {
                Write-Host "[$locale] $($state.Name) (existing)"
                continue
            }

            $prefsDir = Join-Path $tempRoot ("prefs-" + $locale + "-" + $state.Name)
            New-Item -ItemType Directory -Force -Path $prefsDir | Out-Null

            foreach ($key in $shotKeys) {
                [Environment]::SetEnvironmentVariable($key, $null, "Process")
            }
            [Environment]::SetEnvironmentVariable("DAW_PREF_DIR", $prefsDir, "Process")
            [Environment]::SetEnvironmentVariable("DAW_SHOT_SIZE", "1440x1200", "Process")
            foreach ($entry in $state.Env.GetEnumerator()) {
                [Environment]::SetEnvironmentVariable([string]$entry.Key, [string]$entry.Value, "Process")
            }

            Write-Host "[$locale] $($state.Name)"
            $process = Start-Process -FilePath $Executable -ArgumentList @(
                "--screenshot", ('"' + $output + '"'), "--theme", "logic", "--language", $locale
            ) -WorkingDirectory (Split-Path -Parent $Executable) -WindowStyle Hidden -Wait -PassThru
            if ($process.ExitCode -ne 0) {
                throw "Screenshot process failed ($($process.ExitCode)): $locale/$($state.Name)"
            }
            if (-not (Test-Path -LiteralPath $output -PathType Leaf) -or (Get-Item -LiteralPath $output).Length -le 100) {
                throw "Screenshot was not created: $output"
            }
        }
    }

    $files = @(Get-ChildItem -LiteralPath $OutputRoot -Recurse -File -Filter *.png)
    foreach ($locale in @("ru", "en")) {
        foreach ($state in $states) {
            $expected = Join-Path (Join-Path $OutputRoot $locale) ($state.Name + ".png")
            if (-not (Test-Path -LiteralPath $expected -PathType Leaf) -or (Get-Item -LiteralPath $expected).Length -le 100) {
                throw "Missing required screenshot: $expected"
            }
        }
    }
    if ($files.Count -lt 78) {
        throw "Expected at least 78 screenshots, found $($files.Count)."
    }
    Write-Host "Manual screenshots ready: 78 files in $OutputRoot"
} finally {
    foreach ($key in $shotKeys + @("DAW_PREF_DIR", "DAW_SHOT_SIZE")) {
        [Environment]::SetEnvironmentVariable($key, $null, "Process")
    }
    $resolvedTemp = [IO.Path]::GetFullPath($tempRoot)
    if ($resolvedTemp.StartsWith($tempBase, [StringComparison]::OrdinalIgnoreCase) -and (Test-Path -LiteralPath $resolvedTemp)) {
        Remove-Item -LiteralPath $resolvedTemp -Recurse -Force
    }
}
