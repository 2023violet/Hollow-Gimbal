/**
 * @file    test_foc_parameter_table.c
 * @brief   Host checks of the generated Board1 parameter table.
 * @version v0.1.0
 * @date    2026.11.24
 *
 * Test role
 * ---------
 * The table is ascending and unique, lookup finds every entry and nothing else,
 * every constant bound equals the value documented in parameters.json, field
 * offsets lie inside their structs and the allowed-value set works.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board_store.h"
#include "foc_parameter_documented.h"
#include "foc_parameter_table.h"
#include "foc_serial.h"

#include <stdio.h>

/* Private Helpers -------------------------------------------------------------- */

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "check failed: %s at %s:%d\n", #condition, __FILE__, __LINE__); \
            return 1;                                                                       \
        }                                                                                   \
    } while (0)

static int
bounds_match(const struct foc_parameter_entry *entry, const struct foc_parameter_documented *row)
{
    if (entry->min_bound == FOC_PARAM_BOUND_CONST && row->has_minimum) {
        CHECK(entry->minimum.u == row->minimum);
    }
    if (entry->max_bound == FOC_PARAM_BOUND_CONST && row->has_maximum) {
        CHECK(entry->maximum.u == row->maximum);
    }
    if (entry->access != FOC_PARAM_ACCESS_RO && entry->min_bound == FOC_PARAM_BOUND_CONST &&
        entry->max_bound == FOC_PARAM_BOUND_CONST) {
        if (entry->type == FOC_PARAM_TYPE_U32) {
            CHECK(entry->minimum.u <= entry->maximum.u);
        } else {
            CHECK(entry->minimum.f <= entry->maximum.f);
        }
    }
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    const size_t documented = sizeof foc_parameter_documented / sizeof foc_parameter_documented[0];
    CHECK(documented == FOC_PARAMETER_COUNT);

    unsigned int persistent = 0U;
    for (size_t index = 0U; index < FOC_PARAMETER_COUNT; ++index) {
        const struct foc_parameter_entry *entry = &foc_parameter_table[index];
        if (index > 0U) {
            CHECK(foc_parameter_table[index - 1U].id < entry->id);
        }
        CHECK(foc_parameter_find(entry->id) == entry);
        CHECK(foc_parameter_documented[index].id == entry->id);
        CHECK(bounds_match(entry, &foc_parameter_documented[index]) == 0);
        CHECK(entry->type == FOC_PARAM_TYPE_U32 || entry->type == FOC_PARAM_TYPE_F32);
        if (entry->group == FOC_PARAM_GROUP_SERIAL) {
            CHECK(entry->offset + 4U <= sizeof(struct foc_serial));
        }
        if (entry->group == FOC_PARAM_GROUP_TUNING) {
            CHECK(entry->offset + 4U <= sizeof(struct foc_runtime_tuning));
            CHECK(entry->access == FOC_PARAM_ACCESS_TUNING);
        }
        if ((entry->flags & FOC_PARAM_FLAG_PERSIST) != 0U) {
            CHECK(entry->persist_offset + 4U <= sizeof(struct foc_board_settings));
            CHECK(entry->access == FOC_PARAM_ACCESS_RW || entry->access == FOC_PARAM_ACCESS_TUNING);
            persistent++;
        } else {
            CHECK(entry->persist_offset == FOC_PARAM_NO_PERSIST);
        }
    }
    CHECK(persistent > 0U);

    const uint16_t absent[] = {0U, 4U, 15U, 63U, 100U, 105U, 149U, 0xFFFFU};
    for (size_t index = 0U; index < sizeof absent / sizeof absent[0]; ++index) {
        CHECK(foc_parameter_find(absent[index]) == NULL);
    }

    const struct foc_parameter_entry *bitrate = foc_parameter_find(FOC_PARAM_CAN_BITRATE);
    CHECK(bitrate != NULL && foc_parameter_allowed(bitrate, 250000U));
    CHECK(!foc_parameter_allowed(bitrate, 300000U));
    CHECK(foc_parameter_allowed(foc_parameter_find(FOC_PARAM_RX_BUDGET), 9U));

    printf(
        "test-foc-parameter-table: %u entries, %u persistent\n",
        (unsigned int)FOC_PARAMETER_COUNT,
        persistent
    );
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
