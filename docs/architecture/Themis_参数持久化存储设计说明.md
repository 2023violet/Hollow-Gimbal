# FalconFoc Parameter Persistence

## 1. Live Owner

FalconFoc keeps parameter persistence in the application layer on purpose.

Current live chain:

`applications/algo/foc_param.* -> bsp/board/bsp_flash.* -> components/utils/tiny_param/*`

This repository does not assemble the full Themis `device/` / `drivers/`
storage runtime. What must align is the persistence contract, not the old
runtime structure.

## 2. Current Groups

FalconFoc currently persists three independent A/B groups:

- `motor`
- `enc`
- `sys`

Each group is versioned, CRC-protected, and stored independently so one group
can fail without blocking the others.

## 3. Formal Semantics

Current live semantics:

- blank slot is a normal first-boot state
- version mismatch is recoverable
- CRC failure is isolated to the current group
- flash writes must never happen in ISR
- save requests may be latched in ISR or shell, but the actual write runs in
  task context
- `store save <...>` is a deferred request surface, not a direct flash-write
  surface
- shell must not enter flash erase/write from `running` or `calib` state

## 4. Layer Boundary

Layer roles are fixed as:

- `tiny_param`
  - header packing
  - CRC
  - slot save/load/check/erase primitives
- `bsp_flash`
  - board-owned flash read/write/erase backend
- `foc_param`
  - slot layout
  - payload schema
  - version owner
  - recovery policy
  - migration placeholder owner
  - `motor_dev <-> payload` field mapping

## 5. A/B Strategy

Current FalconFoc policy:

1. Check both slots.
2. Select active slot and write slot.
3. Read current sequence through the flash read contract.
4. Stamp the pending payload with current sequence before CRC comparison.
5. Skip write if payload is unchanged.
6. Otherwise increment sequence and write the stale slot.

This is the current formal write-avoidance and rollback contract.

## 6. Migration Rule

When a payload schema changes:

1. Bump the corresponding `_VER_*`.
2. Keep the old payload struct as a legacy migration type.
3. Load defaults or run explicit migration logic.
4. Re-save into the new schema.

Do not:

- dereference flash memory directly from application code
- reimplement CRC/header logic in shell or tasks
- perform migration decisions in ISR

## 7. Current Startup Interaction

Live usage expectation:

- load may happen during bring-up before task creation
- save is task-context only
- shell commands may trigger deferred save requests
- shell `store save <motor|enc|sys|all>` only sets `param_save_req`
- the real flash write is drained by the safe RTOS task
- shell `store save ...` and `store erase` require `axis=idle`
- shell `store load motor/sys` runs the shared application-owned
  restore-after-load path
- that restore-after-load step refreshes only derived runtime state and clears
  transient setpoints
- startup bring-up uses the same restore semantics on the initial FSM
  `IDLE` entry, so persisted `ctrl/input` policy is preserved across reboot
- persisted PID tuning remains authoritative and must not be regenerated during
  restore finalization
- shell `store load enc` remains independent and does not clear active runtime
  motion setpoints
- restart verification path is:
  - `store save sys`
  - reboot
  - `cfg show`
  - `store status`
