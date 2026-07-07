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

Current product budgets when shell is enabled:
- `TM_SHELL_TASK_STACK_BYTES = 192U * 4U` (`768 B`)
- `TM_APP_TASK_STACK_SHELL_BYTES = 384U * 4U` (`1536 B`)
- `SHELL_TX_RING_SIZE = 768U`
- `SHELL_RX_BUF_SIZE = 256U`
- `TM_SHELL_TASK_QUEUE_DEPTH = 3U`

Current resident strategy:
- keep the dual-owner split:
  - `shell_task` owns transport
  - `app_shell_task` owns session
- keep the full command surface resident in shell-on images
- shrink RAM only through FalconFoc product-owned budgets

Output contract:
- summary line first: `[topic] key=value ...`
- short detail rows after that: `label : key=value ...`
- no wide ASCII tables
- no ASCII art banner
- no `%f`-driven shell formatting path
