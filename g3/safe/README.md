# FalconFOC V2 G3-SAFE

`g3/safe/` is the first no-power software safety slice. It defines the state, freshness, heartbeat, watchdog-permission, Flash-guard, and output-inhibit contract without accessing AT32 registers or changing the Legacy runtime.

## Contract

- `BOOT`, `DISABLED`, `READY`, `RUNNING`, and `FAULT_LATCHED` are explicit states.
- Only `RUNNING` can pass a valid `FOC_DEVICE_OUTPUT_UPDATE` request.
- `DISABLED`, `READY`, invalid state, stale health, and fault latch produce `FOC_DEVICE_OUTPUT_INHIBIT`.
- Fault clear returns to `DISABLED` and never starts the output automatically.
- Command and sample freshness use explicit monotonic microseconds.
- Fast and foreground heartbeat windows are independent.
- Watchdog permission requires both heartbeat domains to be healthy and, while running, fresh command and sample observations.
- Flash permission requires disabled state, no fault latch, and an explicit inhibit action.

The module returns permission decisions only. It does not feed a hardware WDT, write Flash, disable a timer, or claim a bridge shutdown latency.

## Fault-injection test sequence

```text
init -> boot complete -> arm -> start
    -> observe command/sample/fast heartbeat/foreground heartbeat
    -> supervisor tick
    -> gate output
```

The test injects command timeout, sample timeout, invalid samples, fast heartbeat loss, foreground heartbeat loss, invalid output actions, fault-clear attempts, and Flash/WDT guard violations.

## Build and test

```text
cmake -S g3/safe -B build/g3-safe -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/g3-safe --clean-first --parallel
ctest --test-dir build/g3-safe --output-on-failure
```

This is a host-only contract. G3-SAFE hardware acceptance still requires WDT selection and reset evidence, output-inhibit register evidence, timing evidence, and the safety matrix's applicable board measurements.
