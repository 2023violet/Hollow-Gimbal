# AT32F415 FalconFoc Code Wiki Design

## Goal

Create a source-grounded, Chinese-language Code Wiki for the current
`AT32F415` working tree. The Wiki must explain the product architecture,
module ownership, important types and functions, build/runtime dependencies,
operating procedures, software-only simulation scenarios, and evidence-backed
risks or inconsistencies.

## Scope and source authority

- The current working tree is the source of truth for implementation facts.
- The selected platform is `platform/at/at32f415` with `cortex-m3-soft`.
- The Wiki analyzes FalconFoc-owned code in detail: `applications/`, `bsp/`,
  `components/`, `subsys/`, `cmake/`, `scripts/`, `tests/`, and the selected
  generated platform layer.
- The large third-party AT32 vendor and CMSIS trees are documented by role,
  selected files, and call boundaries. They are not transcribed function by
  function.
- Existing `AT32F403A` documents, stale contracts, generated build output, and
  uncommitted changes are labeled as baseline, drift, artifact, or working-tree
  evidence instead of being silently merged into current facts.
- No board or serial-port claim is made by the software-only validation lane.

## Deliverables

1. `docs/code-wiki/README.md`: canonical entry, scope, current facts, reading
   map, and evidence legend.
2. `docs/code-wiki/architecture.md`: layer ownership, boot sequence, timing,
   ISR/task boundaries, state machine, and data-flow narratives.
3. `docs/code-wiki/modules.md`: module responsibility tables and key public
   types/functions for product, BSP, components, and subsystems.
4. `docs/code-wiki/dependencies.md`: CMake target graph, include/dependency
   relationships, platform metadata, feature profiles, and persistence path.
5. `docs/code-wiki/operations-and-validation.md`: configure/build/test commands,
   software simulation scenarios, expected observations, fresh validation
   output, and limitations.
6. `docs/code-wiki/deep-analysis.md`: architectural strengths, concrete risks,
   current contradictions, likely failure paths, and prioritized follow-up
   recommendations.

## Evidence model

Every material claim is classified as one of:

- `LIVE-SOURCE`: observed in current source or build configuration.
- `LIVE-TEST`: observed in a fresh host/build/script run in this task.
- `CITED-BASELINE`: present in existing repository documentation or logs but
  not rerun here.
- `UNVERIFIED`: plausible or inferred, but requiring board, timing, or external
  instrumentation.

The validation report must distinguish failures caused by stale repository
contracts or missing host prerequisites from product-code failures.

## Architecture and data-flow coverage

The Wiki will cover these concrete paths:

- generated `main()` and AT32F415 peripheral bring-up to `app_main_bootstrap()`;
- staged pre-kernel/post-kernel export and RTOS task creation;
- ADC injected conversion → phase sampling → 10 kHz FOC step;
- TMR5 overflow → encoder update → outer loop;
- motor task 1 ms orchestration → FSM/fault/input/calibration decisions;
- runtime seams for motion, calibration, fault, restore, and deferred storage;
- shell USART1/DMA transport → bounded line queue → command dispatch;
- shell commands → runtime read/request surfaces;
- A/B parameter slots → `bsp_flash` → `tiny_param`;
- optional VOFA channel binding and transport callback;
- CMake metadata → CPU class → FreeRTOS port → platform target → firmware.

## Validation policy

Run software-only evidence proportional to the repository state:

- inspect and record Git status before and after edits;
- validate Markdown links and source references used by the Wiki;
- run the canonical host configure/build/CTest lane when its current paths are
  usable;
- run focused contract tests or direct CMake targets when the aggregate lane
  is blocked;
- run firmware configure/build for the selected `AT32F415` preset when the
  ARM toolchain is available;
- run static contract/gate scripts that do not require a board;
- run shell CLI mock tests if their dependencies are available;
- simulate state transitions, save/restore decisions, queue saturation, and
  control-loop cadence as source-level scenario checks;
- record board-only validation as not run.

## Non-goals

- No source-code behavior changes are part of the Wiki task.
- No cleanup, reset, staging, commit, or deletion of the user's existing
  working-tree changes.
- No function-by-function documentation of third-party vendor/CMSIS code.
- No claim that host tests prove FOC electrical behavior, real-time deadlines,
  ADC sampling correctness, encoder integrity, or motor stability on hardware.
