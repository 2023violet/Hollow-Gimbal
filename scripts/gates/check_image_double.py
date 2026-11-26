"""Image double check: `double` may appear in Board1 image sources only where allowed.

Usage: python -B check_image_double.py <repo root>

AT32F415 has no FPU, so every double operation links libgcc's soft-double routines
(about 2.4 KiB) and costs far more cycles than float. The code style guide (section 6)
allows double only in the Foreground calibration estimators below: they accumulate
least-squares normal equations whose expanded residual cancels far below float
precision. Every other image file must stay float-only. The scanned files are those
compiled into the image: firmware/at32f415/src and include, and the portable sources
listed in the falconfoc-board1 library (with their headers of the same name).

An allowed file that no longer contains double also fails, so the list stays exact.
fwinteg's arm_static.py separately proves that no soft-double routine is reachable
from an interrupt handler.
"""
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else Path(__file__).resolve().parents[2]
FIRMWARE = ROOT / "firmware" / "at32f415"

REASON = "Foreground calibration least squares (normal equations, expanded residual)"
ALLOWED = {
    "calibration/src/foc_motor_id.c": REASON + "; CAL-3 R/L/dead-time fit and CAL-5 PI design",
    "calibration/include/foc_motor_id.h": REASON,
    "calibration/src/foc_flux_id.c": REASON + "; CAL-4 flux fit",
    "calibration/include/foc_flux_id.h": REASON,
    "calibration/src/foc_mechanical_id.c": REASON + "; CAL-7 inertia and damping fit",
    "calibration/include/foc_mechanical_id.h": REASON,
    "calibration/src/foc_calibration_math.c": "double sqrt/fmax/fmin for the estimators above",
    "calibration/src/foc_calibration_math.h": "double sqrt/fmax/fmin for the estimators above",
    "firmware/at32f415/src/foc_board_calibrate.c": REASON + "; coast check flux and angle fit",
    "firmware/at32f415/include/foc_board_calibrate.h": REASON + "; coast check sums",
}

COMMENT = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
STRING = re.compile(r'"(?:\\.|[^"\\])*"')
DOUBLE = re.compile(r"\bdouble\b")


def image_files():
    files = set(FIRMWARE.glob("src/*.c")) | set(FIRMWARE.glob("src/*.h"))
    files |= set(FIRMWARE.glob("include/*.h"))
    text = (FIRMWARE / "CMakeLists.txt").read_text(encoding="utf-8")
    block = re.search(r"add_library\(\s*falconfoc-board1 STATIC(.*?)\)", text, re.S)
    if block is None:
        sys.exit("check_image_double: falconfoc-board1 library not found in CMakeLists.txt")
    for rel in re.findall(r'"\$\{repo\}/([^"]+\.c)"', block.group(1)):
        source = ROOT / rel
        files.add(source)
        module = source.parent.parent
        for header in (module / "include" / f"{source.stem}.h", source.with_suffix(".h")):
            if header.exists():
                files.add(header)
    return sorted(files)


def has_double(path):
    code = STRING.sub('""', COMMENT.sub(" ", path.read_text(encoding="utf-8")))
    return [number for number, line in enumerate(code.splitlines(), 1) if DOUBLE.search(line)]


failures = []
scanned = image_files()
seen = set()
for path in scanned:
    rel = path.relative_to(ROOT).as_posix()
    lines = has_double(path)
    if rel in ALLOWED:
        seen.add(rel)
        if not lines:
            failures.append(f"{rel}: allowed but contains no double; remove it from ALLOWED")
    elif lines:
        failures.append(f"{rel}: double on line(s) {', '.join(map(str, lines[:10]))}")
for rel in sorted(set(ALLOWED) - seen):
    failures.append(f"{rel}: allowed but not compiled into the image; remove it from ALLOWED")

print(f"[image_double] scanned {len(scanned)} image files, {len(ALLOWED)} allowed")
if failures:
    print("\n".join(f"[image_double] FAIL {item}" for item in failures))
    sys.exit(1)
print("[image_double] double appears only in the allowed calibration estimators")
