$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$wrapperPath = Join-Path $repoRoot "scripts\flows\board\run_raw_sixstep_probe.ps1"
$wrapperText = Get-Content -Raw -LiteralPath $wrapperPath

foreach ($required in @(
    'run_fixed_duty_probe.ps1',
    '-TestName "RAW_SIXSTEP"',
    '-OutputDescription "electrical_hz=1 duty_span=0.40/0.60"'
)) {
    if ($wrapperText -notmatch [regex]::Escape($required)) {
        throw "Raw-sixstep runner is missing: $required"
    }
}

Write-Output "RAW_SIXSTEP_PROBE_CONTRACT=PASS"
