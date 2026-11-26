"""NEXT-FW-INTEG-01R ARM static checks for one reproduction run.

Usage: python -I arm_static.py <baseline root> <run output dir>
Each image is compared with the baseline build that used the same options:
<baseline root>/arm (default, verification), arm-capture (both Capture builds), arm-trace.
Writes <run>/static/<image>/ for the default, verification, both Capture and the
trace builds, and exits nonzero on any violated check:
locked main() prologue differs from baseline, the Board output adapter is neither the
baseline (beyond one literal-pool word) nor the reviewed M0 adapter with its backend
attach function absent from the image, selection is not between clock init and ADC init, a failed selection
does not branch to WFI, Flash program/erase symbols exist, the default image links
the synthetic verification supply, or a protected entry point is missing.
Since RES-01 the images use link-time optimization except for the anchor files (main.c,
foc_board.c, foc_board_boot.c and the supply files): helpers inline or gain .lto_priv/.isra
suffixes, so symbols removed relative to the baseline are reported, not failed, and the
collected stack-usage files cover only the anchor files.
"""
import hashlib
import re
import subprocess
import sys
from pathlib import Path

BASE = Path(sys.argv[1])
RUN = Path(sys.argv[2])
OUT = RUN / "static"
IMAGES = {
    name: RUN / f"arm-{name}" / f"{image}.elf"
    for name, image in (
        ("default", "falconfoc-v2-at32f415"),
        ("verification", "falconfoc-v2-at32f415-calibration-verification"),
        ("default-capture", "falconfoc-v2-at32f415"),
        ("verification-capture", "falconfoc-v2-at32f415-calibration-verification"),
        ("default-trace", "falconfoc-v2-at32f415"),
    )
}
failures = []
# Entry points that must stay in every image as separate symbols: the boot selection chain
# and the output adapter (anchor files, compiled without LTO), the calibration admission and
# initialization, and the inhibit and supervisor entry points of the safety path.
PROTECTED_SYMBOLS = {
    "main",
    "foc_board_calibration_expectation",
    "foc_board_calibration_supply",
    "foc_board_boot_select",
    "foc_board_boot_select_with",
    "foc_board_boot_qualify",
    "foc_board_output_submit",
    "foc_board_init_with_calibration",
    "foc_calibration_admit",
    "foc_runtime_request_inhibit",
    "foc_device_output_inhibit",
    "foc_safe_supervisor_tick",
}
# Reviewed removals relative to the option-matched baseline, each with its reason; other
# removals are LTO inlining and are listed without one.
EXPLAINED_REMOVALS = {
    "foc_board_capture_default_profile":
        "main() takes the capture profile from the effective boot configuration "
        "(foc_board_boot_capture_profile), so the nominal-only helper is garbage-collected",
    "foc_serial_capability":
        "M2: it now reads the adapter's persistence binding (capability bit 6) and the "
        "compiler inlines it into its two callers, HELLO and CAPABILITY_READ",
    "foc_trajectory_position_step":
        "P1 step B: Runtime POSITION mode tracks the planned reference "
        "(foc_trajectory_track_step); the measured-position planner stays for Host "
        "callers and is garbage-collected from the ARM images",
    "sinf": "ROM-01: every sinf()/cosf() call now goes through foc_sincosf() (core/src/foc_trig.c), so newlib's Payne-Hanek reduction is no longer linked",
    "cosf": "ROM-01: every sinf()/cosf() call now goes through foc_sincosf() (core/src/foc_trig.c), so newlib's Payne-Hanek reduction is no longer linked",
    "__ieee754_rem_pio2f": "ROM-01: every sinf()/cosf() call now goes through foc_sincosf() (core/src/foc_trig.c), so newlib's Payne-Hanek reduction is no longer linked",
    "__kernel_rem_pio2f": "ROM-01: every sinf()/cosf() call now goes through foc_sincosf() (core/src/foc_trig.c), so newlib's Payne-Hanek reduction is no longer linked",
    "__kernel_sinf": "ROM-01: every sinf()/cosf() call now goes through foc_sincosf() (core/src/foc_trig.c), so newlib's Payne-Hanek reduction is no longer linked",
    "__kernel_cosf": "ROM-01: every sinf()/cosf() call now goes through foc_sincosf() (core/src/foc_trig.c), so newlib's Payne-Hanek reduction is no longer linked",
    "scalbnf": "ROM-01: every sinf()/cosf() call now goes through foc_sincosf() (core/src/foc_trig.c), so newlib's Payne-Hanek reduction is no longer linked",
    "finitef": "ROM-01: every sinf()/cosf() call now goes through foc_sincosf() (core/src/foc_trig.c), so newlib's Payne-Hanek reduction is no longer linked",
    "PIo2": "ROM-01: every sinf()/cosf() call now goes through foc_sincosf() (core/src/foc_trig.c), so newlib's Payne-Hanek reduction is no longer linked",
    "init_jk": "ROM-01: every sinf()/cosf() call now goes through foc_sincosf() (core/src/foc_trig.c), so newlib's Payne-Hanek reduction is no longer linked",
    "npio2_hw": "ROM-01: every sinf()/cosf() call now goes through foc_sincosf() (core/src/foc_trig.c), so newlib's Payne-Hanek reduction is no longer linked",
    "two_over_pi": "ROM-01: every sinf()/cosf() call now goes through foc_sincosf() (core/src/foc_trig.c), so newlib's Payne-Hanek reduction is no longer linked",
    "hypotf": "ROM-01: hypotf() calls were replaced by sqrtf(x*x + y*y); the operands are volts and amps, far from overflow",
    "__ieee754_hypotf": "ROM-01: hypotf() calls were replaced by sqrtf(x*x + y*y); the operands are volts and amps, far from overflow",
}

