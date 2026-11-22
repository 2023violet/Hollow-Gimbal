# NEXT-FW-INTEG-01R Host matrix for one toolchain.
# Runs every suite even after a failure and records the raw configure, build and
# CTest exit codes per suite in <OutputRoot>/<toolchain>-status.tsv. The verdict
# (fwinteg_verdict.py) decides pass/fail; this script only exits nonzero if any
# recorded exit code is nonzero.
param(
    [Parameter(Mandatory = $true)][string]$OutputRoot,
    [Parameter(Mandatory = $true)][ValidateSet("clang", "gcc", "sanitizer")][string]$Toolchain,
    [Parameter(Mandatory = $true)][string]$RepoRoot
)
$ErrorActionPreference = 'Continue'
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
Set-Location -LiteralPath $RepoRoot
$compiler = if ($Toolchain -eq 'gcc') { 'gcc' } else { 'clang' }
$extra = @()
if ($Toolchain -eq 'sanitizer') {
    $env:Path = "$(clang --print-resource-dir)\lib\windows;$env:Path"
    $env:ASAN_OPTIONS = 'halt_on_error=1'
    $env:UBSAN_OPTIONS = 'halt_on_error=1:print_stacktrace=1'
    $extra += '-DCMAKE_C_FLAGS=-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'
    $extra += '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
}
$dirs = @('core', 'runtime', 'calibration', 'control_plane', 'sensors', 'simulation', 'examples/servo',
          'protocol', 'firmware/at32f415', 'examples/calibration-workflow', 'tools/falconfoc-calibration-replay')
$status = Join-Path $OutputRoot "$Toolchain-status.tsv"
"suite`tconfigure`tbuild`tctest" | Set-Content -LiteralPath $status -Encoding ascii
$failed = $false
foreach ($dir in $dirs) {
    $slug = $dir.Replace('/', '-')
    $out = Join-Path $OutputRoot "$Toolchain-$slug"
    $log = "$out.log"
    $build = 'not-run'
    $test = 'not-run'
    cmake -S $dir -B $out -G Ninja -DCMAKE_BUILD_TYPE=Debug "-DCMAKE_C_COMPILER=$compiler" @extra *> $log
    $configure = $LASTEXITCODE
    if ($configure -eq 0) {
        cmake --build $out -j 4 *>> $log
        $build = $LASTEXITCODE
        if ($build -eq 0) {
            $selected = switch ($dir) {
                'control_plane' { @('-R', '^test-foc-(parameter|config-codec|config-store|config-diagnostics|calibration-application)$') }
                'examples/servo' { @('-R', '^test-falconfoc-servo-') }
                'examples/calibration-workflow' { @('-R', '^test-falconfoc-calibration-workflow-demo$') }
                'tools/falconfoc-calibration-replay' { @('-R', '^test-falconfoc-calibration-replay$') }
            }
            ctest --test-dir $out --no-tests=error --output-on-failure -j 2 @selected *>> $log
            $test = $LASTEXITCODE
        }
    }
    "$slug`t$configure`t$build`t$test" | Add-Content -LiteralPath $status -Encoding ascii
    Write-Output "$Toolchain $slug configure=$configure build=$build ctest=$test"
    "FWINTEG-SUITE-END $slug" | Add-Content -LiteralPath $log -Encoding ascii
    if ($configure -ne 0 -or "$build" -ne '0' -or "$test" -ne '0') { $failed = $true }
}
"FWINTEG-MATRIX-END $Toolchain" | Add-Content -LiteralPath $status -Encoding ascii
Write-Output "FWINTEG-MATRIX-END $Toolchain failed=$failed"
if ($failed) { exit 1 }
exit 0
