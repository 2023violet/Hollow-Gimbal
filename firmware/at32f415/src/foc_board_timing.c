/**
 * @file    foc_board_timing.c
 * @brief   Pure C11 ADC/PWM timing computations for Board1.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Implementation role
 * -------------------
 * Validate future timing against the existing Board1 ADC transaction.
 */

/* Includes --------------------------------------------------------------------- */
#include "foc_board_timing.h"

/* Private Constants ------------------------------------------------------------ */
enum {
    ADC_SAMPLE_NS = 600U,
    ADC_RANK_NS = 1600U,
    ADC_GROUP_NS = FOC_BOARD_RANK_COUNT * ADC_RANK_NS,
    ADC_PCTEN = 1U << 15,
    ADC_SELECT_MASK = (1U << 24) | (7U << 12),
    ADC1_ETP_GMUX_MASK = 1U << 4
};

/* Private Helpers -------------------------------------------------------------- */
static bool foc_board_stamp_ns(
    struct foc_board_carrier_stamp stamp,
    uint32_t sample_cycle,
    uint32_t period_ns,
    uint64_t *ns
)
{
    const uint32_t cycles = stamp.cycle - sample_cycle;
    if (stamp.phase_ns >= period_ns || cycles >= UINT32_C(0x80000000)) {
        return false;
    }
    *ns = (uint64_t)cycles * period_ns + stamp.phase_ns;
    return true;
}

static bool foc_board_id_newer(uint32_t id, uint32_t prior)
{
    const uint32_t delta = id - prior;
    return delta != 0U && delta < UINT32_C(0x80000000);
}

static bool
foc_board_stamp_newer(struct foc_board_carrier_stamp stamp, struct foc_board_carrier_stamp prior)
{
    return stamp.cycle == prior.cycle ? stamp.phase_ns > prior.phase_ns
                                      : foc_board_id_newer(stamp.cycle, prior.cycle);
}

/* Public Functions ------------------------------------------------------------- */
bool foc_board_spi_on_time(
    enum foc_board_spi_edge edge,
    uint32_t phase_us,
    uint32_t before_phase_us,
    uint32_t observed_us
)
{
    static const uint32_t earliest[] = {0U, FOC_BOARD_SPI_SEND_US, FOC_BOARD_SPI_CLOSE_US};
    static const uint32_t latest[] = {10U, 30U, 100U};
    const unsigned int index = (unsigned int)edge;
    return index < sizeof earliest / sizeof earliest[0] && phase_us >= earliest[index] &&
        phase_us <= latest[index] && observed_us - before_phase_us <= latest[index] - phase_us;
}

enum foc_board_encoder_result foc_board_spi_close_result(
    uint32_t phase_us,
    uint32_t before_phase_us,
    uint32_t checked_us,
    uint32_t rx_us,
    bool received,
    bool busy,
    bool error
)
{
    const uint32_t hold = checked_us - rx_us;
    if (error) {
        return FOC_BOARD_ENCODER_FRAME;
    }
    return received && !busy && hold >= 2U && hold < UINT32_C(0x80000000) &&
            foc_board_spi_on_time(FOC_BOARD_SPI_CLOSE, phase_us, before_phase_us, checked_us)
        ? FOC_BOARD_ENCODER_OK
        : FOC_BOARD_ENCODER_TIMEOUT;
}

bool foc_board_adc_route_config(
    enum foc_board_adc_route route,
    struct foc_board_adc_route_config *config
)
{
    if (route != FOC_BOARD_ADC_SOFTWARE && route != FOC_BOARD_ADC_TMR1_CH4 &&
        route != FOC_BOARD_ADC_REMAPPED_CH4) {
        return false;
    }
    const uint32_t gmux = route == FOC_BOARD_ADC_REMAPPED_CH4 ? ADC1_ETP_GMUX_MASK : 0U;
    *config = (struct foc_board_adc_route_config){
        .ctrl2_mask = ADC_SELECT_MASK | ADC_PCTEN,
        .ctrl2 = ((uint32_t)route << 12) | ADC_PCTEN,
        .iomux_remap7_mask = gmux,
        .iomux_remap7 = gmux,
    };
    return true;
}

bool foc_board_adc_candidate_begin(
    struct foc_runtime *runtime,
    struct foc_board_adc *adc,
    const struct foc_board_adc_event *compare,
    bool unique_compare_witness,
    bool pcce_pending
)
{
    struct foc_board_adc_event event = *compare;
    event.coherent = event.coherent && unique_compare_witness &&
        event.phase_ticks == FOC_BOARD_ADC_COMPARE_US * (FOC_BOARD_TIMER_HZ / FOC_BOARD_US_HZ);
    return foc_board_adc_begin(runtime, adc, &event, pcce_pending);
}

bool foc_board_adc_scan_at(uint32_t effective_soc_ns, struct foc_board_adc_scan *scan)
{
    if (effective_soc_ns > UINT32_MAX - ADC_GROUP_NS) {
        return false;
    }
    for (unsigned int rank = 0U; rank < FOC_BOARD_RANK_COUNT; ++rank) {
        const uint32_t start = effective_soc_ns + rank * ADC_RANK_NS;
        scan->rank[rank] =
            (struct foc_board_aperture){.start_ns = start, .end_ns = start + ADC_SAMPLE_NS};
    }
    scan->complete_ns = effective_soc_ns + ADC_GROUP_NS;
    return true;
}

