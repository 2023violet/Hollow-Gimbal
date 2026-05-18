# FalconFoc Shell

`applications/shell/` is the product-owned shell command surface for FalconFoc.

Current owner split:
- `subsys/shell/`: shell engine and shared output contract
- `applications/tasks/app_shell_task.*`: shell session owner and backlog policy
- `applications/tasks/shell_task.*`: UART DMA transport glue
- `applications/shell/`: FalconFoc vocabulary, profile gating, banner, and command behavior

Current command surface:
- `sys`
- `state`
- `obs`
- `fault`
- `store`
- `cal`
- `cfg`
- `diag`

Profile policy:
- `product`: `sys state obs fault store cfg`
- `factory`: `product + cal`, and `store/cfg` unlock controlled write paths
- `diag`: `factory + diag`

Output contract:
- summary line first: `[topic] key=value ...`
- short detail rows after that: `label : key=value ...`
- no wide ASCII tables
- no ASCII art banner
- no `%f`-driven shell formatting path
