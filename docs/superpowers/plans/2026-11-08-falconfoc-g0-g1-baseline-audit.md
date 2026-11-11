# FalconFOC V2 G0/G1 Baseline and Resource Audit Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task after plan approval. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Freeze a reproducible Legacy baseline and produce a source-backed AT32F415 resource, timing, UART, concurrency, and G3-SAFE capability record without modifying the Legacy control chain or performing powered board actions.

**Architecture:** G0 records the current firmware, tests, build identity, board facts, and historical evidence. G1 audits the generated platform, DMA/timer/IRQ ownership, UART path, encoder path, timing assumptions, and safety boundaries. The output is evidence and decisions for G2-G4. No control implementation is changed in this plan.

**Tech Stack:** CMake/Ninja, Arm GNU toolchain, PowerShell 5.1-compatible scripts, existing host CTest, AT32F415 generated sources, FreeRTOS Legacy build metadata, Markdown evidence records.

**Spec:** [FalconFOC V2 platform design](../specs/2026-11-07-falconfoc-v2-platform-design.md), [Runtime ADR R1](../specs/2026-11-08-falconfoc-v2-runtime-adr-r1.md)

## Global Constraints

- Legacy `cortex-m3-soft`, soft-float ABI, and FreeRTOS `ARM_CM3` remain unchanged during G0/G1.
- No source, CMake, generated platform, or Legacy runtime behavior changes are allowed in G0/G1.
- No automatic Git tag creation, commit, push, release, or branch publication.
- No debugger, firmware flash, UART command, PWM enable, motor command, or powered board action.
- Current Board1 external communication is USART/UART. CAN 2.0, CAN-FD, CANopen, and EtherCAT are not G0/G1 delivery conditions.
- G3-SAFE is required before any future real PWM output or powered motor test.
- H0 owns current-loop, modulation, and fast safety state. H1 owns sensor, velocity, position, calibration, and outer-loop state.
- Command, sensor, and telemetry data use different publication and failure rules.
- Every measured or inferred fact must identify its source, command, timestamp, and confidence.

## Review Focus

- Legacy baseline drift — the recorded build, test, and source identity must be reproducible without modifying the working tree.
- Timer/DMA ownership conflict — TMR2, TMR5, SPI DMA, UART DMA, ADC DMA, and IRQ priorities must be mapped before any new runtime allocation.
- Hidden real-time blocking — encoder SPI and UART paths must be checked for unbounded waits, and the audit must name every such path.
- H0/H1 state aliasing — the evidence must show which structures are owned by fast current control and which belong to outer motion control.
- G3-SAFE incompleteness — the audit must list every required pre-power gate and distinguish software checks from hardware protection.

---

### Task 1: Freeze the Legacy baseline manifest

**Files:**
- Create: `docs/evidence/2026-11-08-legacy-baseline.md`
- Read: `README.md`, `AGENTS.md`, `CMakePresets.json`
- Read: `docs/code-wiki/README.md`, `docs/code-wiki/AI-handoff-2026-09-26.md`
- Read: `docs/governance/Themis_工程维护日志.md`

**Interfaces:**
- Produces a baseline record containing repository SHA, working-tree status, build commands, test commands, artifact paths, artifact hashes, current platform, current board witness, known limits, and evidence priority.

- [ ] **Step 1: Capture repository identity without mutation**

Run:

```powershell
git rev-parse HEAD
git status --short
git diff --check
git log -5 --oneline --decorate
```

Expected: record the exact SHA and whether proposal documents are untracked or modified. No files are changed.

- [ ] **Step 2: Re-run the current host verification lane**

