# clang-format Tool Bundle

## Purpose

This directory provides a stable repository-local `clang-format` entry for Themis.

The current machine already has `clang-format.exe` under Visual Studio Build Tools,
but it is not exposed on `PATH`. To avoid repeating path discovery in prompts,
scripts, and IDE setup, the executable is copied here and `scripts/clang-format.cmd`
uses this location first.

## Default executable path

```text
tools/clang-format/clang-format.exe
```

## Wrapper path

```text
scripts/clang-format.cmd
```

## Source on the current machine

Preferred copy source:

```text
C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\Llvm\x64\bin\clang-format.exe
```

Fallback source:

```text
C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\Llvm\bin\clang-format.exe
```

## Usage examples

```powershell
.\scripts\clang-format.cmd -style=file -i drivers\drv_uart.c
.\scripts\clang-format.cmd -style=file -n --Werror drivers\drv_uart.c
```
