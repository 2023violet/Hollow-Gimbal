#!/usr/bin/env python3
"""Analyze run_openloop_probe.ps1 logs (T1 TriggerScan / T2 VILinearity / T3 ThetaSweep).

Host-side, stdlib only, never touches a board. Usage:

    python scripts/flows/board/analyze_openloop_probe.py [log_dir ...]

With no arguments it scans tmp/bench/*_openloop_probe_*/ (newest first) and
analyzes every directory that contains case logs.

Model being tested (docs/planning/Themis_开环根因延申验证与修复计划.md §5.2):

    firmware:  meas_a = -k*true_c,  meas_b = -k*true_b,  meas_c = -k*true_a
    raw level: dR2 = k*true_a, dR3 = k*true_b, dR4 = k*true_c  (counts)

where dR = raw - offset of that ADC rank, true = V/R phase current computed
from the trace theta_sim/vq/vbus with the manual Rs=2.55 ohm, and k is the
unknown scale/validity factor. The analyzer reports per-case k per rank and
the trigger position where each rank transitions invalid<->valid, which pins
the rank/phase pairing and the sampling-window geometry (plan P1-P4).
"""

import glob
import math
import os
import re
import sys

PR = 3749.0
TMR_CLK = 150e6
RS_DEFAULT = 2.55
COUNTS_PER_AMP = 1.0 / 0.0016117  # FAC_CURR inverse, ~620.5 counts/A


def parse_float(text, default=None):
    try:
        return float(text)
    except (TypeError, ValueError):
        return default


def parse_case(path):
    """Parse one probe case log into a dict."""
    case = {"file": os.path.basename(path), "samples": [], "offsets": None,
            "theta0": None, "trig": None, "vq": None, "we": None,
            "peaks": None, "post_pos": None, "pre_pos": None, "raw_range": None}
    name = case["file"]
    m = re.search(r"trig(\d+)", name)
    if m:
        case["trig"] = int(m.group(1))
    m = re.search(r"theta(\d+)_(\d+)", name)
    if m and "scan" in name:
        case["theta0"] = float("%s.%s" % (m.group(1), m.group(2)))
    m = re.search(r"vi_vq_(\d+)_(\d+)", name)
    if m:
        case["vq"] = float("%s.%s" % (m.group(1), m.group(2)))
    m = re.search(r"sweep_theta(\d+)_(\d+)", name)
    if m:
        case["theta0"] = float("%s.%s" % (m.group(1), m.group(2)))

    text = open(path, encoding="utf-8", errors="replace").read()

    m = re.search(r"TX: diag set theta0 ([-\d.]+)", text)
    if m:
        case["theta0"] = float(m.group(1))
    m = re.search(r"TX: diag set adc_trig (\d+)", text)
    if m:
        case["trig"] = int(m.group(1))
    m = re.search(r"TX: diag set vq ([-\d.]+) ([-\d.]+)", text)
    if m:
        case["vq"] = float(m.group(1))
        case["we"] = float(m.group(2))

    for m in re.finditer(r"off=([\d.]+)/([\d.]+)/([\d.]+)", text):
        case["offsets"] = tuple(float(x) for x in m.groups())
    m = re.search(r"peak=([-0-9.]+)/([-0-9.]+)/([-0-9.]+)", text)
    if m:
        case["peaks"] = tuple(float(x) for x in m.groups())
    m = re.search(r"raw=([\d]+)-([\d]+)/([\d]+)-([\d]+)/([\d]+)-([\d]+)", text)
    if m:
        case["raw_range"] = tuple(int(x) for x in m.groups())
    for m in re.finditer(r"ccr=(\d+)/(\d+)/(\d+)/(\d+)", text):
        pass  # keep last
    ccrs = None
    for m in re.finditer(r"ccr=(\d+)/(\d+)/(\d+)/(\d+)", text):
        ccrs = tuple(int(x) for x in m.groups())
    case["ccr"] = ccrs

    # Trace prints raw=(vbus, ic_raw, ib_raw, ia_raw): label order, not rank
    # order (app_shell_cmd_diag.c). Offsets from obs are (ia, ib, ic).
    for m in re.finditer(
            r"raw=(\d+)/(-?\d+)/(-?\d+)/(-?\d+)\s*\n\s*phase\s*:.*?i_ma=(-?[\d.]+)/(-?[\d.]+)/(-?[\d.]+).*?theta_mrad=(-?\d+)/(-?\d+)",
            text, re.DOTALL):
        vb, r_ic, r_ib, r_ia = (int(m.group(i)) for i in range(1, 5))
        ia, ib, ic = (float(m.group(i)) / 1000.0 for i in range(5, 8))
        ts = int(m.group(8)) / 1000.0
        case["samples"].append({"raw": (r_ia, r_ib, r_ic), "i": (ia, ib, ic), "theta_sim": ts})

    m = re.search(r"duty\s*:\s*a=([\d.]+)-[\d.]+\s+b=([\d.]+)-[\d.]+\s+c=([\d.]+)-[\d.]+", text)
    if m:
        case["duty_show"] = tuple(float(x) for x in m.groups())

    pos_all = re.findall(r"pos_turn=(-?[\d.]+)", text)
    if pos_all:
        case["post_pos"] = float(pos_all[-1])
    return case


