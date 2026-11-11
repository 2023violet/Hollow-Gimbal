# FalconFOC V2 G3-SAFE First Host Validation

Date: 2026-11-09

## Scope

This record covers the first no-power G3-SAFE software slice. It defines and tests safety state, fault latch, command/sample freshness, fast and foreground heartbeat, watchdog permission, Flash permission, and output gating. It does not modify Legacy ISR code, ADC/PWM runtime, firmware CMake, WDT registers, Flash backend, debugger state, UART behavior, or board power.

Implementation files are under `g3/safe/` and are built by an independent C11 host project.

## Contract behavior

- BOOT always inhibits.
- DISABLED and READY cannot pass an enabled PWM update.
- Only RUNNING can pass a finite, normalized `FOC_DEVICE_OUTPUT_UPDATE` request.
- Any fault latch transitions to FAULT_LATCHED and inhibits output.
- Ordinary start, arm, command, or sample operations cannot clear a latch.
- Fault clear resets observations and returns to DISABLED.
- Command and sample expiration use elapsed monotonic microseconds, independent of sample count.
- Invalid samples latch after the configured bounded threshold.
- Watchdog permission requires both heartbeat domains. Running state additionally requires fresh command and sample observations.
- A normal FAULT_LATCHED state may continue watchdog permission while both safety heartbeats remain healthy. `FATAL_RESET_REQUIRED` is a separate explicit fault bit that denies watchdog permission.
- A currently invalid sample immediately inhibits the output decision even before the configured invalid-sample latch threshold is reached.
- Flash permission requires DISABLED, no fault latch, and an explicit INHIBIT action.

## Fault-injection coverage

`test-foc-safe-contract` covers boot, disabled, ready, and running output decisions, valid healthy update permission, command timeout, sample timeout, fast heartbeat loss, foreground heartbeat loss, bounded invalid-sample latching, immediate inhibit for a sub-threshold invalid sample, 32-bit timestamp and sequence wrap, invalid action and duty rejection, normal fault-state watchdog permission, fatal-reset watchdog denial, Flash permission, latch priority, final-gate fault injection, and non-automatic recovery.

## Results

The independent G3-SAFE CMake project was clean-built with Clang 22.1.8.

```text
G3-SAFE CTest       1/1 PASS
```

The G2 Core, G3-NP, Legacy Host, and AT32F415 bench build lanes remain passing after this host-only addition.

## Evidence boundary

This is `host-verified` software contract evidence. It does not prove:

- WDT or WWDT selection and reset timing
- hardware bridge inhibit latency
- TMR1 break behavior
- PWM shadow-register behavior
- ADC sample-to-PWM latency
- current or temperature protection
- driver fault visibility
- Flash erase/program recovery
- any powered-board or motor behavior

G3-SAFE is not yet finally accepted. G3-PWR remains locked.

The G3-SAFE files are included in the FalconFOC source contract and dedicated `falconfoc` quality scope. This confirms source/layout coverage, not hardware safety acceptance.
