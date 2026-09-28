$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$scriptPath = Join-Path $repoRoot "scripts\flows\board\run_fixed_duty_probe.ps1"
$scriptText = Get-Content -Raw -LiteralPath $scriptPath

foreach ($required in @(
    'sys reboot',
    'diag adc show',
    'diag param velmax 1000000',
    'state start',
    'diag bench keepalive',
    '$TestName`_HOLD_COMPLETE software_stop=not_sent'
)) {
    if ($scriptText -notmatch [regex]::Escape($required)) {
        throw "Fixed-duty runner is missing required command or marker: $required"
    }
}
if ($scriptText -match 'state disable') {
    throw "Fixed-duty runner must not send state disable."
}

Write-Output "FIXED_DUTY_PROBE_CONTRACT=PASS"
