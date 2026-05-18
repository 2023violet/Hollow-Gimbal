# FalconFoc BSP Boundary

## 1. Role

`bsp/` is the live board-owned layer of FalconFoc, not a second platform and
not a reduced `dev/drv` runtime.

Current split:

- `bsp/board`
  - board facts that must exist before task bring-up
  - minimal pre-kernel glue
  - board-owned on-chip flash backend
- `bsp/ports`
  - thin functional ports for control-plane peripherals
  - console / reset / future soft-task peripheral adapters

## 2. What BSP Owns

`bsp/` may own only these responsibilities:

1. Board-local pre-kernel glue.
2. Board-local flash access backend used by FalconFoc persistence.
3. Thin control-plane hardware ports that are not on the FOC hot path.

## 3. What BSP Must Not Own

`bsp/` must not:

- become a second `platform/`
- copy vendor driver stacks back under `bsp/`
- absorb product shell semantics
- absorb application state machines
- wrap the FOC ISR hot path behind generic interfaces

## 4. FalconFoc-Specific Rule

FalconFoc is intentionally not a full Themis `device/` / `drivers/` product.

Therefore:

- FOC fast paths may access vendor code and registers directly from
  hardware-coupled application code.
- soft-task peripherals should stop at `bsp/ports`.
- `bsp/ports` is function-oriented, not peripheral-taxonomy-oriented.

## 5. Current Live Truth

Current live files:

- board init: `bsp/board/bsp_init.c`
- flash backend: `bsp/board/bsp_flash.c`
- console port: `bsp/ports/console_port.c`
- system port: `bsp/ports/system_port.c`
- board facts: `bsp/bsp_config.h`

Current live startup relationship:

`applications/app_main.c -> bsp_init() -> export bring-up`

Current live persistence relationship:

`applications/algo/foc_param.c -> bsp_flash -> tiny_param`

## 6. Evolution Rule

Future BSP changes must prefer:

- adding a thin `*_port` only when a real control-plane peripheral needs it
- keeping vendor headers inside `.c`
- keeping public headers northbound and product-focused

Do not introduce:

- `hal/`
- vtables
- runtime device registries
- speculative abstraction for hardware that has no second consumer yet