def parse_preflight_positions(directory):
    """Ordered pre-pulse pos_turn values (one state show per case)."""
    path = os.path.join(directory, "preflight.log")
    if not os.path.exists(path):
        return []
    text = open(path, encoding="utf-8", errors="replace").read()
    return [float(x) for x in re.findall(r"pos_turn=(-?[\d.]+)", text)]


def true_currents(theta_sim, vq, vbus, rs):
    """Phase currents for a static vector (vd=0, vq) at theta_sim, star R load."""
    if None in (theta_sim, vq, vbus) or not vbus:
        return None
    v_alpha = -vq * math.sin(theta_sim)
    v_beta = vq * math.cos(theta_sim)
    va = v_alpha
    vb = -v_alpha / 2.0 + math.sqrt(3.0) / 2.0 * v_beta
    vc = -v_alpha / 2.0 - math.sqrt(3.0) / 2.0 * v_beta
    cm = (max(va, vb, vc) + min(va, vb, vc)) / 2.0
    i_a = (va - cm) / rs
    i_b = (vb - cm) / rs
    i_c = (vc - cm) / rs
    return (i_a, i_b, i_c)


def analyze_case(case):
    rows = []
    offs = case.get("offsets")
    if not offs or not case["samples"]:
        return rows
    # map0: A->CH3 (CCR3), B->CH2 (CCR2), C->CH1 (CCR1)
    ccr = case.get("ccr") or (None, None, None, None)
    ccr_c, ccr_b, ccr_a = ccr[0], ccr[1], ccr[2]
    vbus = None
    for s in case["samples"]:
        ts = s["theta_sim"]
        true_i = true_currents(ts, case.get("vq"), vbus_guess(case), RS_DEFAULT)
        true_counts = tuple(x * COUNTS_PER_AMP for x in true_i) if true_i else None
        dr = []
        for idx, raw in enumerate(s["raw"]):
            dr.append(raw - offs[idx])
        # k per phase: firmware labels are the corrected 2026-08-30 mapping,
        # so measured label = k * true label directly.
        ks = []
        if true_counts:
            for meas, exp in zip(dr, true_counts):
                ks.append(meas / exp if abs(exp) > 30 else None)
        else:
            ks = [None, None, None]
        rows.append({
            "theta0": case.get("theta0"), "trig": case.get("trig"), "vq": case.get("vq"),
            "ccr_abc": (ccr_a, ccr_b, ccr_c), "theta_sim": ts,
            "dR": tuple(dr), "true_A": true_i, "k_abc": (ks[0], ks[1], ks[2]),
        })
    return rows


def vbus_guess(case):
    # 11.9 V nominal bench supply; per-case vbus is inside raw[0] but the
    # divisor matters at the few-percent level only for this analysis.
    return 11.9


