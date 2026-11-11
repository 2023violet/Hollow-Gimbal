# FalconFOC V2 G2/G3-NP Source-Style Acceptance

Date: 2026-11-09

## Scope

This review covers the uncommitted G2 and G3-NP source, test, CMake, README, and gate changes in the local working tree. It applies the following repository rules.

- `docs/governance/Themis_代码规范指南.md`
- `docs/governance/Themis_Utils与通用组件代码规范指南.md`
- `docs/governance/Themis_C与H文件布局与注释书写规范.md`
- `docs/governance/Themis_CMake编写规范与质量门禁.md`
- `docs/governance/Themis_AI高质量编码必读清单.md`
- repository `.clang-format`

The review is based on commit `1f542521189d5e0ef25e64e6530608ece8d6e137` plus the local working-tree additions. It does not claim that the older GitHub `main` snapshot contains these files.

## Findings before remediation

The first source-style audit confirmed the following concrete defects.

- G2 and G3-NP C/H files had no Themis file headers, public-header usage sections, or EOF markers.
- Public headers exposed types and functions without item-level ownership, unit, lifecycle, context, blocking, or failure comments.
- Test sources had no formal file roles or section layout.
- `core/CMakeLists.txt` and `g3/np/CMakeLists.txt` had no Themis header comments and used underscore target names.
- `scripts/format_themis.ps1` did not select `core/` or `g3/np/` C/H files.
- `scripts/gates/check_cmake_quality.ps1` did not list either new CMake file.
- `check_themis_quality_gate.ps1` had no FalconFOC source-contract scope.
- `AGENTS.md` did not place the three C/H and CMake standards in the mandatory read order.

These were confirmed compliance defects. They were not inferred from aesthetic preference.

## Remediation

- Added consumer-facing module headers with design overview, usage sequence, ownership/lifecycle/context, blocking, and failure/recovery sections.
- Added item-level comments for public enums, enum values, structures, contract-bearing fields, aliases, and functions.
- Added implementation or test-role headers, section banners, rationale comments at mapping and transactional boundaries, and Themis EOF markers.
- Renamed new CMake targets to kebab-case and added Themis CMake role headers.
- Added the new directories to the changed/untracked C/H formatter scope.
- Added both standalone CMake files to the CMake quality file list and treated them as intentional standalone project entries.
- Added `check_falconfoc_source_contract.ps1` for C/H header, section, EOF, whitespace, CMake target, and README build-contract checks.
- Added `falconfoc` to the unified quality-gate scope and wired the source-contract child check.
- Added all five relevant governance documents to the `AGENTS.md` mandatory read order.

No control algorithm, public Core behavior, Device mapping behavior, Legacy source, root firmware CMake, or board action was changed for style remediation.

## Verification

The dedicated source contract passes.

```text
11 C/H files checked
2 CMake files checked
2 README files checked
all requested checks passed
```

The repository clang-format launcher passes all 11 C/H files in dry-run warning-as-error mode. The changed/untracked C/H formatter flow also passes and now lists both new directories.

The CMake quality gate passes its structural checks and now lists both standalone CMake files. `cmake-format` is not installed in the current environment, so its existing local warning remains. Strict CMake formatter enforcement still requires installing `cmakelang` or providing `cmake-format` on PATH.

G2 and G3-NP functional verification remains intact.

- G2 Core CTest `3/3 PASS`
- G3-NP CTest `2/2 PASS`
- Legacy Host `45/45 PASS`
- AT32F415 bench configure/build `PASS`
- `git diff --check` `PASS`

The dedicated FalconFOC quality scope passes. It runs CMake structural quality, preset, platform backend, CMake smoke, cache guard, transport-shell hardening, and the new source contract. It intentionally skips whole-repository CMake formatter enforcement and the unrelated firmware CLI contract because those checks cover pre-existing root and board lanes outside this local G2/G3-NP scope.

The two new CMake files pass strict `cmakelang` checks directly. The repository-wide `all` quality scope now executes the FalconFOC source contract before it stops at the pre-existing root `CMakeLists.txt` formatter mismatch. Once that repository-wide format debt is handled, its firmware CLI contract still requires Bash on this Windows environment.

## Decision

G2/G3-NP source-style compliance is **PASS in the local working tree**, with the following explicit environment limitation.

- Whole-repository CMake formatter enforcement remains blocked by the pre-existing root CMake formatting mismatch.
- The repository-wide firmware CLI contract is not executable until Bash is available.

The source contract, C/H formatter, dedicated FalconFOC quality scope, CMake structural checks, functional tests, and Legacy regressions provide the currently available evidence. This decision does not authorize G3-SAFE, production BSP integration, ISR changes, or powered-board testing.

The later G3-SAFE host slice is also included in the same source contract. The current total is 14 C/H files, 3 CMake files, and 3 README files. Its safety semantics are recorded separately in `docs/evidence/2026-11-09-g3-safe-validation.md`.