# M0 Board1 SIL added an optional output backend to foc_board_output_submit. The ARM
# images never attach one: foc_board_output_attach, the only writer of the backend
# field, is unreferenced and garbage-collected, so the adapter always takes the locked
# path. The reviewed adapter is pinned by the SHA-256 of its normalized disassembly
# (literal-pool words masked); any further change fails until reviewed again.
REVIEWED_ADAPTERS = {
    "ef8862282895dfce01302b8d1e8e86cd56bece8a92063e5e898d7d42ceeac4bc":
        "M0: backend branch for UPDATE/INHIBIT; locked dispositions unchanged without a backend",
}
BACKEND_ATTACH = "foc_board_output_attach"

# ROM-01: the image avoids libm's large-argument trig reduction (use foc_sincosf) and the
# double sqrt/hypot/fmax/fmin (calibration uses foc_calibration_math.c). Any of these
# symbols returning means a sinf/cosf/hypotf or double libm call slipped back in.
HEAVY_LIBM = {"__ieee754_rem_pio2f", "__kernel_rem_pio2f", "__ieee754_hypotf", "__ieee754_hypot",
              "__ieee754_sqrt", "fmax", "fmin"}


# Reviewed locked main() prologues (up to system_core_clock_update) that intentionally differ
# from the baseline, by digest of the normalized instructions and first ten literals (RAM
# addresses masked), each with its reason. Every one still starts with the output lock.
REVIEWED_PROLOGUES = {
    "ebd5ef22d8738811e4851bde41ac5f635db9b69825b4086e889ab88e0a0ff6fc":
        "New Board1 pinout (2026-11-23): after the unchanged output lock (TMR1 reset, PA7-PA10 "
        "and PB0/PB1 analog) the board pins are set (PA1-PA3 phase-voltage analog, NFAULT PB2 "
        "input, PB8 pull-down) and the PLL runs from the 8 MHz crystal (x75/4) with a HICK "
        "fallback recorded in clock_from_crystal",
    "eb8d476b7a15c849f0d4bc5efeed290c316213c70de1575ece78a63b84e60ba0":
        "CAN1 start after the restore (2026-11-24): the pinout prologue instruction for "
        "instruction, same calls and order; only register allocation differs because the "
        "inlined CAN init reads clock_from_crystal later and keeps its address in a "
        "callee-saved register",
    "d058797222020e7f65d9a753eafbbcb52e920413e07f64619fb739e6e805d806":
        "CAN1 start after the restore (2026-11-24), timing-trace build: as above, plus wide "
        "encodings (ldr.w/strb.w) of the same clock_from_crystal store",
}


def prologue_digest(main_text):
    lines = rename_callee_saved(normalize(main_text, True))
    words = ["RAM" if value.startswith("0x2000") else value for value in literals(main_text)[:10]]
    return hashlib.sha256(chr(10).join(lines + words).encode()).hexdigest()


def adapter_digest(text):
    lines = [re.sub(r"^\.word\s+\S+", ".word LIT", line) for line in normalize(text)]
    return hashlib.sha256("\n".join(lines).encode()).hexdigest()