def fmt(x, nd=3):
    if x is None:
        return "  --  "
    return "%*.3f" % (7, x) if nd == 3 else "%*g" % (7, x)


def main():
    dirs = sys.argv[1:]
    if not dirs:
        root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "tmp", "bench")
        dirs = sorted(glob.glob(os.path.join(root, "*_openloop_probe_*")), reverse=True)
    if not dirs:
        print("no *_openloop_probe_* directories found")
        return

    for directory in dirs:
        logs = sorted(glob.glob(os.path.join(directory, "*.log")))
        logs = [p for p in logs if os.path.basename(p) not in ("preflight.log", "operator-notes.md")]
        cases = [parse_case(p) for p in logs]
        cases = [c for c in cases if c["samples"]]
        if not cases:
            continue
        print("=" * 100)
        print("DIR:", directory)
        pre_positions = parse_preflight_positions(directory)

        print("%-14s %5s %6s | %-17s | %-8s | %-21s | %-21s | %s" % (
            "case", "trig", "vq", "ccr(a/b/c)", "th_sim", "dR a/b/c s0 (cnt)",
            "k(a/b/c) swap-model", "note"))
        for case in cases:
            rows = analyze_case(case)
            for row in rows[:1]:  # sample 0 (sample 1 repeats the same static condition)
                ccr_txt = "/".join("-" if x is None else str(x) for x in row["ccr_abc"])
                dr_txt = "/".join("%+.0f" % x for x in row["dR"])
                k_txt = "/".join(("%+.2f" % x) if x is not None else "--" for x in row["k_abc"])
                note = ""
                if case.get("peaks"):
                    note = "peak=" + "/".join("%.2f" % abs(p) for p in case["peaks"])
                print("%-14s %5s %6s | %-17s | %-8.3f | %-21s | %-21s | %s" % (
                    case["file"].replace(".log", ""), str(row["trig"]),
                    "--" if row["vq"] is None else "%.2f" % row["vq"],
                    ccr_txt, row["theta_sim"], dr_txt, k_txt, note))

        # transition summary per (theta0, rank): first trig where |dR| clearly tracks
        print("-- validity vs trigger (|dR|>25 counts counts as active) --")
        by_seed = {}
        for case in cases:
            rows = analyze_case(case)
            if not rows:
                continue
            row = rows[0]
            key = row["theta0"]
            by_seed.setdefault(key, []).append(row)
        for seed in sorted(by_seed, key=lambda x: (x is None, x)):
            rows = sorted(by_seed[seed], key=lambda r: (r["trig"] is None, r["trig"]))
            print("  theta0=%s:" % seed)
            for rank_idx, rank_name in ((0, "ia"), (1, "ib"), (2, "ic")):
                states = []
                for r in rows:
                    active = abs(r["dR"][rank_idx]) > 25.0
                    ccr = r["ccr_abc"][rank_idx]
                    states.append("%s:%s%s" % (r["trig"], "A" if active else ".",
                                               "(%s)" % ccr if ccr else ""))
                print("    %-8s %s" % (rank_name, " ".join(states)))

        # T3 sweep: encoder position response
        sweep = [c for c in cases if "sweep_theta" in c["file"]]
        if sweep:
            print("-- theta sweep encoder response (post_pos - pre_pos, turns) --")
            for idx, case in enumerate(sorted(sweep, key=lambda c: c["theta0"] or 0)):
                pre = pre_positions[idx] if idx < len(pre_positions) else None
                delta = (case["post_pos"] - pre) if (case["post_pos"] is not None and pre is not None) else None
                print("  theta0=%-9.4f pre=%s post=%s delta=%s peaks=%s" % (
                    case["theta0"] or -1,
                    "%.3f" % pre if pre is not None else "--",
                    "%.3f" % case["post_pos"] if case["post_pos"] is not None else "--",
                    "%+.3f" % delta if delta is not None else "--",
                    "/".join("%.2f" % abs(p) for p in case["peaks"]) if case["peaks"] else "--"))


if __name__ == "__main__":
    main()
