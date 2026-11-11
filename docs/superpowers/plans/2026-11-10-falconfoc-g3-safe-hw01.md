# FalconFOC V2 G3-SAFE-HW01 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans or superpowers:subagent-driven-development to implement this plan task-by-task after a separate hardware-integration approval.

**Goal:** Establish a source-backed AT32F415/Board1 safety-chain design that can be reviewed before any WDT, PWM inhibit, TMR1 Break, driver-fault, or powered-board implementation is authorized.

**Architecture:** Keep the G3-SAFE Host contract as the policy owner. Add a thin AT32F415 hardware execution seam later, with an explicit final output gate, a separately selected watchdog, and a separately verified driver or timer hardware inhibit path. Treat the current Board1 `nFAULT`/`nSLEEP` topology and TMR1 startup ordering as hardware facts, not as software abstractions.

**Tech Stack:** AT32F415 generated C, Artery peripheral SDK, existing `g3/safe/` Host C11 contract, Markdown evidence, source-level resource audit, unpowered-board instrumentation, and controlled powered tests only after a new authorization.

**Spec:** [`docs/evidence/2026-11-10-g3-safe-hw01-resource-audit.md`](../../evidence/2026-11-10-g3-safe-hw01-resource-audit.md), plus [`docs/evidence/2026-11-08-g3-safe-acceptance-matrix.md`](../../evidence/2026-11-08-g3-safe-acceptance-matrix.md) and [`docs/superpowers/specs/2026-11-08-falconfoc-v2-runtime-adr-r1.md`](../specs/2026-11-08-falconfoc-v2-runtime-adr-r1.md).

## Global Constraints

- Current Board1 has UART as the formal external interface. CAN-FD, CANopen and EtherCAT are outside this hardware gate.
- Legacy keeps the existing FreeRTOS `ARM_CM3` port, soft-float ABI and current CPU class.
- V2 Core and G3-SAFE policy remain independent of RTOS and vendor registers.
- Do not modify `applications/`, `platform/`, `bsp/`, `subsys/`, `components/`, root CMake, Legacy ISR, ADC/PWM runtime, WDT configuration or board registers in the read-only HW01 lane.
- `INHIBIT`, `ZERO_DUTY` and `ACTIVE_BRAKE` are separate output actions. `ZERO_DUTY` is never an implicit substitute for output inhibition.
- A source or Host result cannot be reported as unpowered-board or powered-board safety evidence.
- G3-PWR remains locked until a separately approved hardware safety matrix is complete.

## Review Focus

1. **TMR1 starts before the application safety state.** A source test must reject any future initialization that enables TMR1 main output or six PWM channels before an explicit inhibit gate.
2. **Zero compare can have brake semantics.** Tests and review must distinguish disabled channels, zero-duty active clamp and active brake.
3. **A timer Break register is not a board Break path.** The plan must require a physical signal route or explicitly record the protection as unavailable.
4. **Host feed permission is not WDT proof.** WDT/WWDT clock, timeout, reset flag and reset-time output state need independent evidence.
5. **DRV nFAULT is not software-visible on Board1.** No code may claim an MCU fault input or sleep control until the netlist and pin measurement show one.

---

### Task 1: Freeze the hardware evidence set

**Files:**
- Read: `docs/pcb docs/pcb原理图.pdf`
- Read: `docs/pcb docs/pcb 网表.tel`
- Read: `docs/pcb docs/pcb bom.xlsx`
- Read: `docs/pcb docs/drv8311.pdf`
- Read: `platform/at/at32f415/generated/src/main.c`
- Read: `platform/at/at32f415/generated/src/wk_tmr.c`
- Read: `platform/at/at32f415/generated/src/wk_gpio.c`
- Read: `platform/at/at32f415/generated/inc/at32f415_wk_config.h`
- Read: `platform/at/at32f415/generated/inc/at32f415_conf.h`
- Read: `applications/periph/motor_drv.c`
- Read: `platform/at/at32f415/vendor/drivers/inc/at32f415_tmr.h`
- Read: `platform/at/at32f415/vendor/drivers/inc/at32f415_wdt.h`
- Read: `platform/at/at32f415/vendor/drivers/inc/at32f415_wwdt.h`
- Modify: `docs/governance/Themis_工程维护日志.md`

**Interfaces:**
- Consumes: Current local hardware artifacts and the G3-SAFE acceptance labels.
- Produces: A dated evidence record with source-confirmed, register-only and not-available facts.

- [ ] **Step 1: Record the exact source set and current commit.**

  Include the artifact paths, current `HEAD`, and the no-power boundary in the evidence record.

- [ ] **Step 2: Cross-check the DRV nets.**

  Record `INH*`, `INL*`, `NFAULT`, `NSLEEP`, `SOA/SOB/SOC` and `VBUS` from the netlist, then compare them with the generated pin header and `motor_drv.c` mapping.

