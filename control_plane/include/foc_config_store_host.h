/**
 * @file    foc_config_store_host.h
 * @brief   Host-only two-slot device model with programmable power cuts.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Design overview
 * ---------------
 * Model independent erase slots, 1-to-0 programming, and four-byte program
 * units. Reads and each erase/program unit count as one observable step.
 *
 * Usage example
 * -------------
 * Initialize erased storage, obtain its driver, and call the store. Arm a
 * fault at a selected step, then reboot the same storage to retain its bytes.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Caller owns and serializes each Host device, including its fault counters.
 *
 * Failure / recovery
 * ------------------
 * Any injected cut leaves the device offline until reboot. A torn step may
 * change the first half of one erase/program unit. A completed step can
 * also lose its acknowledgement.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types ---------------------------------------------------------------- */

enum foc_config_store_host_cut {
    FOC_CONFIG_STORE_HOST_BEFORE = 0,
    FOC_CONFIG_STORE_HOST_TORN,
    FOC_CONFIG_STORE_HOST_AFTER,
};

struct foc_config_store_host {
    uint8_t slots[2][FOC_CONFIG_STORE_SLOT_BYTES];
    uint32_t steps;
    uint32_t cut_step;
    enum foc_config_store_host_cut cut;
    bool offline;
};

/* Public Functions ------------------------------------------------------------- */

/** Clear both slots and all volatile counters when constructing a new Host device. */
void foc_config_store_host_init(struct foc_config_store_host *host);

/** Restore volatile device state while retaining all simulated NVM bytes. */
void foc_config_store_host_reboot(struct foc_config_store_host *host);

/** Reset the step counter and arm a 1-based cut. Zero disables injection. */
void foc_config_store_host_inject(struct foc_config_store_host *host,
                                  uint32_t step,
                                  enum foc_config_store_host_cut cut);

/** Build the copyable driver for this caller-owned Host device. */
struct foc_config_store_driver foc_config_store_host_driver(struct foc_config_store_host *host);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
