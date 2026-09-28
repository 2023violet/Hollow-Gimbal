# AT32F415 FalconFoc Code Wiki Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a source-grounded Code Wiki for the current AT32F415 FalconFoc working tree and verify its claims with software-only evidence.

**Architecture:** Use a small multi-page Wiki under `docs/code-wiki/` with one canonical entry page and focused pages for architecture, modules, dependencies/operation, and deep analysis. Keep current source facts separate from stale formal documents, cited historical evidence, and unverified hardware claims.

**Tech Stack:** Markdown, PowerShell repository inspection, CMake/Ninja, CTest, ARM GCC when available, existing FalconFoc contract scripts.

**Spec:** `docs/superpowers/specs/2026-08-27-at32f415-code-wiki-design.md`

## Global Constraints

- Current working tree and `platform/at/at32f415` are the implementation authority.
- Third-party AT32 vendor/CMSIS code is documented by role, selected files, and boundaries only.
- No existing user changes or generated artifacts are reset, deleted, staged, or committed.
- No source behavior is changed by this task.
- Board and serial validation are explicitly outside the validation lane.
- Every material claim is labeled `LIVE-SOURCE`, `LIVE-TEST`, `CITED-BASELINE`, or `UNVERIFIED`.

---

### Task 1: Establish the AT32F415 source and evidence inventory

**Files:**
- Read: `README.md`, `AGENTS.md`, `docs/README.md`, architecture/boot/governance entry docs
- Read: `CMakeLists.txt`, `CMakePresets.json`, layer `CMakeLists.txt` files
- Read: `applications/`, `bsp/`, `components/`, `subsys/`, `platform/at/at32f415`, `scripts/`, and `tests/` source contracts
- Output evidence: terminal logs and a working-tree fact table, not a repository source change

**Interfaces:**
- Consumes: current repository files and Git status
- Produces: verified file/module inventory, current platform/build facts, and a list of stale or contradictory docs to cite in the Wiki

- [ ] **Step 1: Re-check source authority and dirty-worktree boundaries**

Run:

```powershell
git status --short --branch
git log -1 --oneline --decorate
Test-Path platform/at/at32f415
Test-Path platform/at/at32f403a
```

Expected: current changes are preserved; `platform/at/at32f415` exists; the
Wiki scope is not expanded to unrelated generated/build artifacts.

- [ ] **Step 2: Inventory product files, public APIs, and build targets**

Run PowerShell inspection over `applications`, `bsp`, `components`, `subsys`,
`platform/at/at32f415`, `cmake`, `scripts`, and `tests`. Record only paths and
symbols needed by the six deliverables.

Expected: every live product module is mapped to an owner, public header, key
implementation, and at least one dependency or test relationship.

- [ ] **Step 3: Identify validation blockers before writing claims**

Check tool availability and current preset/cache paths:

```powershell
Get-Command cmake,ninja,ctest,arm-none-eabi-gcc,arm-none-eabi-size -ErrorAction SilentlyContinue
Select-String -Path CMakePresets.json,tests/CMakeLists.txt,tests/script/*.ps1 -Pattern 'at32f403a|at32f415|host-debug|fw-'
```

Expected: known stale paths and missing tools are captured as evidence, not
silently treated as product failures.

### Task 2: Write the canonical Wiki entry and architecture page

**Files:**
- Create: `docs/code-wiki/README.md`
- Create: `docs/code-wiki/architecture.md`

**Interfaces:**
- Consumes: Task 1 source/evidence inventory; current `app_main.c`, `motor_task.c`, `motor_fsm.c`, FOC configuration, and AT32F415 generated code
- Produces: stable links and terminology used by Tasks 3–5

- [ ] **Step 1: Write the entry page**

Include current scope, platform facts, evidence legend, repository map, reading
map, authority precedence, and explicit warning that existing formal docs still
contain AT32F403A/COM54 baseline material.

- [ ] **Step 2: Write the architecture page**

Include layer diagram, boot sequence, ISR/task cadence table, motor state graph,
FOC data flow, shell flow, persistence flow, and product-vs-platform ownership.
Every path must link to the source file that implements it.

- [ ] **Step 3: Self-check architecture claims**

Run:

```powershell
Select-String -Path docs/code-wiki/*.md -Pattern 'AT32F403A|AT32F415|COM54|COM3|LIVE-SOURCE|LIVE-TEST|UNVERIFIED'
```

Expected: platform facts are consistently labeled and no hardware claim is
presented as software-only evidence.

### Task 3: Write the module/API and dependency pages

**Files:**
- Create: `docs/code-wiki/modules.md`
- Create: `docs/code-wiki/dependencies.md`

**Interfaces:**
- Consumes: terminology and links from Tasks 1–2
- Produces: module responsibility matrix, key types/functions table, CMake target graph, include/dependency notes, feature/profile matrix, and persistence ownership map

- [ ] **Step 1: Document applications modules**

Cover `algo`, `periph`, `runtime`, `tasks`, `shell`, and `app_main`. For each,
state responsibility, context, inputs/outputs, key symbols, and tests.

- [ ] **Step 2: Document BSP, components, subsys, and platform modules**