bool foc_board_low_side_window(
    uint32_t period_ns,
    uint32_t compare_ns,
    uint32_t after_edge_ns,
    uint32_t before_edge_ns,
    struct foc_board_aperture *window
)
{
    if (period_ns == 0U || compare_ns > period_ns / 2U) {
        return false;
    }
    const uint32_t width = period_ns - 2U * compare_ns;
    if (after_edge_ns >= width || before_edge_ns >= width - after_edge_ns) {
        return false;
    }
    *window = (struct foc_board_aperture){
        .start_ns = compare_ns + after_edge_ns,
        .end_ns = period_ns - compare_ns - before_edge_ns,
    };
    return true;
}

bool foc_board_adc_current_window(
    const struct foc_board_adc_scan *scan,
    const struct foc_board_aperture windows[3]
)
{
    for (unsigned int rank = FOC_BOARD_RANK_IC; rank < FOC_BOARD_RANK_COUNT; ++rank) {
        const struct foc_board_aperture *sample = &scan->rank[rank];
        const struct foc_board_aperture *window = &windows[rank - FOC_BOARD_RANK_IC];
        if (sample->start_ns >= sample->end_ns || sample->start_ns < window->start_ns ||
            sample->end_ns > window->end_ns) {
            return false;
        }
    }
    return true;
}

enum foc_board_shadow_result foc_board_shadow_check_timing(
    const struct foc_board_load_plan *plan,
    const struct foc_board_shadow_attempt *attempt
)
{
    if (!attempt->window_valid || attempt->eoc.cycle != attempt->sample_cycle) {
        return FOC_BOARD_SHADOW_INVALID;
    }
    uint64_t eoc;
    uint64_t start;
    uint64_t finish;
    uint64_t write;
    uint64_t load;
    if (!foc_board_stamp_ns(attempt->eoc, attempt->sample_cycle, plan->period_ns, &eoc) ||
        !foc_board_stamp_ns(attempt->h0_start, attempt->sample_cycle, plan->period_ns, &start) ||
        !foc_board_stamp_ns(attempt->h0_finish, attempt->sample_cycle, plan->period_ns, &finish) ||
        !foc_board_stamp_ns(
            attempt->shadow_write,
            attempt->sample_cycle,
            plan->period_ns,
            &write
        ) ||
        !foc_board_stamp_ns(plan->load, attempt->sample_cycle, plan->period_ns, &load) ||
        eoc > start || start > finish || finish > write || plan->setup_ns >= plan->period_ns) {
        return FOC_BOARD_SHADOW_INVALID;
    }
    if (load <= plan->setup_ns || write >= load - plan->setup_ns) {
        return FOC_BOARD_SHADOW_DEADLINE;
    }
    return FOC_BOARD_SHADOW_STAGED;
}

enum foc_board_shadow_result foc_board_shadow_stage(
    struct foc_board_shadow *shadow,
    const struct foc_board_load_plan *plan,
    const struct foc_board_adc_transaction *token,
    const struct foc_board_shadow_attempt *attempt
)
{
    const bool occupied = shadow->pending;
    shadow->pending = false;
    if (token->slot_id != attempt->sample_cycle || token->generation != attempt->generation ||
        (shadow->sample_seen &&
         (!foc_board_id_newer(token->slot_id, shadow->last_sample_cycle) ||
          !foc_board_id_newer(token->generation, shadow->last_generation)))) {
        return FOC_BOARD_SHADOW_INVALID;
    }
    shadow->sample_seen = true;
    shadow->last_sample_cycle = token->slot_id;
    shadow->last_generation = token->generation;
    if (occupied || token->state != FOC_BOARD_ADC_COMPLETED || !token->adc_valid ||
        !token->h0_called || !token->h0_consumed || token->h0_result != FOC_RUNTIME_OK ||
        !token->update_accepted || token->update_declined || token->physical_output_applied ||
        (shadow->applied_seen && !foc_board_stamp_newer(plan->load, shadow->applied))) {
        return FOC_BOARD_SHADOW_INVALID;
    }

    const enum foc_board_shadow_result result = foc_board_shadow_check_timing(plan, attempt);
    if (result != FOC_BOARD_SHADOW_STAGED) {
        return result;
    }
    shadow->load = plan->load;
    shadow->pending = true;
    return FOC_BOARD_SHADOW_STAGED;
}

enum foc_board_shadow_result foc_board_shadow_load(
    struct foc_board_shadow *shadow,
    struct foc_board_carrier_stamp observed,
    bool witness_valid,
    bool output_allowed
)
{
    const bool match = shadow->pending && witness_valid && output_allowed &&
        observed.cycle == shadow->load.cycle && observed.phase_ns == shadow->load.phase_ns;
    shadow->pending = false;
    if (match) {
        shadow->applied_seen = true;
        shadow->applied = observed;
    }
    return match ? FOC_BOARD_SHADOW_MODEL_APPLIED : FOC_BOARD_SHADOW_INVALID;
}

/* ---------------------------------------- end of file ----------------------------------------- */