Run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flows/host/run_host_tests.ps1 -RepoRoot D:/study/Software/Hollow-Gimbal
```

Expected: record the exact CTest count and pass/fail result. A failure is recorded with its first failing command and is not repaired in this task.

- [ ] **Step 3: Re-run the current AT32F415 reference build without flashing**

Run:

```powershell
cmake --preset fw-full-diag-carrier-bench-debug
cmake --build --preset build-fw-full-diag-carrier-bench-debug --parallel
```

Expected: record ELF, BIN, HEX, MAP paths, SHA-256, Flash/RAM usage, and compiler/toolchain identity. No debugger or board command is run.

- [ ] **Step 4: Write the baseline manifest**

Write `docs/evidence/2026-11-08-legacy-baseline.md` with these headings:

```text
# Legacy Baseline
## Repository identity
## Toolchain and build commands
## Host test result
## Firmware artifact identity
## Current platform and board facts
## Historical real-board evidence
## Known stale or invalid checks
## Evidence limits
## G0 exit decision
```

The evidence limits section must state that host tests and firmware builds do not prove physical PWM, independent phase current, temperature, or continuous motor acceptance.

- [ ] **Step 5: Verify the manifest is read-only evidence**

Run:

```powershell
git diff --check
git status --short
```

Expected: only the new evidence manifest and already-approved proposal documents are changed. No source or build file changes are present.

---

### Task 2: Build the AT32F415 resource ownership matrix

**Files:**
- Create: `docs/evidence/2026-11-08-at32f415-resource-matrix.md`
- Read: `platform/at/at32f415/generated/src/main.c`
- Read: `platform/at/at32f415/generated/src/wk_tmr.c`
- Read: `platform/at/at32f415/generated/src/wk_adc.c`
- Read: `platform/at/at32f415/generated/src/wk_dma.c`
- Read: `platform/at/at32f415/generated/src/wk_usart.c`
- Read: `platform/at/at32f415/generated/src/wk_spi.c`
- Read: `platform/at/at32f415/generated/src/at32f415_wk_config.c`
- Read: `platform/at/at32f415/generated/inc/at32f415_wk_config.h`
- Read: `platform/at/at32f415/ThemisPlatformDefaults.cmake`

**Interfaces:**
- Produces a source-backed resource matrix for G1 and a list of resources that require a new ADR before use.

- [ ] **Step 1: Record current initialized peripherals and order**

Inspect `main.c` and record the exact initialization order for GPIO, ADC1, DMA channels, USART1, SPI1, TMR1, TMR5, TMR3, and any omitted TMR2 path.

Expected: TMR2 is explicitly recorded as absent from the generated startup chain.

- [ ] **Step 2: Record timer ownership and trigger relationships**

Inspect `wk_tmr.c` and record TMR1 counter mode, ARR, CH1-CH4 roles, CC4 interrupt, deadtime, break, synchronization, TMR3 trigger behavior, and TMR5 timing and interrupt behavior.

Expected: every timer has exactly one current owner and every proposed V2 timer assignment is labeled `current`, `candidate`, or `unavailable`.

- [ ] **Step 3: Record ADC and DMA ownership**

Inspect `wk_adc.c` and `wk_dma.c` and record ADC injected sequence, trigger source, DMA request mappings, UART RX/TX assignments, ADC ordinary DMA assignments, and unused channels.

Expected: the matrix identifies whether SPI1 DMA can be added without colliding with ADC or UART DMA. Unknown facts remain `unknown` and list the required SDK source.

- [ ] **Step 4: Record IRQ priorities and owners**

Inspect `at32f415_wk_config.c` and application ISR definitions. Record ADC1, TMR1 CH4, TMR5, USART1, SysTick, and PendSV priorities, plus the owning source file for each handler.

Expected: all ISR nesting assumptions relevant to H0, H1, UART, and watchdog supervision are recorded.

- [ ] **Step 5: Write the resource matrix**

Write `docs/evidence/2026-11-08-at32f415-resource-matrix.md` with:

```text
# AT32F415 Resource Matrix
## Current startup ownership
## Timer ownership
## ADC and DMA ownership
## UART and SPI ownership
## IRQ priority map
## H0/H1 candidate execution boundaries
## TMR2 monotonic-time audit
## SPI DMA feasibility audit
## Resources that require an ADR before allocation
## G1 decision
```

Do not change generated code or allocate TMR2/DMA resources in this task.

---

### Task 3: Establish current timing and blocking-path facts

**Files:**
- Create: `docs/evidence/2026-11-08-timing-and-blocking-baseline.md`
- Read: `applications/tasks/motor_task.c`
- Read: `applications/periph/encoder.c`
- Read: `applications/periph/foc_ctrl.c`
- Read: `applications/algo/foc_config.h`
- Read: `applications/config/motor_task_config.h`
- Read: `docs/governance/Themis_工程维护日志.md`

**Interfaces:**
- Produces a distinction between configured timing, historical measured timing, and timing still requiring future measurement.

- [ ] **Step 1: Trace the fast control call graph**

Record the exact path from TMR1 CC4 through ADC completion to `foc_current_loop_step()`, including the divider and re-arm flag.

Expected: the record identifies whether ADC service latency can change effective sample cadence.

- [ ] **Step 2: Trace the H1 candidate path**

Record the path from TMR5 overflow through `encoder_update()` and `foc_outer_loop_step()`.

Expected: the record identifies whether the current encoder path can block in ISR context and which state is updated by H1.

- [ ] **Step 3: Classify control state ownership**

Build a table with:

```text
object | current owner | proposed H0/H1 owner | readers | writers | publication rule | risk
```

Include current integrators, modulation state, encoder state, velocity state, position state, calibration state, command target, fault latch, and telemetry snapshot.

Expected: any object with multiple writers is marked as a migration blocker.

- [ ] **Step 4: Classify timing facts**

For each frequency record configured value, source file, historical measured value, evidence source, and whether the value is a target or acceptance criterion.

Include PWM, ADC injected completion, fast FOC, TMR5, velocity loop, position loop, UART service, and telemetry.

Expected: historical 13.35 kHz injected completion and approximately 3.34 kHz bench FOC step are labeled historical evidence, not current remeasurement.

- [ ] **Step 5: Write timing and blocking baseline**

Write `docs/evidence/2026-11-08-timing-and-blocking-baseline.md` with:

```text
# Timing and Blocking Baseline
## Fast path call graph
## H1 candidate call graph
## H0/H1 state ownership
## Configured timing
## Historical measured timing
## Blocking operations
## Timing facts that require future instrumentation
## G1 decision
```

The document must state that no code changes or powered measurements were made by G0/G1.

---

### Task 4: Audit UART continuous-stream behavior

**Files:**
- Create: `docs/evidence/2026-11-08-uart-dma-audit.md`
- Read: `applications/tasks/shell_task.c`
- Read: `applications/tasks/app_shell_task.c`
- Read: `bsp/ports/bsp_console.c`
- Read: `bsp/ports/bsp_console.h`
- Read: `platform/at/at32f415/generated/src/wk_usart.c`
- Read: `platform/at/at32f415/generated/src/wk_dma.c`

**Interfaces:**
- Produces the UART transport constraints that UART Protocol v1 and G4 must implement.

- [ ] **Step 1: Map RX and TX buffers**

Record buffer sizes, DMA mode, indices, idle handling, TX queue behavior, and overflow counters.

Expected: the audit identifies whether current RX consumption relies exclusively on USART IDLE.

- [ ] **Step 2: Analyze continuous-stream and wraparound behavior**

Trace the receive logic for continuous bytes without IDLE, DMA wraparound, producer overtaking consumer, partial frame, and RX overflow.

Expected: each condition has a documented current behavior and a V2 requirement.

- [ ] **Step 3: Separate traffic classes**

Document policies for control commands, STOP/DISABLE/FAULT events, status telemetry, and debug logs.

Expected: safety traffic cannot be blocked by telemetry or debug output.

- [ ] **Step 4: Write UART audit**

Write `docs/evidence/2026-11-08-uart-dma-audit.md` with:

```text
# UART and DMA Audit
## Current USART1 configuration
## RX path
## TX path
## Continuous-stream risks
## Traffic-class policy
## Required Protocol v1 behavior
## G1 decision
```

No UART implementation changes are made in this task.

---

### Task 5: Define the G3-SAFE acceptance matrix

**Files:**
- Create: `docs/evidence/2026-11-08-g3-safe-acceptance-matrix.md`
- Read: `docs/superpowers/specs/2026-11-08-falconfoc-v2-runtime-adr-r1.md`
- Read: `applications/runtime/motor_fault_runtime.c`
- Read: `applications/runtime/motor_bench_watchdog.c`
- Read: `applications/periph/motor_drv.c`
- Read: `platform/at/at32f415/generated/src/wk_tmr.c`

**Interfaces:**
- Produces a no-power, register-only, and powered-test gate matrix for G3 and later.

- [ ] **Step 1: Enumerate G3-SAFE requirements**

Record the presence and evidence for default output disabled, enable guard, limits, command timeout, ADC freshness, immediate inhibit, fast heartbeat, foreground heartbeat, independent watchdog condition, and hardware protection visibility.

- [ ] **Step 2: Classify evidence**

Use exactly:

```text
source-confirmed
host-verified
register-only
unpowered-board-verified
powered-board-verified
not-available
```

Expected: source-level checks are never treated as physical protection proof.

- [ ] **Step 3: Define gates**

Write explicit gates:

- G3-NP for no-power build/register/sample validation
- G3-SAFE for the minimum safety chain without normal motor operation
- G3-PWR for powered testing only after G3-SAFE

- [ ] **Step 4: Write the matrix**

Write `docs/evidence/2026-11-08-g3-safe-acceptance-matrix.md` with:

```text
# G3-SAFE Acceptance Matrix
## Safety requirements
## Evidence classification
## G3-NP gate
## G3-SAFE gate
## G3-PWR gate
## Explicitly unavailable Board1 protections
## Required failure actions
```

No PWM enable, flash, debugger, serial command, or motor test is performed.

---

### Task 6: Reconcile G0/G1 evidence with the ADR

**Files:**
- Modify: `docs/superpowers/specs/2026-11-07-falconfoc-v2-platform-design.md`
- Modify: `docs/superpowers/specs/2026-11-08-falconfoc-v2-runtime-adr-r1.md`
- Modify: `docs/governance/Themis_工程维护日志.md`

**Interfaces:**
- Consumes Tasks 1-5 evidence records.
- Produces reconciled G0/G1 decisions and a dated maintenance-log entry. Live architecture remains unchanged until G2 approval.

- [ ] **Step 1: Compare evidence against the frozen ADR**

For every `candidate`, `unknown`, `not-available`, or conflict item, identify whether the ADR needs an amendment.

- [ ] **Step 2: Resolve only evidence-backed decisions**

Allowed decisions are keeping the current Legacy CPU class and FreeRTOS port, confirming or rejecting TMR2 as a time-source candidate, confirming or rejecting SPI DMA feasibility, confirming H0/H1 ownership, confirming G3-SAFE preconditions, and confirming UART continuous-stream constraints.

No control algorithm or board timing is changed.

- [ ] **Step 3: Update proposal documents**

Update only sections whose evidence changed. Keep unresolved items explicit rather than selecting a replacement resource informally.

- [ ] **Step 4: Append a concise maintenance-log entry**

Record G0/G1 date, baseline SHA, commands run, test/build result, evidence files, unresolved items, and the statement that no powered board action occurred.

- [ ] **Step 5: Final verification**

Run:

```powershell
git diff --check
git status --short
```

Expected: only evidence documents, proposal/ADR documents, and the maintenance-log entry are changed. No Legacy source, CMake, generated platform, or build configuration changes exist.

---

## G0/G1 Exit Criteria

G0/G1 is complete only when:

- Legacy firmware identity is reproducible.
- Current host and firmware verification results are recorded.
- Timer, ADC, DMA, SPI, UART, IRQ, and watchdog ownership is documented.
- TMR2 and SPI DMA remain explicitly candidate or rejected based on evidence.
- H0/H1 ownership is explicit and has no unresolved shared writers.
- Command, sensor, and telemetry publication rules are explicit.
- UART continuous-stream, overflow, and traffic-class behavior are documented.
- G3-SAFE has a three-stage acceptance matrix.
- Frontend scheduler budget, lateness, starvation, and skip behavior are documented in the ADR.
- No Legacy runtime or hardware behavior has changed.
- No powered-board action occurred.

## Handoff to G2-G4

After G0/G1 exit, the next approval must be explicit for G2 Core implementation. G2 may create the Core host target and voltage-open-loop model. G3 implementation is blocked until the G3-SAFE matrix is reviewed. G4 implementation is blocked until H0/H1 ownership, snapshot rules, and scheduler budget rules are accepted by tests or a new ADR.
