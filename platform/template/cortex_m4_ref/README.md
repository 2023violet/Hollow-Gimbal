# Themis template platform skeleton

`platform/template/cortex_m4_ref/` is the formal skeleton for a future second
platform.

It is not a production-ready port. It exists only to freeze the required
directory contract for a future migration.

Current template target:

- `template-cortex-m4-ref-hal`

Retained compatibility alias:

- `template_cortex_m4_ref_hal`

Current template boundaries:

- `generated/`
  - generated board/bootstrap code only
- `vendor/`
  - vendor headers, startup files, and linker scripts only
- `CMakeLists.txt`
  - platform target declaration only

When a second platform is introduced, replace this directory with a build-ready
implementation. Do not push second-platform facts back into the current live
`platform/at/at32f403a/`.
