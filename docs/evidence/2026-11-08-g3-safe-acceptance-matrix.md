# G3-SAFE Acceptance Matrix

- Audit date: 2026-11-08
- Scope: design gate definition only
- No PWM enable, debugger, flash, serial command, or motor test performed.

## Safety requirements

| Requirement | Current source/evidence | Evidence class at G1 | G3-SAFE requirement |
| --- | --- | --- | --- |
| Power output disabled at boot | TMR1 channels initialize with zero compare and application output helpers exist | source-confirmed | Verify output state and inhibit latch before any powered test |
| Explicit enable guard | Motor FSM and PWM enable helpers exist | source-confirmed | Require READY state, valid samples, valid VBUS, and no fault latch |
| Current and voltage limits | Fast and task-level checks exist in current runtime | source-confirmed / host-verified | Re-express in H0 safety contract and test threshold behavior |
| Command freshness timeout | Current bench watchdog and runtime paths exist | source-confirmed | Add V2 command timeout owned by fast safety domain |
| ADC validity and freshness | ADC sequence and injected completion path exist | source-confirmed | Reject stale or incomplete sample before control output |
| Immediate output inhibit | `pwm_out_disable()` and fault paths exist | source-confirmed | Prove final H0 gate can inhibit output without foreground help |
| Fast heartbeat | Current bench watchdog records runtime activity | source-confirmed | Separate fast heartbeat from foreground heartbeat |
| Foreground heartbeat | Current task/runtime supervision exists | source-confirmed | Supervisor must publish bounded progress and feed WDT only when both domains progress |
| Independent watchdog | MCU WDT capability exists, current application usage requires audit | not-available at G1 | Confirm exact WDT configuration and reset behavior before powered V2 |
| Hardware break | Current TMR1 break is disabled | source-confirmed / not-available | Do not claim hardware break protection for Board1 |
| Driver fault visibility | Current Board1 fault visibility is incomplete | not-available | Treat driver protection as external unless independent signal is verified |
| Safe recovery | Current FSM has fault and clear behavior | source-confirmed | `FAULT_CLEAR` returns to disabled/idle and never auto-runs |
| Flash write safety | Current deferred-save paths require final-state audit | source-confirmed risk | Final write point rechecks state and output inhibit |

## Evidence classification

Use these exact labels:

- `source-confirmed`
- `host-verified`
- `register-only`
- `unpowered-board-verified`
- `powered-board-verified`
- `not-available`

A source-confirmed or host-verified result does not prove physical driver protection, PWM waveform safety, independent current, or thermal safety.

## G3-NP gate

G3-NP permits only:

- Core host tests
- firmware configuration and build
- register and generated-source inspection
- unpowered board initialization checks
- ADC sample path checks without normal power output
- timing snapshot validation without motor operation

G3-NP exit requires the build and source contracts to pass and output inhibit to remain active.

## G3-SAFE gate

Before any real PWM output or motor test, the following must be demonstrated in a controlled, non-normal-operation lane:

- boot output disabled
- explicit output enable guard
- valid VBUS requirement
- valid ADC sample requirement
- fast current/voltage checks
- command freshness timeout
- immediate output inhibit
- fast heartbeat
- foreground heartbeat
- independent watchdog decision
- fault latch and non-automatic recovery
- final Flash safety guard
- UART/transport failure behavior

G3-SAFE is a software and register-level gate. It does not replace independent PWM, phase-current, temperature, or hardware driver measurements.

## G3-PWR gate

G3-PWR is allowed only after G3-SAFE and requires a separate powered-board approval. It must start with:

1. no-motion boot check
2. static ADC and VBUS check
3. fixed-vector low-energy check
4. controlled stop/disable check
5. bounded runtime timeout
6. immediate manual power cutoff availability

G3-PWR cannot be entered by a UART command, protocol ACK, host test, or software trace alone.

## Explicitly unavailable Board1 protections

At G1, the following are not accepted as available Board1 protections:

- enabled TMR1 hardware break path
- MCU-visible DRV fault input
- independently measured phase current
- independently observed PWM/DRV/phase waveforms
- automatic proof of thermal safety

## Required failure actions

| Failure | Required action |
| --- | --- |
| command expires | inhibit output or enter the predefined safe state |
| ADC sample invalid | do not consume stale sample, then latch after bounded count |
| fast deadline miss | inhibit or degrade according to explicit policy, record overrun |
| foreground stall | stop feeding WDT and allow hardware reset |
| UART overflow | discard incomplete frame, preserve safety channel, report transport fault |
| Flash write failure | retain previous record and report persistence fault |

## Gate decision

G3 is blocked from powered testing until G3-SAFE is implemented and reviewed. G3-NP may proceed with no-power build, register, sample, Core, and timing validation.
