Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$canonicalScript = Join-Path $PSScriptRoot "flows\board\run_diag_carrier_witness.ps1"
if (-not (Test-Path -LiteralPath $canonicalScript)) {
    throw "Canonical script not found: $canonicalScript"
}

& powershell -NoProfile -ExecutionPolicy Bypass -File $canonicalScript @args
exit $LASTEXITCODE
