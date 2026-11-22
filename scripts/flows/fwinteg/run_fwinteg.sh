#!/usr/bin/env bash
# NEXT-FW-INTEG-01R reproducible software verification.
#
#   bash scripts/flows/fwinteg/run_fwinteg.sh <new output dir> [--stub]
#
# Run from the repository root. Requires CMake >= 3.25, Ninja, Clang (with the
# ASan/UBSan runtime), MinGW GCC, Arm GNU Toolchain (arm-none-eabi-*), Python 3,
# pwsh, git and cmakelang. The output directory must not exist, so no stale log
# can be mixed in. Every step records its raw exit code in <out>/steps.tsv and
# never stops the run; fwinteg_verdict.py then decides the top-level exit code.
#
# --stub replaces every step with fwinteg_stub.py (synthetic artifacts, optional
# FWINTEG_INJECT faults). It exists only to test the aggregation from this same
# top-level process; it never builds or runs anything.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:?usage: run_fwinteg.sh <new output dir> [--stub]}"
MODE="${2:-real}"
BASELINE_COMMIT="211b226c4ca870c90bf9b9048d112bfce2365ce8"
if [ -e "$OUT" ]; then
  echo "run_fwinteg: output directory '$OUT' already exists; choose a new one" >&2
  exit 2
fi
mkdir -p "$OUT/matrix"
OUT="$(cd "$OUT" && pwd)"
ROOT="$(pwd -W 2>/dev/null || pwd)"
OUTW="$(cd "$OUT" && (pwd -W 2>/dev/null || pwd))"
printf 'step\texit\tlog\n' > "$OUT/steps.tsv"

# step <name> <log file name> <command...>
step() {
  local name="$1" log="$2"
  shift 2
  if [ "$MODE" = "--stub" ] && [ "${FWINTEG_INJECT:-}" = "skip-step" ] && [ "$name" = "uart-check" ]; then
    return 0 # fault injection only: the step never runs and is never recorded
  fi
  if [ "$MODE" = "--stub" ]; then
    python -I "$HERE/fwinteg_stub.py" "$name" "$OUT" "$HERE/expected_tests.tsv" > "$OUT/$log" 2>&1
  else
    "$@" > "$OUT/$log" 2>&1
  fi
  local code=$?
  printf '%s\t%s\t%s\n' "$name" "$code" "$log" >> "$OUT/steps.tsv"
  echo "$name exit $code"
}

# Baseline ARM image from BASELINE_COMMIT, built with the options of the images compared to it.
baseline_arm() {
  local name="$1"
  shift
  cmake -S "$OUT/baseline/src/firmware/at32f415" -B "$OUT/baseline/$name" -G Ninja \
    "-DCMAKE_TOOLCHAIN_FILE=$ROOT/tools/cmake/arm-gcc-toolchain.cmake" \
    -DMCU_CPU=cortex-m4 -DMCU_FLOAT=soft -DMCU_FPU=none "$@" &&
    cmake --build "$OUT/baseline/$name"
}

baseline() {
  local src="$OUT/baseline/src"
  mkdir -p "$src" &&
    git archive "$BASELINE_COMMIT" | tar -x -C "$src" &&
    baseline_arm arm &&
    baseline_arm arm-capture -DFALCONFOC_BOARD_CAPTURE=ON -DFALCONFOC_BOARD_CAPTURE_BUILD_TAG=7 &&
    baseline_arm arm-trace -DFALCONFOC_RT02_TIMING_TRACE=ON &&
    cmake -S "$src/firmware/at32f415" -B "$OUT/baseline/host" -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=clang &&
    cmake --build "$OUT/baseline/host" --target foc-serial-sim &&
    python -B tools/falconfoc-cli/falconfoc_cli.py \
      --simulator "$OUTW/baseline/host/foc-serial-sim.exe" demo > "$OUT/baseline/uart-demo.jsonl"
}

