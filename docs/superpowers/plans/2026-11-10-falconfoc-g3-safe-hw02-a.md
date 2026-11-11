# FalconFOC V2 HW02-A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans or superpowers:subagent-driven-development to implement this plan task-by-task after the read-only preparation is accepted. HW02-B requires a separate hardware-test authorization.

**Goal:** Prepare a source-backed and electrically constrained test definition for Board1 reset safety without powering the motor stage or changing firmware control code.

**Architecture:** Keep HW02-A as a read-only preparation lane. Resolve the Board1 power dependency, TMR1 register state, DRV8311H truth-table mapping, and `nFAULT`/`nSLEEP` resistor network before any waveform observation. Treat `CCR=0` as an unresolved output state until timer polarity and counter behavior are derived and later measured.

**Tech Stack:** AT32F415 generated C and SDK headers, Board1 netlist/BOM/schematic, DRV8311H datasheet, AT32F415 official reference manual, Markdown evidence, host-side calculations, and a separately authorized oscilloscope/logic-analyzer test.

**Spec:** [`docs/evidence/2026-11-10-g3-safe-hw02-a-prep.md`](../../evidence/2026-11-10-g3-safe-hw02-a-prep.md) and [`docs/evidence/2026-11-10-g3-safe-hw01-resource-audit.md`](../../evidence/2026-11-10-g3-safe-hw01-resource-audit.md).

## Global Constraints

- HW02-A is read-only preparation. Do not modify Legacy ISR, generated TMR1/ADC code, WDT/WWDT configuration, board registers, root CMake or hardware files.
- HW02-A does not connect DCBUS, inject external 3.3 V, enable PWM, start the debugger, flash firmware or run a motor.
- HW02-B requires a separate authorization after HW02-A passes its prerequisites.
- G3-SAFE hardware acceptance and G3-PWR remain locked.
- `INHIBIT`, `ZERO_DUTY` and `ACTIVE_BRAKE` remain separate actions.
- A resistor connection is not automatically an effective safety gate. All claims about `nFAULT -> nSLEEP` require node-voltage assumptions and threshold evidence.

## Review Focus

1. **The MCU may depend on DRV AVDD.** The test plan must define a valid supply path before any reset waveform measurement.
2. **R7 couples nFAULT and nSLEEP.** The plan must calculate whether the network can cross the DRV sleep threshold under worst-case DCBUS and tolerance conditions.
3. **CCR zero may be an active low-side state.** The plan must derive and later measure actual INH/INL levels.
4. **Timer register API values are not enough.** The final matrix must include `oen`, channel enables, polarities, compare mode, idle state and Break fields.
5. **WDT must remain separate.** The first reset waveform test must not silently become a WDT configuration or timeout test.

---

### Task 1: Freeze the supply and measurement boundary

**Files:**
- Read: `docs/pcb docs/pcb 网表.tel`
- Read: `docs/pcb docs/pcb原理图.pdf`
- Read: `docs/pcb docs/pcb bom.xlsx`
- Read: `docs/code-wiki/hardware-and-drv8311h.md`
- Modify: `docs/evidence/2026-11-10-g3-safe-hw02-a-prep.md`

**Interfaces:**
- Consumes: Board1 power nets and the no-power restrictions from HW01.
- Produces: A supply and measurement boundary that can be reviewed before HW02-B.

- [ ] **Step 1: Map the supply path.**

  Record DCBUS, U4 `VIN_AVDD`, U4 `VM`, U4 `AVDD`, `DRV_3V3`, U3 VDD, U5 VDD, CSAREF, nFAULT pull-up and nSLEEP pull-up.

- [ ] **Step 2: Reject unsafe supply assumptions.**

  State explicitly whether DCBUS must be present for U3 to run. Do not use external 3.3 V injection until reverse-power paths through U4, CSAREF, nFAULT and nSLEEP have been reviewed.

- [ ] **Step 3: Define the measurement points.**

  Name the legal probe points, ground reference, current limit, motor-disconnect condition, and physical cutoff action. Distinguish “motor disconnected” from “power stage unpowered.”

- [ ] **Step 4: Verify the preparation record.**

  Run `git diff --check` and confirm that no step authorizes board power or firmware changes.

### Task 2: Build the TMR1 register matrix

**Files:**
- Read: `platform/at/at32f415/generated/src/wk_tmr.c`
- Read: `platform/at/at32f415/vendor/drivers/inc/at32f415_tmr.h`
- Read: `platform/at/at32f415/generated/inc/at32f415_wk_config.h`
- Modify: `docs/evidence/2026-11-10-g3-safe-hw02-a-prep.md`

**Interfaces:**
- Consumes: Generated TMR1 initialization and AT32F415 register definitions.
- Produces: A register-level snapshot definition for the reset-to-application window.

- [ ] **Step 1: Record output ownership.**

  List `oen`, `aoen`, `brken`, CH1/2/3 and complementary channel enable bits, polarity bits, compare modes, idle states, deadtime, ARR and CCR1-4.

- [ ] **Step 2: Record the initialization order.**

  Keep `wk_gpio_config() -> wk_tmr1_init() -> app_main_bootstrap()` explicit. Mark when PWM pins become mux outputs and when the application can first call `pwm_out_disable()`.

