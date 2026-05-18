/**
 * @file    tm_shell.h
 * @brief   Host forwarder for the canonical Themis shell contract
 * @version v1.1.0
 * @date    2026.05.14
 *
 * Host-test include paths currently search `tests/host/support/` before the
 * live subsystem headers. Forward to the real shell contract so shared tests
 * and application-owned shell code observe the same API surface.
 */

#pragma once

#include "../../../subsys/shell/tm_shell.h"

/* ---------------------------------------- end of file ----------------------------------------- */
