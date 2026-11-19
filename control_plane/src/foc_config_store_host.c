/**
 * @file    foc_config_store_host.c
 * @brief   Deterministic, caller-owned Host model for bounded storage writes.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Retain programmed bytes across a model reboot and inject before, during
 * or after each erase/program unit or read operation.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_config_store_host.h"

#include <string.h>

/* Private Helpers -------------------------------------------------------------- */

static bool access_valid(
    const struct foc_config_store_host *host,
    unsigned int slot,
    size_t offset,
    size_t size
)
{
    return host != NULL && !host->offline && slot < 2U && offset <= FOC_CONFIG_STORE_SLOT_BYTES &&
        size != 0U && size <= FOC_CONFIG_STORE_SLOT_BYTES - offset;
}

static size_t transfer(struct foc_config_store_host *host, size_t bytes)
{
    ++host->steps;
    if (host->cut_step != 0U && host->steps == host->cut_step) {
        host->offline = true;
        if (host->cut == FOC_CONFIG_STORE_HOST_BEFORE) {
            return 0U;
        }
        if (host->cut == FOC_CONFIG_STORE_HOST_TORN) {
            return bytes / 2U;
        }
    }
    return bytes;
}

static int host_read(void *context, unsigned int slot, size_t offset, uint8_t *bytes, size_t size)
{
    struct foc_config_store_host *host = context;
    if (bytes == NULL || !access_valid(host, slot, offset, size)) {
        return -1;
    }
    const size_t transferred = transfer(host, size);
    memcpy(bytes, host->slots[slot] + offset, transferred);
    return host->offline ? -1 : 0;
}

static int host_erase(void *context, unsigned int slot)
{
    struct foc_config_store_host *host = context;
    if (!access_valid(host, slot, 0U, FOC_CONFIG_STORE_SLOT_BYTES)) {
        return -1;
    }
    const size_t unit = FOC_CONFIG_STORE_PROGRAM_BYTES;
    for (size_t offset = 0U; offset < FOC_CONFIG_STORE_SLOT_BYTES; offset += unit) {
        const size_t transferred = transfer(host, unit);
        memset(host->slots[slot] + offset, 0xFF, transferred);
        if (host->offline) {
            return -1;
        }
    }
    return 0;
}

static int
host_program(void *context, unsigned int slot, size_t offset, const uint8_t *bytes, size_t size)
{
    struct foc_config_store_host *host = context;
    const size_t unit = FOC_CONFIG_STORE_PROGRAM_BYTES;
    if (bytes == NULL || !access_valid(host, slot, offset, size) || (offset % unit) != 0U ||
        (size % unit) != 0U) {
        return -1;
    }
    uint8_t *dst = host->slots[slot] + offset;
    for (size_t index = 0U; index < size; ++index) {
        if ((dst[index] & bytes[index]) != bytes[index]) {
            return -1;
        }
    }
    for (size_t index = 0U; index < size; index += unit) {
        const size_t transferred = transfer(host, unit);
        memcpy(dst + index, bytes + index, transferred);
        if (host->offline) {
            return -1;
        }
    }
    return 0;
}

/* Public Functions ------------------------------------------------------------- */

void foc_config_store_host_init(struct foc_config_store_host *host)
{
    memset(host->slots, 0xFF, sizeof(host->slots));
    foc_config_store_host_reboot(host);
}

void foc_config_store_host_reboot(struct foc_config_store_host *host)
{
    host->offline = false;
    foc_config_store_host_inject(host, 0U, FOC_CONFIG_STORE_HOST_BEFORE);
}

void foc_config_store_host_inject(
    struct foc_config_store_host *host,
    uint32_t step,
    enum foc_config_store_host_cut cut
)
{
    host->steps = 0U;
    host->cut_step = step;
    host->cut = cut;
}

struct foc_config_store_driver foc_config_store_host_driver(struct foc_config_store_host *host)
{
    return (struct foc_config_store_driver){host, host_read, host_erase, host_program};
}

/* ---------------------------------------- end of file ----------------------------------------- */
