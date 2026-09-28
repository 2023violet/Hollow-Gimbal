$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$wrapperPath = Join-Path $repoRoot "scripts\flows\board\run_raw_sine_match_sixstep_probe.ps1"
$wrapperText = Get-Content -Raw -LiteralPath $wrapperPath

foreach ($required in @(
    'run_fixed_duty_probe.ps1',
    '-TestName "RAW_SINE_MATCH_SIXSTEP"',
    '-OutputDescription "electrical_hz=1 duty_center=0.50 duty_amplitude=0.115470 matched_vector=raw_sixstep"',
    '-ObservationCommand "obs all"'
)) {
    if ($wrapperText -notmatch [regex]::Escape($required)) {
        throw "Raw-sine-match-sixstep runner is missing: $required"
    }
}

Write-Output "RAW_SINE_MATCH_SIXSTEP_PROBE_CONTRACT=PASS"
