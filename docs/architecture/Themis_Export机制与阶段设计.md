# FalconFoc Export And Boot Staging

## 1. Purpose

`tm_export` is the live staged bring-up mechanism used by FalconFoc after raw
platform initialization and before or during task bring-up.

It exists to keep startup order explicit and reviewable.

## 2. Live Startup Context

Current live chain:

`platform generated main -> applications/app_main -> bsp_init -> export_init_execute_range(...) -> task bring-up`

FalconFoc uses export staging only for the layers that still need ordered
bring-up around RTOS initialization. It is not a license to hide product
ownership.

## 3. Current Stage Split

The current live split is:

- pre-kernel export range
- `osKernelInitialize()`
- post-kernel export range through `EXPORT_LEVEL_APP`

This means:

- raw MCU bring-up belongs to `platform/at/at32f403a/generated/src/main.c`
- board-local pre-kernel glue belongs to `bsp_init()`
- ordered framework and APP bring-up belongs to export staging

## 4. FalconFoc Owner Rule

In this repository:

- `app_main.c` owns the staged bridge
- `motor_task_init()` is the current APP bring-up owner
- `shell_task_init()` is called explicitly from `motor_task_init()`
- `vofa_task_init()` remains an independent APP export owner

Do not move these responsibilities back into:

- old BSP-init chains
- retired task-owner layouts
- hidden shell command registration exports

## 5. Why Export Still Exists Here

FalconFoc does not use full `dev/drv`, but ordered bring-up is still useful
for:

- shared framework setup
- APP export ordering
- isolating pre-kernel and post-kernel work
- capturing boot-stage failures in one place

## 6. Usage Rule

Use export staging when:

- the initialization must be ordered against RTOS startup
- the owner is still a live framework or APP bring-up unit

Do not use export staging when:

- normal direct function calls are clearer
- the logic is ordinary product behavior rather than startup behavior
- the goal is only to avoid writing explicit ownership in startup code
