$ErrorActionPreference = 'Stop'
$env:DAW_PLUGIN_DIAGNOSTICS = '1'
$outputDir = 'D:\Code\DAW\artifacts\vst3-scan'
$installed = 'C:\Program Files\VLT Studio Pro\bin\daw_scan.exe'
$fixed = 'D:\Code\DAW\build-windows\bin\daw_scan.exe'
$cache = Get-Content -LiteralPath "$env:APPDATA\VLT Studio Pro\plugins.json" -Raw | ConvertFrom-Json
$candidates = $cache.entries | Where-Object { $_.blacklisted -and $_.format -eq 'vst3' -and $_.path -like 'C:\*' }
$results = [Collections.Generic.List[object]]::new()
if (Test-Path -LiteralPath (Join-Path $outputDir 'comparison.json')) {
    foreach ($row in (Get-Content -LiteralPath (Join-Path $outputDir 'comparison.json') -Raw | ConvertFrom-Json)) { $results.Add($row) }
}
function Invoke-Scan($exe, $arguments, $label) {
    $stdout = Join-Path $outputDir "$label.json"
    $stderr = Join-Path $outputDir "$label.err"
    $watch = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $timedOut = -not $process.WaitForExit(30000)
    if ($timedOut) { $process.Kill(); $process.WaitForExit() }
    $process.Refresh()
    [pscustomobject]@{ ExitCode = $process.ExitCode; TimedOut = $timedOut; Seconds = [math]::Round($watch.Elapsed.TotalSeconds, 2); Output = (Get-Content -LiteralPath $stdout -Raw) }
}
foreach ($candidate in $candidates) {
    $name = [IO.Path]::GetFileNameWithoutExtension($candidate.path)
    if ($name -in $results.Name) { continue }
    $label = $name -replace '[^a-zA-Z0-9-]', '_'
    $pathArg = '"--path=' + $candidate.path + '"'
    $inspection = Invoke-Scan $installed "--inspect --format=vst3 $pathArg" "$label-inspect"
    if ($inspection.ExitCode -ne 0 -or $inspection.TimedOut) { Write-Output "$name inspection failed"; continue }
    $descriptor = ($inspection.Output | ConvertFrom-Json).plugins[0]
    $arguments = "--validate --format=vst3 $pathArg --uid=$($descriptor.uid)"
    $before = Invoke-Scan $installed $arguments "$label-before"
    $after = Invoke-Scan $fixed $arguments "$label-after"
    $row = [pscustomobject]@{ Name = $name; Before = $before.ExitCode; After = $after.ExitCode; BeforeTimeout = $before.TimedOut; AfterTimeout = $after.TimedOut; BeforeSeconds = $before.Seconds; AfterSeconds = $after.Seconds }
    $results.Add($row)
    $results | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $outputDir 'comparison.json')
    Write-Output "$name : before=$($row.Before), after=$($row.After), seconds=$($row.BeforeSeconds)/$($row.AfterSeconds), timeouts=$($row.BeforeTimeout)/$($row.AfterTimeout)"
}
