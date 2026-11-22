"""NEXT-FW-INTEG-01R ARM static checks for one reproduction run.

Usage: python -I arm_static.py <baseline root> <run output dir>
Each image is compared with the baseline build that used the same options:
<baseline root>/arm (default, verification), arm-capture (both Capture builds), arm-trace.
Writes <run>/static/<image>/ for the default, verification, both Capture and the
trace builds, and exits nonzero on any violated check:
locked main() prologue differs from baseline, Board output adapter differs beyond one
literal-pool word, selection is not between clock init and ADC init, a failed selection
does not branch to WFI, Flash program/erase symbols exist, or the default image links
the synthetic verification supply.
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
# Intentional removals relative to the option-matched baseline, each with its reason.
EXPLAINED_REMOVALS = {
    "foc_board_capture_default_profile":
        "main() takes the capture profile from the effective boot configuration "
        "(foc_board_boot_capture_profile), so the nominal-only helper is garbage-collected",
}


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
    pro_base, pro_new = normalize(base_main, True), normalize(main, True)
    prologue_ok = pro_base == pro_new and literals(base_main)[:10] == literals(main)[:10]
    try:
        i = calls.index("system_core_clock_update")
        order_ok = calls[i + 1 : i + 4] == [
            "foc_board_calibration_expectation",
            "foc_board_calibration_supply",
            "foc_board_boot_select",
        ] and "adc_reset" in calls[i + 4 :] and "foc_board_init" not in calls
    except ValueError:
        order_ok = False
    after = main.split("<foc_board_boot_select>", 1)[1].splitlines()[1:3] if "<foc_board_boot_select>" in main else []
    target = re.search(r"b\w*\.n?\s+([0-9a-f]+)", after[1]) if len(after) == 2 else None
    wfi_ok = bool(target) and "cmp\tr0, #0" in after[0] and re.search(rf"^\s*{target.group(1)}:\s*wfi", main, re.M) is not None
    adapter_diff = [
        (a, b) for a, b in zip(normalize(base_adapter), normalize(function(dis, "foc_board_output_submit"))) if a != b
    ]
    adapter_ok = len(normalize(base_adapter)) == len(normalize(function(dis, "foc_board_output_submit"))) and all(
        a.startswith(".word") and b.startswith(".word") for a, b in adapter_diff
    ) and len(adapter_diff) <= 1
    flash_ok = not re.search(r"flash_(sector|page|word|halfword|byte|program|erase|unlock)", nm, re.I)
    supply_ok = ("foc_board_verification_supply" in syms) == name.startswith("verification")
    reach_ok = {"foc_board_init_with_calibration", "foc_calibration_admit", "foc_board_boot_select"} <= syms
    checks = {
        "prologue_identical": prologue_ok,
        "selection_between_clock_and_adc": order_ok,
        "failed_selection_branches_to_wfi": wfi_ok,
        "adapter_identical_but_literal": adapter_ok,
        "no_flash_write_symbols": flash_ok,
        "verification_supply_only_in_verification": supply_ok,
        "calibration_init_reachable": reach_ok,
        "no_unexplained_symbols_removed": not (set(removed) - set(EXPLAINED_REMOVALS)),
    }
    lines = [f"{key}: {'PASS' if value else 'FAIL'}" for key, value in checks.items()]
    lines.append(f"baseline: {base_folder}")
    lines.append(f"adapter literal differences: {adapter_diff}")
    lines.append("explained removals: " + "; ".join(f"{sym}: {EXPLAINED_REMOVALS[sym]}" for sym in removed
                                                     if sym in EXPLAINED_REMOVALS))
    lines.append(f"symbols added ({len(added)}): {' '.join(added)}")
    (out / "checks.txt").write_text("\n".join(lines) + "\n")
    summary.append(f"[{name}] " + ", ".join(lines[: len(checks)]))
    failures += [f"{name}:{key}" for key, value in checks.items() if not value]

(OUT / "summary.txt").write_text("\n".join(summary) + "\n")
print("\n".join(summary))
print("FAILURES: " + ", ".join(failures) if failures else "ALL STATIC CHECKS PASS")
sys.exit(1 if failures else 0)
