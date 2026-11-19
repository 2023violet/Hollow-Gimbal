/**
 * @file    foc_config_store.c
 * @brief   Choose and commit independently erasable configuration slots.
 * @version v0.1.0
 * @date    2026.11.18
 *
 * Implementation role
 * -------------------
 * Keep a previously selected record intact while committing and verifying
 * an inactive slot. Recover from either slot after reopening the device.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_config_store.h"

#include <string.h>

/* Private Types ---------------------------------------------------------------- */

struct save_gate {
    struct foc_runtime *runtime;
    uint32_t session;
};

/* Private Constants ------------------------------------------------------------ */

static const uint8_t commit[FOC_CONFIG_STORE_PROGRAM_BYTES] = {
    (uint8_t)FOC_CONFIG_STORE_COMMIT_WORD,
    (uint8_t)(FOC_CONFIG_STORE_COMMIT_WORD >> 8U),
    (uint8_t)(FOC_CONFIG_STORE_COMMIT_WORD >> 16U),
    (uint8_t)(FOC_CONFIG_STORE_COMMIT_WORD >> 24U),
};

/* Private Helpers -------------------------------------------------------------- */

static struct foc_config_store_report new_report(void)
{
    return (struct foc_config_store_report){
        .active_slot = FOC_CONFIG_STORE_SLOT_NONE,
        .slot_status = {FOC_CONFIG_STORE_ERR_INPUT, FOC_CONFIG_STORE_ERR_INPUT},
    };
}

static int scan(
    const struct foc_config_store_driver *driver,
    struct foc_config_store_workspace *workspace,
    struct foc_config_store_report *report
)
{
    if (driver == NULL || driver->read == NULL || workspace == NULL) {
        return FOC_CONFIG_STORE_ERR_INPUT;
    }

    struct foc_config_store_record records[2];
    for (unsigned int slot = 0U; slot < 2U; ++slot) {
        uint8_t *bytes = workspace->slots[slot];
        report->slot_status[slot] =
            driver->read(driver->context, slot, 0U, bytes, FOC_CONFIG_STORE_SLOT_BYTES) == 0
            ? foc_config_store_inspect(bytes, FOC_CONFIG_STORE_SLOT_BYTES, &records[slot])
            : FOC_CONFIG_STORE_ERR_IO;
    }

    const int a = report->slot_status[0];
    const int b = report->slot_status[1];
    if (a == FOC_CONFIG_STORE_ERR_IO || b == FOC_CONFIG_STORE_ERR_IO) {
        return FOC_CONFIG_STORE_ERR_IO;
    }
    /* A CRC-valid foreign format shares the outer envelope but may redefine sequence. */
    if (a == FOC_CONFIG_STORE_ERR_FORMAT || b == FOC_CONFIG_STORE_ERR_FORMAT) {
        return FOC_CONFIG_STORE_ERR_FORMAT;
    }
    if (a != FOC_CONFIG_STORE_OK && b != FOC_CONFIG_STORE_OK) {
        return a == FOC_CONFIG_STORE_EMPTY && b == FOC_CONFIG_STORE_EMPTY
            ? FOC_CONFIG_STORE_EMPTY
            : FOC_CONFIG_STORE_ERR_NO_VALID;
    }

    unsigned int active = a == FOC_CONFIG_STORE_OK ? 0U : 1U;
    if (a == FOC_CONFIG_STORE_OK && b == FOC_CONFIG_STORE_OK) {
        const uint32_t delta = records[1].sequence - records[0].sequence;
        if (delta == UINT32_C(0x80000000) ||
            (delta == 0U &&
             (records[0].size != records[1].size ||
              memcmp(workspace->slots[0], workspace->slots[1], records[0].size) != 0))) {
            return FOC_CONFIG_STORE_ERR_SEQUENCE;
        }
        if (delta != 0U && delta < UINT32_C(0x80000000)) {
            active = 1U;
        }
    }
    report->active_slot = (enum foc_config_store_slot)active;
    report->record = records[active];
    return FOC_CONFIG_STORE_OK;
}