- [ ] **Step 3: Append the maintenance-log entry.**

  State that HW01 is a read-only resource audit, that the boot gate is not yet proven, and that G3-SAFE hardware and G3-PWR remain locked.

- [ ] **Step 4: Verify the record.**

  Run `git diff --check` and confirm every evidence label has a corresponding source path or an explicit missing-evidence statement.

### Task 2: Define the reset-to-inhibit PWM gate

**Files:**
- Read: `platform/at/at32f415/generated/src/main.c`
- Read: `platform/at/at32f415/generated/src/wk_tmr.c`
- Read: `platform/at/at32f415/generated/src/wk_gpio.c`
- Read: `applications/periph/motor_drv.c`
- Read: `applications/tasks/motor_fsm.c`
- Future modify only after approval: a thin AT32F415 output-gate owner, exact path to be chosen after this task

**Interfaces:**
- Consumes: G3-SAFE output actions and current TMR1 register ownership.
- Produces: A hardware-facing rule that `INHIBIT` is the first state after reset and the final check before CCR submission.

- [ ] **Step 1: Draw the current boot order.**

  Show `wk_gpio_config()` through `wk_tmr1_init()` and `app_main_bootstrap()`. Mark the first point where TMR1 pins become mux outputs and the first point where application state can call `pwm_out_disable()`.

- [ ] **Step 2: State the required safe default.**

  Require either a pre-TMR1 GPIO safe state, a TMR1 configuration that leaves `oen` and channels disabled until the guard passes, or an equivalent external gate. The implementation must not rely on zero compare alone.

- [ ] **Step 3: Specify the final commit rule.**

  The final PWM write path must re-check the safety latch immediately before enabling or updating the six channels. A gate failure must produce `INHIBIT`, not stale CCR values, zero-duty clamp or active brake.

- [ ] **Step 4: Define the unpowered test.**

  With no motor bus and no normal power output, capture reset release, PWM pin mux/output levels, TMR1 `brk.oen`, channel enable bits and the first application safety decision. Expected result is no effective gate-enable window before the explicit guard.

### Task 3: Close the hardware Break and driver-fault topology

**Files:**
- Read: `docs/pcb docs/pcb 网表.tel`
- Read: `docs/pcb docs/pcb原理图.pdf`
- Read: `docs/pcb docs/drv8311.pdf`
- Read: `platform/at/at32f415/vendor/drivers/inc/at32f415_tmr.h`
- Read: `platform/at/at32f415/generated/src/wk_tmr.c`
- Future modify only after approval: board revision or external gate design documents

**Interfaces:**
- Consumes: DRV8311H `nFAULT`/`nSLEEP` behavior and AT32F415 TMR1 Break fields.
- Produces: A binary decision that Board1 either has a physical independent shutdown path or does not.

- [ ] **Step 1: Verify the current absence of a board route.**

  Confirm that `NFAULT` and `NSLEEP` have no U3 endpoint. Also check the official QFN32 pin table. PA6 is the available `TMR1_BRK` alternate, but the current board uses PA6 as IA, so the Break candidate conflicts with the existing current-sense channel.

- [ ] **Step 2: Keep register-only and board-connected facts separate.**

  Do not turn `brken`, `brkv`, `aoen` or `brkif` into a hardware safety claim without a physical signal source and a measured active polarity.

- [ ] **Step 3: Define the required next hardware option.**

  Select one explicit path for a future board revision. The candidates are DRV `nFAULT` to a true timer Break input, DRV `nFAULT` to an independently latched external gate, or a separate driver-enable/shutdown control. The current Board1 is recorded as unavailable until one is implemented and verified.

- [ ] **Step 4: Define fault timing evidence.**

  For the selected path, measure signal assertion to bridge inhibit, power-up default, fault release, and re-enable guard. DRV internal automatic retry must not be treated as MCU fault recovery.

### Task 4: Select and verify the watchdog