Cover BSP contracts, selected reusable algorithms/utilities/protocols, shared
runtime/shell/export/storage/time mechanisms, and the AT32F415 generated/vendor
boundary. Keep vendor documentation at boundary level.

- [ ] **Step 3: Document dependency and build assembly**

Map `falcon-foc-at32` to `applications`, `bsp`, `subsys-core`, `freertos`,
`algorithm`, `protocol`, `utils`, and `at32f415-hal`; explain metadata ownership,
CPU class, linker/startup selection, feature gates, and host-test targets.

- [ ] **Step 4: Check links and symbol names**

Run a PowerShell link/path check for every relative source link introduced in
`docs/code-wiki/*.md`. Expected: all referenced files exist and all key symbols
match current source spelling.

### Task 4: Write operations, software simulation, and deep analysis pages

**Files:**
- Create: `docs/code-wiki/operations-and-validation.md`
- Create: `docs/code-wiki/deep-analysis.md`

**Interfaces:**
- Consumes: all prior Wiki pages and the current test/build scripts
- Produces: reproducible software-only commands, scenario expectations, fresh evidence slots, limitations, and prioritized architectural analysis

- [ ] **Step 1: Document run/configure/build commands**

Cover `cmake --preset host-debug`, `cmake --build --preset build-host-debug`,
`ctest --preset test-host-debug`, AT32F415 firmware presets, `./run.sh` commands,
memory reporting, and relevant PowerShell gates. Mark board-only commands as
excluded from this task.

- [ ] **Step 2: Define software simulation scenarios**

Describe deterministic source-level scenarios for boot failure capture, FSM
transitions, fault clear, shell queue full, deferred save, A/B slot selection,
restore finalization, motion trajectory update, calibration completion, and
10 kHz/5 kHz/1 kHz cadence. Include precondition, stimulus, expected state, and
actual test target when one exists.

- [ ] **Step 3: Record deep architectural findings**

Separate strengths, risks, and follow-ups. At minimum evaluate platform/doc
drift, `SHELL_ENABLE` vs `TM_FEATURE_SHELL`, host test path hardcoding, generated
DMA initialization versus runtime ownership, soft-float cadence, persistence
context safety, ISR/task shared state, and unused/optional subsystem assembly.

### Task 5: Update formal indexes and maintenance log without rewriting user work

**Files:**
- Modify: `docs/README.md`
- Modify: `docs/architecture/README.md` only if the new Wiki belongs in the architecture read path
- Modify: `docs/governance/Themis_工程维护日志.md`

**Interfaces:**
- Consumes: final Wiki paths and current evidence classification
- Produces: discoverable formal documentation entry and concise maintenance-log entry

- [ ] **Step 1: Add one Code Wiki entry to the docs root index**

Link to `code-wiki/README.md` and state that it is generated from the current
AT32F415 working-tree review.

- [ ] **Step 2: Append a concise maintenance entry**

Record the Wiki scope, AT32F415 authority, software-only validation boundary,
and the fact that older AT32F403A/COM54 documents remain cited drift until a
separate policy round reconciles them.

- [ ] **Step 3: Ensure no unrelated doc changes are overwritten**

Use `git diff -- docs/README.md docs/architecture/README.md docs/governance/...`
and preserve all pre-existing hunks.

### Task 6: Run validation and close the evidence chain

**Files:**
- Read: all `docs/code-wiki/*.md`
- Read: test/build output and generated reports under existing build directories
- Do not modify source files as part of validation

**Interfaces:**
- Consumes: complete Wiki and existing toolchain
- Produces: fresh validation results recorded in `operations-and-validation.md`

- [ ] **Step 1: Validate Markdown path references**

Run a PowerShell script that resolves every relative Markdown link under
`docs/code-wiki/` against its containing file. Expected: zero missing targets;
external links are reported separately.

- [ ] **Step 2: Run native host configure/build/tests**

Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/run_host_tests.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
```

Expected: record exact exit code and test count. If blocked by stale
`at32f403a` include paths or tool availability, record the first blocking error
and run the narrowest usable target instead.

- [ ] **Step 3: Run software-only contract and CMake checks**

Run available checks such as:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/gates/check_cmake_smoke.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
powershell -NoProfile -ExecutionPolicy Bypass -File tests/script/test_falconfoc_themis_lite_contract.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
powershell -NoProfile -ExecutionPolicy Bypass -File tests/script/test_firmware_cli_contract.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
```

Run each only if its declared prerequisites are available; record pass/fail and
whether failure is caused by an existing stale contract.

- [ ] **Step 4: Run AT32F415 firmware configure/build if available**

Use the current `fw-*` preset or `./run.sh build` with the selected AT32F415
defaults. Record artifact paths, size output, and any build failure without
touching or deleting existing build trees.

- [ ] **Step 5: Add fresh results and limitations to the validation page**

Record commands, exit codes, test counts, and scenario outcomes. Explicitly
state that no board/serial/real-time/electrical behavior was validated.

- [ ] **Step 6: Final requirement and diff review**

Run:

```powershell
git diff --check
git status --short --branch
git diff --stat
```

Confirm all six Wiki deliverables exist, indexes point to them, pre-existing
changes remain intact, and no completion claim is made without fresh output.