static int active_allowed(const struct save_gate *gate, struct foc_config_store_report *report)
{
    if (gate == NULL) {
        return FOC_CONFIG_STORE_OK;
    }
    const int status =
        foc_parameter_write_access(gate->runtime, FOC_PARAMETER_LIMITS_ENABLED, gate->session);
    report->parameter_status = status;
    if (status != FOC_PARAMETER_OK) {
        return FOC_CONFIG_STORE_ERR_GATE;
    }
    struct foc_runtime_lifecycle_result result;
    if (foc_runtime_read_lifecycle_result(gate->runtime, &result) != FOC_RUNTIME_OK ||
        result.result == FOC_RUNTIME_REQUEST_PENDING) {
        report->parameter_status = FOC_PARAMETER_ERR_BUSY;
        return FOC_CONFIG_STORE_ERR_GATE;
    }
    return FOC_CONFIG_STORE_OK;
}

static int write_record(
    const struct foc_config_store_driver *driver,
    unsigned int target,
    const uint8_t *encoded,
    size_t size,
    uint8_t *readback,
    const struct save_gate *gate,
    struct foc_config_store_report *report
)
{
    const size_t start = sizeof(commit);
    if (driver->erase(driver->context, target) != 0 ||
        driver->program(driver->context, target, start, encoded + start, size - start) != 0 ||
        driver->read(driver->context, target, 0U, readback, size) != 0) {
        report->slot_status[target] = FOC_CONFIG_STORE_ERR_IO;
        return FOC_CONFIG_STORE_ERR_IO;
    }
    report->slot_status[target] = FOC_CONFIG_STORE_ERR_UNCOMMITTED;
    if (memcmp(readback, encoded, size) != 0) {
        report->slot_status[target] = FOC_CONFIG_STORE_ERR_VERIFY;
        return FOC_CONFIG_STORE_ERR_VERIFY;
    }
    const int status = active_allowed(gate, report);
    if (status != FOC_CONFIG_STORE_OK) {
        return status;
    }

    const int programmed = driver->program(driver->context, target, 0U, commit, start);
    if (driver->read(driver->context, target, 0U, readback, size) != 0) {
        report->slot_status[target] = FOC_CONFIG_STORE_ERR_UNCERTAIN;
        return FOC_CONFIG_STORE_ERR_UNCERTAIN;
    }
    if (memcmp(readback, commit, start) != 0) {
        return programmed == 0 ? FOC_CONFIG_STORE_ERR_VERIFY : FOC_CONFIG_STORE_ERR_IO;
    }
    if (memcmp(readback + start, encoded + start, size - start) != 0 ||
        foc_config_store_inspect(readback, size, &report->record) != FOC_CONFIG_STORE_OK) {
        report->slot_status[target] = FOC_CONFIG_STORE_ERR_UNCERTAIN;
        return FOC_CONFIG_STORE_ERR_UNCERTAIN;
    }

    report->slot_status[target] = FOC_CONFIG_STORE_OK;
    report->active_slot = (enum foc_config_store_slot)target;
    return FOC_CONFIG_STORE_OK;
}

static bool initial_writable(int status)
{
    return status == FOC_CONFIG_STORE_EMPTY || status == FOC_CONFIG_STORE_ERR_UNCOMMITTED;
}