**Files:**
- Read: `platform/at/at32f415/vendor/drivers/inc/at32f415_wdt.h`
- Read: `platform/at/at32f415/vendor/drivers/inc/at32f415_wwdt.h`
- Read: `platform/at/at32f415/vendor/drivers/src/at32f415_wdt.c`
- Read: `platform/at/at32f415/vendor/drivers/src/at32f415_wwdt.c`
- Read: [AT32F415 official Reference Manual](https://www.arterytek.com/download/RM/RM_AT32F415_V2.05_EN.pdf), clock and reset chapters
- Read: [AT32F415 official Datasheet](https://arterytek.com/download/DS/DS_AT32F415_V2.02_EN.pdf), QFN32 pin table
- Future modify only after approval: WDT owner and generated configuration

**Interfaces:**
- Consumes: `foc_safe_watchdog_feed_allowed()` and the independent Fast/Foreground heartbeat contract.
- Produces: A selected hardware watchdog, timeout equation, reset-cause capture and reset-output rule.

- [ ] **Step 1: Verify clock and timeout equations.**

  Use the official AT32F415 Reference Manual as the source of truth. It states that WDT uses the LICK oscillator, whose frequency is 30–60 kHz, while WWDT uses the main clock. Record the selected prescaler, reload range, tolerance and maximum supervision interval. For WWDT, also record PCLK1, window-open timing and early/late service behavior.

- [ ] **Step 2: Select the independent candidate.**

  Select WDT as the first candidate because the device manual confirms its LICK clock is independent of the system main clock. Keep WWDT as a fallback only if a later device or product constraint requires it. Do not enable either peripheral in this task.

- [ ] **Step 3: Define reset cause handling.**

  Capture `CRM_WDT_RESET_FLAG` or `CRM_WWDT_RESET_FLAG` before any generic reset-flag clear. Persist or expose the cause without allowing ordinary `FAULT_LATCHED` to force a reset.

- [ ] **Step 4: Verify reset-time output behavior without the power stage.**

  Let the selected watchdog expire while the board has no motor bus. Confirm reset duration, GPIO/TMR1 defaults, channel enable state, and that boot cannot re-enable PWM before the safety guard.

### Task 5: Build the safety timing budget

**Files:**
- Read: `platform/at/at32f415/generated/src/wk_tmr.c`
- Read: `platform/at/at32f415/generated/src/wk_adc.c`
- Read: `applications/tasks/motor_task.c`
- Read: `applications/periph/motor_drv.c`
- Read: `g3/safe/include/foc_safe.h`
- Read: `g3/np/include/foc_device.h`
- Future modify only after approval: timing instrumentation at the hardware seam

**Interfaces:**
- Consumes: H0 safety gate, Device output actions and TMR1/ADC trigger ownership.
- Produces: A bounded chain from sample/fault observation to physical inhibit.

- [ ] **Step 1: Write the static chain.**

  Use `TMR1 CC4 -> software ADC trigger -> ADC1 IRQ -> fast check -> Core -> CCR commit` as the current Legacy baseline. Mark `CCR4=3740` and all model-only timing values as non-physical evidence.

- [ ] **Step 2: Assign measurement points.**

  Define timestamp or probe points for CC4, ADC completion, H0 entry, final gate decision, channel-disable write and INH/INL/OUT transition.

- [ ] **Step 3: Define deadline behavior.**

  A stale/invalid sample, missed fast deadline or new fault after the second gate must cause `INHIBIT`. No foreground task, telemetry path or UART response may be required to complete the emergency action.

- [ ] **Step 4: Produce the timing record.**

  Report minimum, maximum and worst-case observed values, test clock/prescaler, probe points and whether the result is `unpowered-board-verified` or `powered-board-verified`.

### Task 6: Hardware safety entry and exit review

**Files:**
- Modify: `docs/evidence/2026-11-10-g3-safe-hw01-resource-audit.md`
- Modify: `docs/governance/Themis_工程维护日志.md`
- Create: a later hardware acceptance record with a new date and authorization

**Interfaces:**
- Consumes: Tasks 1–5 evidence and the existing G3-SAFE acceptance matrix.
- Produces: A signed decision that either keeps G3-PWR locked or authorizes the next controlled lane.

- [ ] **Step 1: Require all P0 gates.**

  The reset-to-inhibit window, an independent fault shutdown route, and a verified watchdog/reset contract must all be closed. A host PASS cannot substitute for any one of them.

- [ ] **Step 2: Separate software and hardware conclusions.**

  Record independent statuses for Host contract, source audit, register behavior, unpowered board behavior and powered behavior.

- [ ] **Step 3: Authorize only the next smallest lane.**

  If all evidence is complete, authorize the narrowest next test sequence. Otherwise keep G3-SAFE hardware and G3-PWR locked and list the exact missing artifact.

- [ ] **Step 4: Re-run repository checks.**

  Run `git diff --check`, the FalconFOC source contract, the relevant host tests and the current firmware build. Do not expand a hardware acceptance decision from a passing software gate.

## Exit Criteria

- [ ] Board1 reset-to-inhibit behavior is demonstrated or an external gate is documented and verified.
- [ ] TMR1 Break is either physically wired and measured or explicitly excluded from the current board safety claim.
- [ ] DRV `nFAULT` and `nSLEEP` software visibility is accurately represented. No phantom MCU input exists in the design.
- [ ] WDT/WWDT selection, clock, timeout, reset-cause and reset-output rules are documented and tested without the power stage.
- [ ] Sample-to-inhibit and final-gate timing has measured bounds.
- [ ] G3-SAFE hardware acceptance is still blocked if any of the above is missing.
- [ ] G3-PWR remains locked until a separate powered-board authorization is issued.
