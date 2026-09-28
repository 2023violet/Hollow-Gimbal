param(
    [Parameter(Mandatory = $true)][string]$RepoRoot
)

$ErrorActionPreference = "Stop"
$scriptPath = Join-Path (Resolve-Path -LiteralPath $RepoRoot).Path "scripts\flows\board\run_bench_matrix.ps1"
if (-not (Test-Path -LiteralPath $scriptPath)) {
    throw "Bench matrix runner not found: $scriptPath"
}

$scriptText = Get-Content -LiteralPath $scriptPath -Raw
$phaseMatch = [regex]::Match($scriptText, '(?s)function Invoke-PhaseMatrix \{.*?\n\}')
if (-not $phaseMatch.Success) {
    throw "Invoke-PhaseMatrix implementation is missing."
}

$phaseBlock = $phaseMatch.Value
if ($phaseBlock -match '3\.1415926|2\.3561945|1\.5707963|0\.7853982') {
    throw "Phase matrix contains more than six fractional digits; shell parser rejects these values."
}
if ($phaseBlock -notmatch '"-3\.141592"' -or $phaseBlock -notmatch '"3\.141592"') {
    throw "Phase matrix must use six-digit values inside the inclusive +/- pi runtime bound."
}
if ($phaseBlock -notmatch '\$thetaReply\s*=\s*Save-CommandResult') {
    throw "Phase matrix must capture the theta0 setup reply before energizing the motor."
}
if ($phaseBlock -notmatch '\$thetaReply\s*-notmatch\s*''result=ok''') {
    throw "Phase matrix must abort when theta0 setup is rejected."
}

if ($scriptText -notmatch '\^diag set enc_dir \[01\]\$') {
    throw "Bench runner allow-list must include the idle-only encoder direction experiment."
}

$pulseBlock = [regex]::Match($scriptText, '(?s)function Invoke-BenchPulse \{.*?\n\}')
if (-not $pulseBlock.Success) {
    throw "Invoke-BenchPulse implementation is missing."
}
if ($scriptText -notmatch '\$BenchStopMarginMs') {
    throw "Bench runner must reserve a stop margin before the firmware run budget expires."
}
if ($pulseBlock.Value -notmatch 'BenchStopMarginMs') {
    throw "Bench pulse must apply the stop margin to the effective duration."
}
if ($pulseBlock.Value -notmatch '\[Math\]::Min\(\$runDurationMs,\s*\(\$BenchBudgetMs\s*-\s*\$BenchStopMarginMs\)\)') {
    throw "Bench pulse must clamp the requested duration before the firmware budget expires."
}

Write-Output "BENCH_MATRIX_CONTRACT=PASS"
