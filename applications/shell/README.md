# FalconFoc Shell

`applications/shell/` is the product-owned shell command surface for FalconFoc.

Current owner split:
- `subsys/shell/`: shell engine and shared output contract
- `applications/tasks/app_shell_task.*`: shell session owner and single-queue policy
- `applications/tasks/shell_task.*`: UART DMA transport glue
- `applications/shell/`: FalconFoc vocabulary, banner, and command behavior

Current command surface:
- `sys`
- `state`
- `obs`
- `fault`
- `store`
- `cal`
- `cfg`
- `diag`

Build policy:
- `TM_SHELL_ENABLE=1`: compile shell transport, session, command table, and banner
- `TM_SHELL_ENABLE=0`: compile shell owners out of the final image

When shell is enabled, FalconFoc exports one complete command surface:
- `sys`
- `state`
- `obs`
- `fault`
- `store`
- `cal`
- `cfg`
- `diag`

Output contract:
- summary line first: `[topic] key=value ...`
- short detail rows after that: `label : key=value ...`
- no wide ASCII tables
- no ASCII art banner
- no `%f`-driven shell formatting path
