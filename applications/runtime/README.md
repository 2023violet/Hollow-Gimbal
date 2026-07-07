# FalconFoc Runtime

`applications/runtime/` is the FalconFoc product-runtime thin layer.

Owner contract:
- runtime owns product meaning
- tasks own cadence
- shell owns vocabulary
- periph owns hardware-coupled engines
- algo owns pure math, observers, and planning kernels

Current runtime scope:
- default product configuration and startup restore glue
- deferred parameter save handling
- motion-command runtime policy
- software fault policy
- calibration-complete / calibration-abort runtime behavior
- shell-facing snapshot/read/request seam for product commands and diagnostics

Current files:
- `motor_runtime.*`
- `motor_store.*`
- `foc_restore.*`
- `foc_motion_runtime.*`
- `motor_fault_runtime.*`
- `motor_calibration_runtime.*`

Boundary rules:
- do not move FOC ISR hot path into runtime
- do not rebuild a generic manager / bus / registry here
- do not let shell own persistent product meaning
- do not let tasks re-accumulate product policy that already lives here
- shell may read runtime snapshots and issue runtime requests only
- fault runtime may latch `fault_code` and kill PWM, but `axis_state` stays FSM-owned