- [ ] **Step 3: Define the snapshot format.**

  The later hardware record must capture raw `TMR1->brk`, `TMR1->cctrl`, `TMR1->ctrl2`, `TMR1->cm1`, `TMR1->cm2`, `TMR1->pr`, `TMR1->c1dt/c2dt/c3dt` and the relevant GPIO/IOMUX registers.

- [ ] **Step 4: Verify no code change.**

  Confirm that this task only produces a matrix and does not change generated initialization.

### Task 3: Resolve the `CCR=0` logic before measurement

**Files:**
- Read: `platform/at/at32f415/generated/src/wk_tmr.c`
- Read: `platform/at/at32f415/vendor/drivers/inc/at32f415_tmr.h`
- Read: `docs/code-wiki/hardware-and-drv8311h.md`
- Read: `docs/pcb docs/drv8311.pdf`
- Modify: `docs/evidence/2026-11-10-g3-safe-hw02-a-prep.md`

**Interfaces:**
- Consumes: TMR1 output mode, polarity and DRV8311H 6x PWM truth table.
- Produces: A derived table mapping `CCR=0` to expected INH/INL combinations for each channel and counter direction.

- [ ] **Step 1: Derive the raw PWM signal.**

  Use the configured PWM mode, ARR, CCR, center-aligned count and compare semantics to derive the raw CH1/2/3 output at `CCR=0`.

- [ ] **Step 2: Apply polarity and complementary output rules.**

  Apply `oc_polarity`, `occ_polarity`, channel enable, complementary enable and idle states. Do not infer final pins from the compare value alone.

- [ ] **Step 3: Map to DRV truth-table states.**

  For each phase, classify the result as Hi-Z, low-side conduction, high-side conduction or unresolved transition. Identify any active-brake interpretation.

- [ ] **Step 4: Define the hardware observation.**

  HW02-B must observe `NRST` and all six INH/INL lines against one common trigger. Separate captures cannot prove simultaneous safety.

### Task 4: Calculate the R7 `nFAULT -> nSLEEP` network

**Files:**
- Read: `docs/pcb docs/pcb 网表.tel`
- Read: `docs/pcb docs/pcb bom.xlsx`
- Read: `docs/pcb docs/drv8311.pdf`
- Modify: `docs/evidence/2026-11-10-g3-safe-hw02-a-prep.md`

**Interfaces:**
- Consumes: R6, R7, R10, D2, C9 and DRV8311H nSLEEP threshold requirements.
- Produces: A node-voltage table with explicit DCBUS, clamp, tolerance and input-current assumptions.

- [ ] **Step 1: Draw the DC network.**

  Represent DCBUS through R6, nFAULT through R7/R10, D2 clamp, C9 and the DRV nSLEEP input. Keep the open-drain behavior explicit.

- [ ] **Step 2: Calculate nominal and boundary cases.**

  Calculate normal nFAULT-high, nFAULT-low, minimum DCBUS and maximum DCBUS cases. Include resistor tolerance and D2 clamp assumptions.

- [ ] **Step 3: Separate electrical coupling from safety effectiveness.**

  Record that R7 proves a physical coupling path. It does not prove that nSLEEP crosses the DRV sleep threshold or remains asserted during automatic retry.

- [ ] **Step 4: Define the later measurement.**

  HW02-B must observe nFAULT and nSLEEP simultaneously during startup and a controlled fault condition. The fault source and power limits require separate approval.

### Task 5: HW02-A entry and exit review

**Files:**
- Modify: `docs/evidence/2026-11-10-g3-safe-hw02-a-prep.md`
- Modify: `docs/governance/Themis_工程维护日志.md`
- Create: a later HW02-B authorization record only after review

**Interfaces:**
- Consumes: Tasks 1–4 outputs.
- Produces: A binary decision that either keeps HW02-B locked or permits a separate request for it.

- [ ] **Step 1: Require the preparation gates.**

  Power path, reverse-power risk, TMR1 matrix, `CCR=0` derivation and R7 calculations must all be complete.

- [ ] **Step 2: Keep WDT separate.**

  Do not enable WDT or change timeout configuration as part of HW02-B startup waveform preparation.

- [ ] **Step 3: Review the evidence class.**

  HW02-A results remain `source-confirmed` or calculation-only. They cannot be promoted to `unpowered-board-verified` or `powered-board-verified` without actual measurements.

- [ ] **Step 4: Decide the next lane.**

  If any preparation item is missing, keep HW02-B, G3-SAFE hardware and G3-PWR locked. If all are complete, issue a separate, narrowly scoped HW02-B authorization.

## Exit Criteria

- [ ] DCBUS, DRV_3V3 and U3 power dependency is explicit.
- [ ] External 3.3 V injection is either ruled out or separately justified by reverse-power analysis.
- [ ] TMR1 register matrix contains all output, polarity, compare, Break and enable fields.
- [ ] `CCR=0` is mapped to a derived INH/INL state or explicitly marked unresolved.
- [ ] R7 coupling is represented with voltage and current assumptions.
- [ ] HW02-B remains separately gated.
- [ ] G3-SAFE final hardware acceptance remains not passed.
- [ ] G3-PWR remains locked.
