# FalconFOC V2 G3-SAFE No-Power Safety Contract Plan

> Scope: first G3-SAFE software slice only. No Legacy ISR, ADC/PWM chain, firmware image, debugger, flash, UART command, PWM enable, or powered-board action is authorized.

**Goal:** Define and host-test the minimum independent safety contract that prevents effective PWM submission when state, command freshness, sample freshness, heartbeat health, or fault-latch conditions are unsafe.

**Architecture:** A new `g3/safe/` host-only project owns a small safety state machine and supervisor. It consumes an explicit monotonic time, command/sample observations, heartbeat updates, and a Device PWM request. It returns either an explicit inhibit request or an allowed request decision. It does not access registers and does not feed a real watchdog or write Flash.

## Contract decisions

### Safety state machine

```text
BOOT -> DISABLED -> READY -> RUNNING
                         \-> FAULT_LATCHED
RUNNING -> DISABLED
any state -> FAULT_LATCHED
FAULT_LATCHED --clear--> DISABLED
```

- `BOOT` always inhibits output.
- `DISABLED` and `READY` cannot submit an enabled PWM update.
- Only `RUNNING` can pass an update request.
- Any fault latch immediately inhibits output.
- Fault clear returns to `DISABLED` and never auto-runs.
- Ordinary arm, start, or command operations cannot clear a fault latch.

### Independent freshness

- Command freshness uses the monotonic `now_us` domain and a configured command timeout.
- Sample freshness uses the same monotonic domain and a configured sample timeout.
- Heartbeat freshness uses independent fast and foreground deadlines.
- No timeout decision depends on ADC sample count or foreground loop count.

### Heartbeat and watchdog contract

- Fast heartbeat is updated only by the simulated fast domain.
- Foreground heartbeat is updated only by the simulated foreground supervisor.
- Watchdog feed permission requires both heartbeat windows to be healthy. In RUNNING it also requires valid command and sample freshness. A normal FAULT_LATCHED state may continue feeding while safety supervision remains healthy. Only an explicit `FATAL_RESET_REQUIRED` fault denies feeding.
- The first slice exposes a feed-permission decision only. It does not select or configure AT32 WDT/WWDT registers.

### Flash contract

- Flash begin/commit permission requires `DISABLED` state and an explicit inhibited output action.
- Flash permission is denied in `RUNNING`, `READY`, `BOOT`, or any fault-latched state.
- The first slice records permission only. It performs no erase, program, or recovery operation.

## Host API

Create `g3/safe/include/foc_safe.h` and `g3/safe/src/foc_safe.c` with:

- safety states and fault bits
- configuration containing command/sample/heartbeat timeouts
- caller-owned safety context
- `init`, boot completion, arm, start, disable, fault latch, and fault clear operations
- command and sample observation functions
- fast and foreground heartbeat update functions
- supervisor tick and watchdog-feed permission functions
- output gate function that converts unsafe decisions to `FOC_DEVICE_OUTPUT_INHIBIT`
- Flash guard function

The API uses `uint32_t now_us` for the host monotonic domain. Counter wraparound is out of scope for this first contract and must be addressed before production Device integration.

## Fault-injection tests

1. BOOT, DISABLED, and READY reject update requests with INHIBIT.
2. A valid RUNNING context with fresh command, sample, both heartbeats, and an update request permits the request.
3. Invalid state and unknown action always inhibit.
4. Command timeout is detected by elapsed monotonic time even when sample count continues.
5. Sample timeout is detected by elapsed monotonic time even when command updates continue.
6. Fast heartbeat stall latches a fault and inhibits output.
7. Foreground heartbeat stall denies watchdog feed and latches a fault.
8. A fault latch cannot be overwritten by start, arm, or command observation.
9. Fault clear returns to DISABLED and does not auto-enable output.
10. Flash guard denies RUNNING or non-inhibited output actions and permits only disabled/inhibited state.
11. Watchdog feed permission is false for every unhealthy combination and true only for the complete healthy contract.
12. A sub-threshold invalid sample immediately inhibits the current output decision.
13. Timestamp and sequence wraparound use bounded unsigned modular ordering.
14. A fault introduced after a permitted gate decision is rejected at the next final gate check.

## Build boundary

`g3/safe/CMakeLists.txt` is an independent C11 host project. It may link the existing G3-NP Device request types, but it does not join the root firmware graph and does not call G3-NP simulator state or Legacy code.

## Exit criteria

- Safety state, latch, freshness, heartbeat, watchdog permission, and Flash guard contracts are documented in public headers.
- All fault-injection tests pass.
- Invalid states cannot produce an update decision.
- A fault cannot be cleared by an ordinary command.
- Host tests prove time-based timeout behavior independent of sample count.
- No real WDT, Flash, ADC, PWM, ISR, or board path is changed.
- G3-SAFE remains a software contract and host evidence stage, not a powered-board acceptance.
