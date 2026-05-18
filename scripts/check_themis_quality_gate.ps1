Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$canonicalScript = Join-Path $PSScriptRoot "gates\check_themis_quality_gate.ps1"
if (-not (Test-Path -LiteralPath $canonicalScript)) {
    throw "Canonical script not found: $canonicalScript"
}

& powershell -NoProfile -ExecutionPolicy Bypass -File $canonicalScript @args
exit $LASTEXITCODE