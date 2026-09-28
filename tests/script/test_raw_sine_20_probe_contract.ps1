$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$wrapperPath = Join-Path $repoRoot "scripts\flows\board\run_raw_sine_20_probe.ps1"
$wrapperText = Get-Content -Raw -LiteralPath $wrapperPath

foreach ($required in @(
    'run_fixed_duty_probe.ps1',
    '-TestName "RAW_SINE_20"',
    '-OutputDescription "electrical_hz=1 duty_center=0.50 duty_amplitude=0.20"',
    '-ObservationCommand "obs all"',
    '[ValidateRange(1, 20)][int]$HoldSeconds = 20'
)) {
    if ($wrapperText -notmatch [regex]::Escape($required)) {
        throw "Raw-sine-20 runner is missing: $required"
    }
}

Write-Output "RAW_SINE_20_PROBE_CONTRACT=PASS"
