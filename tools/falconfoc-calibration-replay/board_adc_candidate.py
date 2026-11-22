"""FW-ADC-01: Board1 Capture -> ADC zero-offset candidate -> isolated Runtime admission (Host).

The production importer (board_capture.import_image) validates the SRAM image
(magic, version, length, CRC, identity and configuration declaration, sequence,
time order, window, producer/source) and, only with an independent zero-current
witness, prepares the ADC_ZERO Trace V1 whose p1..p3 are logical A/B/C from
ranks 2/3/4. This orchestrator adds no validation or statistics of its own: it
maps the importer's precondition status to the zero-current gate and runs the
C adapter (foc-board-adc-candidate), which uses the existing ADC estimator and
foc_board_boot_select() on an isolated instance.

Zero-current gate:
  QUALIFIED_MEASUREMENT_TRACE (Host producer, fixture witness) -> QUALIFIED,
      labelled HOST_SYNTHETIC or HOST_VIRTUAL as the caller declares.
  TRACE_PREPARED_PROVENANCE_UNVERIFIED (Board producer) -> PRECONDITION_UNVERIFIED,
      no candidate: software INHIBIT is not proof of winding zero current.
  No witness (DIAGNOSTIC_OBSERVATION) -> PRECONDITION_REQUIRED, no candidate.
"""

import argparse
import json
from pathlib import Path
import subprocess
import sys

import board_capture
from replay import PackError


def run(args):
    args.out.mkdir()
    report = {"format_version": 1, "stage": "ADC_ZERO", "scope": "ADC zero-offset candidate only"}
    importer = argparse.Namespace(image=args.image, metadata=args.metadata, out=args.out / "import",
                                  qualification=args.qualification)
    try:
        imported = board_capture.import_image(importer)
    except PackError as error:
        report.update(status="REJECTED", stage_reached="IMPORT", reason=error.reason, detail=str(error))
        return report, 2
    diagnostic = json.loads((args.out / "import" / "diagnostic.json").read_bytes())
    report.update(import_status=imported["status"], provenance=imported["provenance"],
                  identity_check=imported["identity_check"], capture_id=diagnostic["header"]["id"],
                  raw_sha256=diagnostic["files"]["capture.bin"], source=diagnostic["source"])
    if imported["status"] == "TRACE_PREPARED_PROVENANCE_UNVERIFIED":
        report.update(status="PRECONDITION_UNVERIFIED", zero_current="UNVERIFIED", candidate=None,
                      detail="Board-recorded capture without verified independent zero-current evidence.")
        return report, 3
    if imported["status"] != "QUALIFIED_MEASUREMENT_TRACE":
        report.update(status="PRECONDITION_REQUIRED", zero_current="NOT_PROVIDED", candidate=None,
                      detail="No independent zero-current witness was supplied.")
        return report, 3
    source = "HOST_VIRTUAL" if args.host_kind == "virtual" else "HOST_SYNTHETIC"
    review = int(report["raw_sha256"][:8], 16) or 1
    command = [str(args.adapter), str(args.out / "import" / "trace" / "samples.csv"),
               "--capture-id", str(report["capture_id"]), "--review-id", str(review),
               "--source", source, "--zero-current", "QUALIFIED"]
    if args.fault:
        command += ["--fault", args.fault]
    process = subprocess.run(command, capture_output=True, text=True, timeout=30)
    (args.out / "adapter.json").write_text(process.stdout, encoding="utf-8")
    try:
        candidate = json.loads(process.stdout)
    except ValueError:
        report.update(status="REJECTED", stage_reached="ADAPTER", reason="ADAPTER_OUTPUT",
                      detail=process.stdout + process.stderr)
        return report, 2
    report.update(zero_current=f"{source}_FIXTURE_DECLARED", review_id_reference=review,
                  candidate=candidate, adapter_exit=process.returncode, status=candidate.get("status"))
    return report, process.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--metadata", type=Path, required=True)
    parser.add_argument("--qualification", type=Path)
    parser.add_argument("--adapter", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True, help="New output directory.")
    parser.add_argument("--host-kind", choices=("synthetic", "virtual"), default="synthetic")
    parser.add_argument("--fault", help="Adapter fault injection (tests only).")
    args = parser.parse_args()
    try:
        report, code = run(args)
    except OSError as error:
        report, code = {"status": "REJECTED", "reason": "IO_ERROR", "detail": str(error)}, 2
    if args.out.is_dir():
        (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report))
    return code


if __name__ == "__main__":
    sys.exit(main())