witness() {
  pwsh -NoProfile -Command "\$env:Path=\"\$(clang --print-resource-dir)\\lib\\windows;\$env:Path\"; \
    \$env:ASAN_OPTIONS='halt_on_error=1'; \$env:UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'; \
    & '$OUTW/matrix/$1-firmware-at32f415/$2.exe'; exit \$LASTEXITCODE"
}

arm_configure() {
  local name="$1"
  shift
  cmake -S firmware/at32f415 -B "$OUT/arm-$name" -G Ninja \
    "-DCMAKE_TOOLCHAIN_FILE=$ROOT/tools/cmake/arm-gcc-toolchain.cmake" \
    -DMCU_CPU=cortex-m4 -DMCU_FLOAT=soft -DMCU_FPU=none "$@"
}

preset() {
  cmake --preset "$1" && cmake --build --preset "build-$1"
}

fault_injection() {
  python -I "$HERE/test_fault_injection.py" "$OUT/fault-injection"
}

step baseline baseline.log baseline
for tc in clang gcc sanitizer; do
  step "matrix-$tc" "matrix-$tc.log" pwsh -NoProfile -File "$HERE/run_matrix.ps1" \
    -OutputRoot "$OUTW/matrix" -Toolchain "$tc" -RepoRoot "$ROOT"
done
for tc in clang gcc sanitizer; do
  for test in test-foc-board-boot test-foc-board-boot-default; do
    step "witness-$tc-$test" "witness-$tc-$test.txt" witness "$tc" "$test"
  done
done
declare -A ARM_ARGS=(
  [default]="-DFALCONFOC_BOARD_CALIBRATION_SUPPLY=NONE"
  [verification]="-DFALCONFOC_BOARD_CALIBRATION_SUPPLY=VERIFICATION"
  [default-capture]="-DFALCONFOC_BOARD_CALIBRATION_SUPPLY=NONE -DFALCONFOC_BOARD_CAPTURE=ON -DFALCONFOC_BOARD_CAPTURE_BUILD_TAG=7"
  [verification-capture]="-DFALCONFOC_BOARD_CALIBRATION_SUPPLY=VERIFICATION -DFALCONFOC_BOARD_CAPTURE=ON -DFALCONFOC_BOARD_CAPTURE_BUILD_TAG=7"
  [default-trace]="-DFALCONFOC_BOARD_CALIBRATION_SUPPLY=NONE -DFALCONFOC_RT02_TIMING_TRACE=ON"
)
for name in default verification default-capture verification-capture default-trace; do
  # shellcheck disable=SC2086
  step "arm-$name-configure" "arm-$name.configure.log" arm_configure "$name" ${ARM_ARGS[$name]}
  step "arm-$name-build" "arm-$name.build.log" cmake --build "$OUT/arm-$name"
done
step arm-static arm-static.log python -I "$HERE/arm_static.py" "$OUT/baseline" "$OUT"
step uart-check uart-check.log python -I "$HERE/uart_check.py" "$OUT/baseline/uart-demo.jsonl" \
  "$OUT/matrix/clang-firmware-at32f415" "$OUT/uart"
for p in fw-full-diag-carrier-debug fw-standard-carrier-minsizerel fw-tiny-carrier-minsizerel; do
  step "preset-$p" "preset-$p.log" preset "$p"
done
step quality-gate quality-gate.log pwsh -NoProfile -ExecutionPolicy Bypass \
  -File scripts/gates/check_themis_quality_gate.ps1 -RepoRoot "$ROOT" -Scope all \
  -RequireCMakeFormatter -Summary
step fault-injection fault-injection.log fault_injection
echo "FWINTEG-STEPS-END" >> "$OUT/steps.tsv"

python -I "$HERE/fwinteg_verdict.py" "$OUT" "$HERE/expected_tests.tsv" | tee "$OUT/verdict.txt"
exit "${PIPESTATUS[0]}"