def tool(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def function(disassembly, name):
    match = re.search(rf"^[0-9a-f]+ <{re.escape(name)}>:\n(.*?)(?:\n\n|\Z)", disassembly, re.S | re.M)
    return match.group(0) if match else ""


def symbols(nm_text):
    return {line.split()[-1] for line in nm_text.splitlines() if line.strip()}


def normalize(text, prologue=False):
    lines = []
    for raw in text.splitlines()[1:]:
        match = re.match(r"\s*[0-9a-f]+:\s*(.*)", raw)
        if not match:
            continue
        line = match.group(1)
        line = re.sub(r"[0-9a-f]{7,8} (<[^>]+>)", r"\1", line)
        line = re.sub(r"<(\w+)\+0x[0-9a-f]+>", r"<\1+X>", line)
        line = re.sub(r"@ \([0-9a-f]+ <[^>]+>\)", "@ (LIT)", line)
        line = re.sub(r"\[pc, #\d+\]", "[pc, #N]", line)
        if prologue:
            line = re.sub(r"sp, #\d+.*", "sp, #S", line)
            line = re.sub(r"\[sp, #S", "[sp", line)
        lines.append(line.strip())
        if prologue and "system_core_clock_update" in line:
            break
    return lines


CALLEE_SAVED = re.compile(r"\b(r[4-9]|r1[01]|sl|fp)\b")


def rename_callee_saved(lines):
    """M3: main() grew, so the compiler saves more registers and permutes r4-r11.
    Rename callee-saved registers consistently by first use and mask the saved-register
    list; every instruction, operand shape, call target and literal still compares exactly."""
    mapping = {}
    out = []
    for line in lines:
        if line.startswith(("stmdb\tsp!", "push\t")):
            line = re.sub(r"\{[^}]*\}", "{SAVED}", line)
        out.append(CALLEE_SAVED.sub(lambda m: mapping.setdefault(m.group(1), f"R{len(mapping)}"), line))
    return out


def literals(text):
    return [line.split()[-1] for line in text.splitlines() if ".word" in line]


summary = []


def baseline_for(name):
    folder = "arm-capture" if name.endswith("-capture") else "arm-trace" if name.endswith("-trace") else "arm"
    elf = BASE / folder / "falconfoc-v2-at32f415.elf"
    disassembly = tool("arm-none-eabi-objdump", "-d", "--no-show-raw-insn", str(elf))
    return (folder, symbols(tool("arm-none-eabi-nm", str(elf))), function(disassembly, "main"),
            function(disassembly, "foc_board_output_submit"))


for name, elf in IMAGES.items():
    base_folder, base_syms, base_main, base_adapter = baseline_for(name)
    out = OUT / name
    out.mkdir(parents=True, exist_ok=True)
    dis = tool("arm-none-eabi-objdump", "-d", "--no-show-raw-insn", str(elf))
    nm = tool("arm-none-eabi-nm", "-S", str(elf))
    (out / "full.dis").write_text(dis)
    (out / "nm.txt").write_text(nm)
    (out / "size.txt").write_text(tool("arm-none-eabi-size", "-A", str(elf)))
    for fn in ("main", "foc_board_output_submit", "foc_board_boot_select_with", "foc_board_boot_qualify"):
        (out / f"{fn}.dis").write_text(function(dis, fn))
    syms = symbols(nm)
    added, removed = sorted(syms - base_syms), sorted(base_syms - syms)
    (out / "symbols-added.txt").write_text("\n".join(added) + "\n")
    (out / "symbols-removed.txt").write_text("\n".join(removed) + "\n")
    stack = []
    for su in sorted(elf.parent.rglob("*.su")):
        stack.append(su.read_text())
    (out / "stack-usage.su").write_text("".join(stack))
    for item in ("bin", "hex", "map"):
        sibling = elf.with_suffix("." + item)
        (out / "sha256.txt").open("a").write(
            hashlib.sha256(sibling.read_bytes()).hexdigest() + "  " + sibling.name + "\n"
        )

    main = function(dis, "main")
    calls = re.findall(r"\bbl\s+[0-9a-f]+ <(\w+)>", main)
    (out / "main-call-order.txt").write_text("\n".join(calls) + "\n")
    pro_base = rename_callee_saved(normalize(base_main, True))
    pro_new = rename_callee_saved(normalize(main, True))
    prologue_ok = (pro_base == pro_new and literals(base_main)[:10] == literals(main)[:10]) or (
        prologue_digest(main) in REVIEWED_PROLOGUES
    )
    try:
        i = calls.index("system_core_clock_update")
        order_ok = calls[i + 1 : i + 4] == [
            "foc_board_calibration_expectation",
            "foc_board_calibration_supply",
            "foc_board_boot_select",
        ] and "adc_reset" in calls[i + 4 :] and "foc_board_init" not in calls
    except ValueError:
        order_ok = False
    after = main.split("<foc_board_boot_select>", 1)[1].splitlines()[1:4] if "<foc_board_boot_select>" in main else []
    # The compiler may first copy the status to a callee-saved register; the test and branch
    # on r0 must follow directly.
    if after and re.search(r"(^|\s)mov\s+r\d+, r0$", after[0]):
        after = after[1:]
    after = after[:2]
    target = re.search(r"b\w*\.n?\s+([0-9a-f]+)", after[1]) if len(after) == 2 else None
    wfi_ok = bool(target) and "cmp\tr0, #0" in after[0] and re.search(rf"^\s*{target.group(1)}:\s*wfi", main, re.M) is not None
    adapter_diff = [
        (a, b) for a, b in zip(normalize(base_adapter), normalize(function(dis, "foc_board_output_submit"))) if a != b
    ]
    adapter_as_baseline = len(normalize(base_adapter)) == len(
        normalize(function(dis, "foc_board_output_submit"))
    ) and all(a.startswith(".word") and b.startswith(".word") for a, b in adapter_diff) and len(adapter_diff) <= 1
    adapter_sha = adapter_digest(function(dis, "foc_board_output_submit"))
    adapter_reviewed = adapter_sha in REVIEWED_ADAPTERS and BACKEND_ATTACH not in syms
    adapter_ok = adapter_as_baseline or adapter_reviewed
    flash_ok = not re.search(r"flash_(sector|page|word|halfword|byte|program|erase|unlock)", nm, re.I)
    supply_ok = ("foc_board_verification_supply" in syms) == name.startswith("verification")
    reach_ok = {"foc_board_init_with_calibration", "foc_calibration_admit", "foc_board_boot_select"} <= syms
    checks = {
        "prologue_identical": prologue_ok,
        "selection_between_clock_and_adc": order_ok,
        "failed_selection_branches_to_wfi": wfi_ok,
        "adapter_baseline_or_reviewed_unattached": adapter_ok,
        "no_flash_write_symbols": flash_ok,
        "verification_supply_only_in_verification": supply_ok,
        "calibration_init_reachable": reach_ok,
        "protected_symbols_present": PROTECTED_SYMBOLS <= syms,
        "no_libm_heavy_math": not (syms & HEAVY_LIBM),
    }
    lines = [f"{key}: {'PASS' if value else 'FAIL'}" for key, value in checks.items()]
    lines.append(f"baseline: {base_folder}")
    lines.append(f"adapter: {'baseline' if adapter_as_baseline else 'reviewed' if adapter_reviewed else 'UNREVIEWED'}"
                 f" sha256 {adapter_sha}; {BACKEND_ATTACH} linked: {BACKEND_ATTACH in syms}")
    lines.append(f"adapter differences from baseline: {len(adapter_diff)} lines")
    lines.append("protected symbols missing: " + (" ".join(sorted(PROTECTED_SYMBOLS - syms)) or "none"))
    lines.append("explained removals: " + "; ".join(f"{sym}: {EXPLAINED_REMOVALS[sym]}" for sym in removed
                                                     if sym in EXPLAINED_REMOVALS))
    lines.append(f"other removals ({len([s for s in removed if s not in EXPLAINED_REMOVALS])}, "
                 "inlined or localized by LTO): "
                 + " ".join(s for s in removed if s not in EXPLAINED_REMOVALS))
    lines.append(f"symbols added ({len(added)}): {' '.join(added)}")
    (out / "checks.txt").write_text("\n".join(lines) + "\n")
    summary.append(f"[{name}] " + ", ".join(lines[: len(checks)]))
    failures += [f"{name}:{key}" for key, value in checks.items() if not value]

(OUT / "summary.txt").write_text("\n".join(summary) + "\n")
print("\n".join(summary))
print("FAILURES: " + ", ".join(failures) if failures else "ALL STATIC CHECKS PASS")
sys.exit(1 if failures else 0)
