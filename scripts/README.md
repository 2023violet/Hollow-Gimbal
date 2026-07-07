# FalconFoc Scripts

This repository keeps only the FalconFoc live script surface.

Current official entry points:

- quality gate: `scripts/check_themis_quality_gate.ps1`
- host tests: `scripts/run_host_tests.ps1`
- minimal board smoke: `scripts/run_tiny_carrier_regression.ps1`
- diag/cal board witness: `scripts/run_diag_carrier_witness.ps1`
- firmware CLI: `run.sh`, `memory.sh`
- Keil bridge: `scripts/build_project.ps1`

Layout:

- `scripts/gates/`: canonical gate implementations
  - includes `check_cmake_smoke.ps1` for preset-first host/live/prepared-inactive configure smoke
- `scripts/flows/host/`: canonical host flows
- `scripts/flows/board/`: canonical FalconFoc board smoke flows
- `scripts/lib/`: reusable helpers
- root-level `check_*` and `run_*`: compatibility wrappers for the canonical
  paths that remain

The current live board lane is a minimal FalconFoc smoke/regression surface.
Archived product-specific probe flows are not part of the live script contract.

Current formal real-board witness lane:

- `scripts/run_diag_carrier_witness.ps1`
  - full-diag + carrier-demo board witness
  - captures `sys reboot`, boot banner, `help`, `sys status`
  - captures the live `diag/cal` runtime boundary on `COM54`