static int save(
    const struct foc_config_store_driver *driver,
    const struct foc_config_store_policy *policy,
    struct foc_config_store_workspace *workspace,
    const struct foc_parameter_candidate *candidate,
    const struct save_gate *gate,
    struct foc_config_store_report *report
)
{
    if (driver == NULL || driver->erase == NULL || driver->program == NULL) {
        return FOC_CONFIG_STORE_ERR_INPUT;
    }
    int status = scan(driver, workspace, report);
    const bool active = status == FOC_CONFIG_STORE_OK;
    if (active) {
        struct foc_parameter_candidate previous;
        status = foc_config_store_decode(
            policy,
            workspace->slots[report->active_slot],
            report->record.size,
            &previous
        );
        if (status != FOC_CONFIG_STORE_OK && status != FOC_CONFIG_STORE_MIGRATED) {
            return status;
        }
    } else if (
        status != FOC_CONFIG_STORE_EMPTY &&
        (status != FOC_CONFIG_STORE_ERR_NO_VALID || !initial_writable(report->slot_status[0]) ||
         !initial_writable(report->slot_status[1]))
    ) {
        return status;
    }

    const unsigned int target = report->active_slot == FOC_CONFIG_STORE_SLOT_A ? 1U : 0U;
    uint8_t *encoded = workspace->slots[target];
    uint8_t *readback = workspace->slots[1U - target];
    const uint32_t sequence = active ? report->record.sequence + 1U : 0U;
    size_t size;
    status = foc_config_store_encode(
        policy,
        candidate,
        sequence,
        encoded,
        FOC_CONFIG_STORE_SLOT_BYTES,
        &size
    );
    if (status != FOC_CONFIG_STORE_OK) {
        return status;
    }
    status = active_allowed(gate, report);
    if (status != FOC_CONFIG_STORE_OK) {
        return status;
    }
    const size_t header = FOC_CONFIG_STORE_HEADER_BYTES;
    if (active && report->record.schema == FOC_CONFIG_STORE_SCHEMA_VERSION &&
        report->record.size == size &&
        memcmp(encoded + header, readback + header, size - header) == 0) {
        /* Repeating a save after lost acknowledgement does not consume a new slot. */
        return FOC_CONFIG_STORE_OK;
    }
    return write_record(driver, target, encoded, size, readback, gate, report);
}

/* Public Functions ------------------------------------------------------------- */

int foc_config_store_select(
    const struct foc_config_store_driver *reader,
    struct foc_config_store_workspace *workspace,
    struct foc_config_store_report *report
)
{
    if (report == NULL) {
        return FOC_CONFIG_STORE_ERR_INPUT;
    }
    *report = new_report();
    return scan(reader, workspace, report);
}

int foc_config_store_load(
    const struct foc_config_store_driver *driver,
    const struct foc_config_store_policy *policy,
    struct foc_config_store_workspace *workspace,
    struct foc_parameter_candidate *candidate,
    struct foc_config_store_report *report
)
{
    struct foc_config_store_report result = new_report();
    int status = FOC_CONFIG_STORE_ERR_INPUT;
    if (policy != NULL && candidate != NULL) {
        status = scan(driver, workspace, &result);
        if (status == FOC_CONFIG_STORE_OK) {
            status = foc_config_store_decode(
                policy,
                workspace->slots[result.active_slot],
                result.record.size,
                candidate
            );
        }
    }
    if (report != NULL) {
        *report = result;
    }
    return status;
}

int foc_config_store_save(
    const struct foc_config_store_driver *driver,
    const struct foc_config_store_policy *policy,
    struct foc_config_store_workspace *workspace,
    const struct foc_parameter_candidate *candidate,
    struct foc_config_store_report *report
)
{
    struct foc_config_store_report result = new_report();
    const int status = save(driver, policy, workspace, candidate, NULL, &result);
    if (report != NULL) {
        *report = result;
    }
    return status;
}

int foc_config_store_save_active(
    const struct foc_config_store_driver *driver,
    const struct foc_pipeline_config *pipeline,
    const struct foc_config_store_identity *identity,
    struct foc_runtime *runtime,
    uint32_t session,
    const struct foc_parameter_edit *edits,
    size_t count,
    struct foc_config_store_workspace *workspace,
    struct foc_config_store_report *report
)
{
    struct foc_config_store_report result = new_report();
    int status = FOC_CONFIG_STORE_ERR_INPUT;
    if (pipeline != NULL && identity != NULL && runtime != NULL) {
        const struct save_gate gate = {runtime, session};
        status = active_allowed(&gate, &result);
        if (status == FOC_CONFIG_STORE_OK) {
            struct foc_parameter_candidate candidate;
            result.parameter_status =
                foc_parameter_stage(runtime, session, edits, count, &candidate);
            status = FOC_CONFIG_STORE_ERR_GATE;
            if (result.parameter_status == FOC_PARAMETER_VALIDATED) {
                const struct foc_config_store_policy policy = {
                    .pipeline = pipeline,
                    .base = &runtime->config,
                    .identity = *identity,
                };
                status = save(driver, &policy, workspace, &candidate, &gate, &result);
            }
        }
    }
    if (report != NULL) {
        *report = result;
    }
    return status;
}

/* ---------------------------------------- end of file ----------------------------------------- */
