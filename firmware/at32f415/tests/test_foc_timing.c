/**
 * @file    test_foc_timing.c
 * @brief   ARCH-00 Host event counterexamples and Board1 interleavings.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Test role
 * ---------
 * The virtual clock and PWM load marker are explicit test inputs. Board Port
 * calls use the production Runtime and the locked output adapter.
 */

/* Includes --------------------------------------------------------------------- */
#include "foc_board.h"
#include "foc_board_timing.h"
#include "foc_adc_test_clock.h"
#include "foc_serial.h"

#include <stdio.h>

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                       \
        }                                                                   \
    } while (0)

/* Private Types and Constants -------------------------------------------------- */
static const uint16_t ranks[] = {1241U, 2048U, 2048U, 2048U};

enum {
    WINDOW_US = 3000,
    LEGACY_EDGE_US = 25,
    PWM_20K_US = 50,
    LOAD_SETUP_US = 2
};

struct cadence {
    unsigned int trigger;
    unsigned int valid;
    unsigned int h0;
    unsigned int accepted;
    unsigned int loaded;
    unsigned int missed;
    unsigned int late_adc;
    unsigned int late_h0;
    unsigned int duplicate;
};

/* Private Helpers -------------------------------------------------------------- */
static void print_cadence(
    const char *name,
    unsigned int pwm_us,
    unsigned int stride,
    const struct cadence *count
)
{
    printf(
        "%s PWM_FREQ=%u EXPECTED_SAMPLE_FREQ=%u ACTUAL_VALID_SAMPLE_FREQ=%.3f "
        "TRIGGERS=%u VALID=%u H0=%u ACCEPTED=%u LOADED=%u MISSED_EVENTS=%u "
        "DEADLINE_VIOLATIONS=%u ADC_LATE=%u H0_LATE=%u DUPLICATES=%u\n",
        name,
        1000000U / pwm_us,
        1000000U / (pwm_us * stride),
        1.0e6 * (double)count->valid / WINDOW_US,
        count->trigger,
        count->valid,
        count->h0,
        count->accepted,
        count->loaded,
        count->missed,
        count->late_adc + count->late_h0,
        count->late_adc,
        count->late_h0,
        count->duplicate
    );
}

static int test_legacy_edges(void)
{
    const unsigned int delays[] = {40U, 60U};
    unsigned int ready_us = 0U;
    for (unsigned int stage = 0U; stage < 2U; ++stage) {
        struct cadence count = {0};
        unsigned int last_edge = 0U;
        for (unsigned int t = stage * WINDOW_US; t < (stage + 1U) * WINDOW_US;
             t += LEGACY_EDGE_US) {
            if (t < ready_us) {
                continue;
            }
            const unsigned int edge = t / LEGACY_EDGE_US;
            if (count.valid != 0U) {
                CHECK(edge - last_edge == stage + 2U);
            }
            last_edge = edge;
            count.trigger++;
            count.valid++;
            ready_us = t + delays[stage];
        }
        /* Expected opportunities are per carrier, not per candidate edge. */
        count.missed = WINDOW_US / PWM_20K_US - count.valid;
        print_cadence(
            stage == 0U ? "legacy-two-edges" : "legacy-three-edges",
            PWM_20K_US,
            1U,
            &count
        );
        CHECK(count.valid == (stage == 0U ? 60U : 40U));
        CHECK(count.missed == (stage == 0U ? 0U : 20U));
    }

    /* Hold rearm until the 100-us edge. Conversion sequence remains dense,
     * but carrier identity exposes the missing 50-us opportunity. */
    const unsigned int expected_times[] = {0U, 100U, 150U, 200U};
    unsigned int accepted = 0U;
    unsigned int last_carrier = 0U;
    unsigned int missed = 0U;
    ready_us = 0U;
    for (unsigned int t = 0U; t <= 200U; t += LEGACY_EDGE_US) {
        if (t >= ready_us) {
            CHECK(accepted < sizeof expected_times / sizeof expected_times[0]);
            CHECK(t == expected_times[accepted]);
            const unsigned int carrier = t / PWM_20K_US;
            if (accepted != 0U) {
                missed += carrier - last_carrier - 1U;
            }
            CHECK(carrier == accepted + missed);
            last_carrier = carrier;
            ready_us = t + (accepted == 0U ? 100U : delays[0]);
            accepted++;
        }
    }
    CHECK(accepted == 4U && missed == 1U && last_carrier == 4U);
    printf(
        "legacy-rearm-recovery last_conversion_seq=%u last_carrier_id=%u missed=%u\n",
        accepted - 1U,
        last_carrier,
        missed
    );
    return 0;
}

static int test_load_contract(void)
{
    const struct {
        const char *name;
        unsigned int period_us;
        unsigned int stride;
        bool inject;
    } profiles[] = {
        {"20/20", 50U, 1U, false},
        {"10/10", 100U, 1U, false},
        {"20/10", 50U, 2U, false},
        {"20/20-faults", 50U, 1U, true},
    };
    for (unsigned int p = 0U; p < sizeof profiles / sizeof profiles[0]; ++p) {
        const unsigned int period = profiles[p].period_us;
        const unsigned int stride = profiles[p].stride;
        const unsigned int slots = WINDOW_US / period;
        struct cadence count = {0};
        unsigned int last_load = 0U;
        unsigned int last_sample_id = 0U;
        for (unsigned int carrier = 0U; carrier < slots; carrier += stride) {
            if (profiles[p].inject && carrier == 1U) {
                count.missed++;
                continue;
            }
            const unsigned int trigger_us = carrier * period + 10U;
            const unsigned int done_us =
                trigger_us + (profiles[p].inject && carrier == 2U ? 40U : 8U);
            const unsigned int write_us =
                done_us + (profiles[p].inject && carrier == 3U ? 32U : 16U);
            const unsigned int load_us = (carrier + 1U) * period;
            count.trigger++;
            /* Candidate completion identity comes from the ADC trigger token,
             * even if completion is too late to serve this cycle. */
            const unsigned int sample_id = carrier + 1U;
            const unsigned int completions = profiles[p].inject && carrier == 4U ? 2U : 1U;
            for (unsigned int i = 0U; i < completions; ++i) {
                if (sample_id == last_sample_id) {
                    count.duplicate++;
                } else {
                    CHECK(sample_id > last_sample_id);
                    last_sample_id = sample_id;
                    count.valid++;
                }
            }
            if (done_us >= load_us - LOAD_SETUP_US) {
                count.late_adc++;
                continue;
            }
            count.h0++;
            if (write_us >= load_us - LOAD_SETUP_US) {
                count.late_h0++;
                continue;
            }
            count.accepted++;
            const unsigned int loaded_carrier = load_us / period;
            CHECK(loaded_carrier == last_sample_id && loaded_carrier > last_load);
            if (!profiles[p].inject) {
                CHECK(loaded_carrier == count.loaded * stride + 1U);
            }
            last_load = loaded_carrier;
            count.loaded++;
        }
        print_cadence(profiles[p].name, period, stride, &count);
        CHECK(count.trigger + count.missed == slots / stride);
        CHECK(count.valid == count.trigger && count.h0 + count.late_adc == count.valid);
        CHECK(count.loaded == count.accepted);
        CHECK(count.accepted + count.late_h0 == count.h0);
        CHECK(last_load == slots - stride + 1U);
        if (profiles[p].inject) {
            CHECK(count.missed == 1U && count.duplicate == 1U);
            CHECK(count.late_adc == 1U && count.late_h0 == 1U && count.loaded == 57U);
        } else {
            CHECK(count.loaded == slots / stride && count.duplicate == 0U);
        }
    }
    return 0;
}

struct board_fixture {
    struct foc_runtime runtime;
    struct foc_board_output output;
    struct foc_board_adc adc;
    struct foc_board_encoder encoder;
    struct foc_adc_test_clock clock;
};

#if defined(FALCONFOC_RT02_TIMING_TRACE)
static uint32_t mock_trace_cycles;

static uint32_t test_trace_clock(void)
{
    return mock_trace_cycles++;
}
#endif

static int board_init(struct board_fixture *board, uint32_t epoch)
{
    board->clock.epoch = epoch;
    CHECK(foc_board_init(&board->runtime, &board->output, epoch) == FOC_RUNTIME_OK);
    CHECK(foc_board_adc_init_timing(&board->adc, epoch, epoch, 0U));
    return 0;
}

static bool adc_begin(struct board_fixture *board, uint64_t elapsed_us, bool pcce_pending)
{
    struct foc_board_adc_event event = foc_adc_test_event(&board->clock, elapsed_us);
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    event.irq_entry_cycles = test_trace_clock();
    event.irq_entry_phase_ticks = event.phase_ticks;
#endif
    const bool accepted = foc_board_adc_begin(&board->runtime, &board->adc, &event, pcce_pending);
    foc_board_adc_publish(&board->adc);
    return accepted;
}

static int adc_complete(struct board_fixture *board, uint64_t elapsed_us, bool drained)
{
    struct foc_board_adc_event event = foc_adc_test_event(&board->clock, elapsed_us);
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    event.irq_entry_cycles = test_trace_clock();
#endif
    const int result = foc_board_adc_complete(
        &board->runtime,
        &board->adc,
        &board->output,
        ranks,
        4U,
        &event,
        drained
    );
    foc_board_adc_publish(&board->adc);
    return result;
}

static int adc_cycle(struct board_fixture *board, uint32_t captured_us, uint32_t done_us)
{
    if (!adc_begin(board, captured_us, false)) {
        return FOC_RUNTIME_ERR_ORDER;
    }
    return adc_complete(board, done_us, true);
}

static enum foc_board_shadow_result
model_trigger_deadline(const struct board_fixture *board, uint32_t pcce_phase_us)
{
    /* Compare deadline feasibility under equal, hypothetical H0 and commit
     * cost. Runtime itself remains locked and never admits these writes. */
    struct foc_board_adc_transaction token = board->adc.live.transaction;
    token.h0_called = true;
    token.h0_consumed = true;
    token.h0_result = FOC_RUNTIME_OK;
    token.update_accepted = true;
    struct foc_board_shadow shadow = {0};
    const struct foc_board_load_plan plan =
        {.period_ns = FOC_BOARD_H0_US * 1000U, .setup_ns = 2000U, .load = {token.slot_id, 62000U}};
    const uint32_t pcce_ns = pcce_phase_us * 1000U;
    const struct foc_board_shadow_attempt attempt = {
        .sample_cycle = token.slot_id,
        .generation = token.generation,
        .eoc = {token.slot_id, pcce_ns},
        .h0_start = {token.slot_id, pcce_ns},
        .h0_finish = {token.slot_id, pcce_ns + 7000U},
        .shadow_write = {token.slot_id, pcce_ns + 8000U},
        .window_valid = true,
    };
    return foc_board_shadow_stage(&shadow, &plan, &token, &attempt);
}

static int test_trigger_candidates(void)
{
    struct board_fixture soft = {0};
    struct board_fixture hard = {0};
    struct board_fixture unknown = {0};
    struct foc_board_adc_route_config route;
    CHECK(foc_board_adc_route_config(FOC_BOARD_ADC_SOFTWARE, &route));
    CHECK(route.ctrl2 == 0xF000U && route.iomux_remap7_mask == 0U);
    CHECK(foc_board_adc_route_config(FOC_BOARD_ADC_TMR1_CH4, &route));
    CHECK(route.ctrl2 == 0x9000U && route.ctrl2_mask == 0x100F000U);
    CHECK(foc_board_adc_route_config(FOC_BOARD_ADC_REMAPPED_CH4, &route));
    CHECK(route.ctrl2 == 0xE000U && route.iomux_remap7_mask == 0x10U);
    CHECK(!foc_board_adc_route_config(FOC_BOARD_ADC_TMR1_TRGOUT, &route));
    CHECK(board_init(&soft, UINT32_MAX - 44U) == 0);
    CHECK(board_init(&hard, UINT32_MAX - 44U) == 0);
    CHECK(board_init(&unknown, 0U) == 0);
    const struct foc_board_adc_event witnessed = foc_adc_test_event(&unknown.clock, 40U);
    CHECK(!foc_board_adc_candidate_begin(&unknown.runtime, &unknown.adc, &witnessed, false, false));
    CHECK(adc_complete(&unknown, 48U, true) == FOC_RUNTIME_ERR_ORDER);
    CHECK(unknown.adc.live.h0_consumed == 0U);
    struct board_fixture wrong_phase = {0};
    CHECK(board_init(&wrong_phase, 0U) == 0);
    const struct foc_board_adc_event late_compare = foc_adc_test_event(&wrong_phase.clock, 41U);
    CHECK(!foc_board_adc_candidate_begin(
        &wrong_phase.runtime,
        &wrong_phase.adc,
        &late_compare,
        true,
        false
    ));
    for (uint32_t i = 0U; i < 5U; ++i) {
        const uint32_t compare = i * FOC_BOARD_H0_US + FOC_BOARD_ADC_COMPARE_US;
        const uint32_t irq_delay = 3U * i;
        const struct foc_board_adc_event event = foc_adc_test_event(&hard.clock, compare);
        CHECK(adc_begin(&soft, compare + irq_delay, false));
        CHECK(foc_board_adc_candidate_begin(&hard.runtime, &hard.adc, &event, true, false));
        CHECK(soft.adc.live.transaction.generation == hard.adc.live.transaction.generation);
        CHECK(soft.adc.live.transaction.slot_id == hard.adc.live.transaction.slot_id);
        CHECK(
            soft.adc.live.transaction.requested_us - hard.adc.live.transaction.requested_us ==
            irq_delay
        );
        const uint32_t pcce_delay = i == 4U ? 90U : 8U;
        CHECK(
            adc_complete(&soft, compare + irq_delay + pcce_delay, true) ==
            adc_complete(&hard, compare + pcce_delay, true)
        );
        CHECK(soft.adc.live.transaction.adc_valid == (i < 4U));
        CHECK(hard.adc.live.transaction.adc_valid == (i < 4U));
        CHECK(!soft.adc.live.transaction.physical_output_applied);
        CHECK(!hard.adc.live.transaction.physical_output_applied);
        const uint32_t pcce_phase = FOC_BOARD_ADC_COMPARE_US + pcce_delay;
        const enum foc_board_shadow_result soft_expected = i < 2U ? FOC_BOARD_SHADOW_STAGED
            : i < 4U                                              ? FOC_BOARD_SHADOW_DEADLINE
                                                                  : FOC_BOARD_SHADOW_INVALID;
        CHECK(model_trigger_deadline(&soft, pcce_phase + irq_delay) == soft_expected);
        CHECK(
            model_trigger_deadline(&hard, pcce_phase) ==
            (i < 4U ? FOC_BOARD_SHADOW_STAGED : FOC_BOARD_SHADOW_INVALID)
        );
    }
    CHECK(soft.adc.live.adc_completions == 4U && hard.adc.live.adc_completions == 4U);
    CHECK(soft.adc.live.late_completions == 1U && hard.adc.live.late_completions == 1U);
    puts("RT-02 A/B model: 0/3/6/9/12 us injected IRQ delay, 4 valid + 1 late per path");
    puts("RT-02 A/B modeled deadline: A 2 timely + 2 misses, B 4 timely + 0 misses");
    return 0;
}

static int test_rank_apertures(void)
{
    struct foc_board_adc_scan scan;
    CHECK(foc_board_adc_scan_at(23200U, &scan));
    CHECK(scan.rank[FOC_BOARD_RANK_VBUS].start_ns == 23200U);
    CHECK(scan.rank[FOC_BOARD_RANK_VBUS].end_ns == 23800U);
    CHECK(scan.rank[FOC_BOARD_RANK_IC].start_ns == 24800U);
    CHECK(scan.rank[FOC_BOARD_RANK_IB].start_ns == 26400U);
    CHECK(scan.rank[FOC_BOARD_RANK_IA].end_ns == 28600U);
    CHECK(scan.complete_ns == 29600U);
    CHECK(!foc_board_adc_scan_at(UINT32_MAX - 6399U, &scan));

    struct foc_board_aperture leg[3];
    for (unsigned int i = 0U; i < 3U; ++i) {
        CHECK(foc_board_low_side_window(50000U, 13000U + 1000U * i, 2000U, 1000U, &leg[i]));
    }
    CHECK(foc_board_adc_current_window(&scan, leg));
    CHECK(foc_board_low_side_window(50000U, 23000U, 2000U, 1000U, &leg[2]));
    CHECK(!foc_board_adc_current_window(&scan, leg)); /* IA ends beyond its closing edge. */
    CHECK(!foc_board_low_side_window(50000U, 24000U, 2000U, 1000U, &leg[2]));
    CHECK(foc_board_adc_scan_at(41000U, &scan));
    CHECK(!foc_board_adc_current_window(&scan, leg)); /* Entire group sampled too late. */
    puts("RT-02 rank aperture, per-phase windows and high-duty rejection verified");
    return 0;
}

static int test_shadow_deadlines(void)
{
    struct foc_board_shadow shadow = {0};
    struct foc_board_load_plan plan =
        {.period_ns = 50000U, .setup_ns = 2000U, .load = {UINT32_MAX, 25000U}};
    /* Model the admission that a future output adapter could grant. Board1's
     * real adapter continues to refuse that request. */
    struct foc_board_adc_transaction token = {
        .slot_id = UINT32_MAX,
        .generation = UINT32_MAX,
        .state = FOC_BOARD_ADC_COMPLETED,
        .adc_valid = true,
        .h0_called = true,
        .h0_consumed = true,
        .h0_result = FOC_RUNTIME_OK,
        .update_accepted = true,
    };
    struct foc_board_shadow_attempt attempt = {
        .sample_cycle = token.slot_id,
        .generation = token.generation,
        .eoc = {UINT32_MAX, 16600U},
        .h0_start = {UINT32_MAX, 17800U},
        .h0_finish = {UINT32_MAX, 22400U},
        .shadow_write = {UINT32_MAX, 22999U},
        .window_valid = true,
    };
    CHECK(foc_board_shadow_stage(&shadow, &plan, &token, &attempt) == FOC_BOARD_SHADOW_STAGED);
    CHECK(shadow.pending && !shadow.applied_seen && !token.physical_output_applied);
    CHECK(foc_board_shadow_load(&shadow, plan.load, true, true) == FOC_BOARD_SHADOW_MODEL_APPLIED);
    CHECK(foc_board_shadow_load(&shadow, plan.load, true, true) == FOC_BOARD_SHADOW_INVALID);
    CHECK(foc_board_shadow_stage(&shadow, &plan, &token, &attempt) == FOC_BOARD_SHADOW_INVALID);

    plan.load = (struct foc_board_carrier_stamp){0U, 0U};
    struct foc_board_shadow other = {0};
    CHECK(foc_board_shadow_stage(&other, &plan, &token, &attempt) == FOC_BOARD_SHADOW_STAGED);
    CHECK(foc_board_shadow_load(&other, plan.load, true, true) == FOC_BOARD_SHADOW_MODEL_APPLIED);
    token.slot_id = 0U;
    token.generation = 0U;
    attempt.sample_cycle = 0U;
    attempt.generation = 0U;
    attempt.eoc.cycle = 0U;
    attempt.h0_start.cycle = 0U;
    attempt.h0_finish.cycle = 0U;
    attempt.shadow_write.cycle = 0U;
    CHECK(foc_board_shadow_stage(&other, &plan, &token, &attempt) == FOC_BOARD_SHADOW_INVALID);
    plan.load = (struct foc_board_carrier_stamp){0U, 25000U};
    CHECK(foc_board_shadow_stage(&shadow, &plan, &token, &attempt) == FOC_BOARD_SHADOW_STAGED);
    CHECK(
        foc_board_shadow_load(&shadow, (struct foc_board_carrier_stamp){1U, 0U}, true, true) ==
        FOC_BOARD_SHADOW_INVALID
    );
    CHECK(foc_board_shadow_load(&shadow, plan.load, true, true) == FOC_BOARD_SHADOW_INVALID);

    for (unsigned int reason = 0U; reason < 9U; ++reason) {
        struct foc_board_shadow candidate = {0};
        struct foc_board_shadow_attempt sample = attempt;
        struct foc_board_adc_transaction frame = token;
        switch (reason) {
        case 0:
            sample.shadow_write.phase_ns = 23000U;
            break;
        case 1:
            sample.h0_finish.phase_ns = 24000U;
            sample.shadow_write.phase_ns = 24100U;
            break;
        case 2:
            sample.generation++;
            break;
        case 3:
            sample.sample_cycle++;
            break;
        case 4:
            frame.update_accepted = false;
            frame.update_declined = true;
            break;
        case 5:
            frame.h0_result = FOC_RUNTIME_ERR_STOP;
            break;
        case 6:
            sample.window_valid = false;
            break;
        case 7:
            frame.state = FOC_BOARD_ADC_DRAIN;
            break;
        case 8:
            sample.eoc.cycle--;
            break;
        }
        CHECK(
            foc_board_shadow_stage(&candidate, &plan, &frame, &sample) ==
            (reason < 2U ? FOC_BOARD_SHADOW_DEADLINE : FOC_BOARD_SHADOW_INVALID)
        );
        CHECK(foc_board_shadow_load(&candidate, plan.load, true, true) == FOC_BOARD_SHADOW_INVALID);
    }
    other = (struct foc_board_shadow){0};
    CHECK(foc_board_shadow_stage(&other, &plan, &token, &attempt) == FOC_BOARD_SHADOW_STAGED);
    CHECK(foc_board_shadow_load(&other, plan.load, false, true) == FOC_BOARD_SHADOW_INVALID);
    CHECK(foc_board_shadow_load(&other, plan.load, true, true) == FOC_BOARD_SHADOW_INVALID);
    other = (struct foc_board_shadow){0};
    CHECK(foc_board_shadow_stage(&other, &plan, &token, &attempt) == FOC_BOARD_SHADOW_STAGED);
    CHECK(foc_board_shadow_load(&other, plan.load, true, false) == FOC_BOARD_SHADOW_INVALID);
    CHECK(foc_board_shadow_load(&other, plan.load, true, true) == FOC_BOARD_SHADOW_INVALID);
    puts("RT-02 shadow stage/load separation, deadline, stale output and wrap verified");
    return 0;
}

static int test_optional_trace(void)
{
    struct board_fixture traced = {0};
    struct board_fixture silent = {0};
    CHECK(board_init(&traced, 0U) == 0 && board_init(&silent, 0U) == 0);
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    traced.adc.trace_cycles = test_trace_clock;
    mock_trace_cycles = UINT32_MAX;
#endif
    struct foc_board_adc_diagnostics snapshot;
    CHECK(adc_begin(&traced, 40U, false));
    CHECK(foc_board_adc_read(&traced.adc, &snapshot) == FOC_RUNTIME_ERR_BUSY);
    const int observed = adc_complete(&traced, 48U, true);
    CHECK(adc_cycle(&silent, 40U, 48U) == observed);
    CHECK(foc_board_adc_read(&traced.adc, &snapshot) == FOC_RUNTIME_OK);
    CHECK(snapshot.transaction.adc_valid && !snapshot.transaction.physical_output_applied);
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    const struct foc_board_adc_trace *trace = &snapshot.transaction.trace;
    CHECK(silent.adc.live.transaction.trace.points == 0U);
    CHECK(trace->compare_irq_cycles == UINT32_MAX && trace->pcce_irq_cycles == 0U);
    CHECK(trace->compare_irq_phase_ticks == 40U * (FOC_BOARD_TIMER_HZ / FOC_BOARD_US_HZ));
    CHECK(
        (trace->points & (FOC_BOARD_TRACE_PCCE_ENTRY | FOC_BOARD_TRACE_H0)) ==
        (FOC_BOARD_TRACE_PCCE_ENTRY | FOC_BOARD_TRACE_H0)
    );
    CHECK(trace->h0_start_cycles > trace->pcce_irq_cycles);
    CHECK(trace->h0_finish_cycles > trace->h0_start_cycles);
    CHECK(traced.output.trace_owner == NULL);
    puts("RT-02 ON: trace markers, wrapping cycle zero, snapshot and instance isolation");
#else
    puts("RT-02 OFF: timing contract, snapshot and output lock verified without trace");
#endif
    return 0;
}

static int test_adc_events(void)
{
    struct board_fixture a = {0};
    struct board_fixture b = {0};
    CHECK(board_init(&a, 0U) == 0 && board_init(&b, 0U) == 0);
    CHECK(adc_cycle(&a, 40U, 48U) == FOC_RUNTIME_ERR_ALIGNMENT);
    CHECK(a.runtime.adc.produced_us == 40U && a.runtime.adc.seq == 0U);
    /* The first flag could be from the old period. The real CH4 may follow. */
    CHECK(!adc_begin(&a, 215U, false));
    CHECK(a.adc.live.last_rejection == FOC_BOARD_ADC_TRIGGER_WINDOW);
    CHECK(adc_begin(&a, 240U, false));
    CHECK(!adc_begin(&a, 440U, true));
    CHECK(a.adc.live.transaction.state == FOC_BOARD_ADC_DRAIN);
    CHECK(a.adc.live.transaction.slot_id == 1U && a.adc.live.transaction.generation == 1U);
    CHECK(adc_complete(&a, 448U, true) == FOC_RUNTIME_ERR_ORDER);
    CHECK(a.adc.live.transaction.state == FOC_BOARD_ADC_REJECTED && a.runtime.adc.seq == 0U);
    CHECK(adc_cycle(&a, 640U, 648U) == FOC_RUNTIME_ERR_TIMING);
    CHECK(a.runtime.h0.missed == 2U && a.runtime.pipeline.mock.updates == 0U);
    CHECK(adc_complete(&a, 649U, true) == FOC_RUNTIME_ERR_ORDER);
    CHECK(adc_cycle(&a, 840U, 921U) == FOC_RUNTIME_ERR_ORDER);
    CHECK(a.adc.live.trigger_requests == 4U && a.output.result == FOC_BOARD_INHIBIT_SOFTWARE);
    CHECK(a.adc.live.missing_slots == 2U && a.adc.live.late_completions == 2U);
    CHECK(a.adc.live.overlap_rejections == 1U && a.adc.live.unmatched_completions == 1U);
    CHECK(a.adc.live.h0_consumed == 1U && a.adc.live.adc_completions == 2U);
    CHECK(adc_cycle(&b, 40U, 48U) == FOC_RUNTIME_ERR_ALIGNMENT);
    CHECK(adc_cycle(&b, 240U, 248U) == FOC_RUNTIME_ERR_ALIGNMENT);
    CHECK(b.runtime.adc.seq == 1U && b.runtime.h0.missed == 0U);
    CHECK(b.runtime.pipeline.safety.latched_faults == 0U && b.output.declined_updates == 0U);
    puts(
        "board-adc late-drain, duplicate, deadline, recovery identity, independent instance checked"
    );
    return 0;
}

static int test_slots_and_clock(void)
{
    struct board_fixture b = {0};
    CHECK(board_init(&b, 0U) == 0);
    CHECK(adc_cycle(&b, 40U, 48U) == FOC_RUNTIME_ERR_ALIGNMENT);
    CHECK(adc_cycle(&b, 240U, 248U) == FOC_RUNTIME_ERR_ALIGNMENT);
    CHECK(adc_cycle(&b, 640U, 648U) == FOC_RUNTIME_ERR_TIMING);
    CHECK(b.adc.live.missing_slots == 1U && b.adc.live.transaction.slot_id == 3U);
    CHECK(adc_cycle(&b, 1440U, 1448U) == FOC_RUNTIME_ERR_TIMING);
    CHECK(b.adc.live.missing_slots == 4U && b.adc.live.observed_slots == 4U);
    CHECK(b.adc.live.transaction.slot_id == 7U && b.adc.live.transaction.generation == 3U);
    CHECK(!adc_begin(&b, 1550U, false));
    CHECK(b.adc.live.last_rejection == FOC_BOARD_ADC_TRIGGER_WINDOW);
    CHECK(adc_begin(&b, 1640U, false));
    CHECK(!adc_begin(&b, 1641U, false));
    CHECK(b.adc.live.last_rejection == FOC_BOARD_ADC_DUPLICATE_TRIGGER);
    CHECK(adc_complete(&b, 1648U, true) == FOC_RUNTIME_ERR_STOP);
    CHECK(b.adc.live.transaction.h0_called && !b.adc.live.transaction.h0_consumed);
    CHECK(!b.adc.live.transaction.update_accepted);

    struct board_fixture wrap = {0};
    CHECK(board_init(&wrap, UINT32_MAX - 44U) == 0);
    CHECK(adc_cycle(&wrap, 40U, 48U) == FOC_RUNTIME_ERR_ALIGNMENT);
    CHECK(wrap.adc.live.transaction.slot_id == 0U && wrap.adc.live.transaction.completed_us == 3U);
    CHECK(adc_cycle(&wrap, 240U, 248U) == FOC_RUNTIME_ERR_ALIGNMENT);
    CHECK(wrap.adc.live.transaction.slot_id == 1U && wrap.adc.live.h0_consumed == 2U);

    for (unsigned int failure = 0U; failure < 5U; ++failure) {
        struct board_fixture f = {0};
        CHECK(board_init(&f, 0U) == 0);
        CHECK(adc_begin(&f, 40U, false));
        const uint64_t time = failure == 0U ? 0x80000028ULL : failure == 1U ? 0x100000028ULL : 240U;
        struct foc_board_adc_event event = foc_adc_test_event(&f.clock, time);
        if (failure == 2U) {
            /* A stopped timer resumed without the rollover expected from TMR2. */
            event.flags &= ~FOC_BOARD_SLOT_WRAP;
        } else if (failure == 3U) {
            event.phase_ticks -= 1500U;
        } else if (failure == 4U) {
            event.coherent = false;
        }
        CHECK(!foc_board_adc_begin(&f.runtime, &f.adc, &event, false));
        CHECK(!f.adc.live.clock_valid && f.adc.live.unattributable_events == 1U);
        CHECK(adc_complete(&f, time + 8U, true) == FOC_RUNTIME_ERR_ORDER);
        CHECK(f.adc.live.transaction.state == FOC_BOARD_ADC_REJECTED);
        CHECK(!adc_begin(&f, time + 200U, false));
        CHECK(f.adc.live.trigger_requests == 1U && !f.runtime.adc.seen);
    }

    struct board_fixture epoch = {0};
    CHECK(!foc_board_adc_init_timing(&epoch.adc, 0U, 3U, 0U));
    CHECK(!foc_board_adc_init_timing(&epoch.adc, 0U, 0U, 0U));
    struct board_fixture nonzero = {0};
    CHECK(!foc_board_adc_init_timing(&nonzero.adc, 0U, 0U, 1U));
    struct board_fixture bracket = {0};
    CHECK(foc_board_init(&bracket.runtime, &bracket.output, 0U) == FOC_RUNTIME_OK);
    CHECK(foc_board_adc_init_timing(&bracket.adc, 0U, 2U, 0U));
    bracket.clock.epoch = 1U;
    struct foc_board_adc_event event = foc_adc_test_event(&bracket.clock, 40U);
    event.before_us--;
    CHECK(foc_board_adc_begin(&bracket.runtime, &bracket.adc, &event, false));
    CHECK(adc_complete(&bracket, 48U, true) == FOC_RUNTIME_ERR_ALIGNMENT);
    bracket.adc.live.missing_slots = UINT32_MAX - 1U;
    bracket.adc.live.slot_id = UINT32_MAX - 2U;
    CHECK(adc_cycle(&bracket, 640U, 648U) == FOC_RUNTIME_ERR_TIMING);
    CHECK(bracket.adc.live.missing_slots == UINT32_MAX && bracket.adc.live.slot_id == 0U);
    puts(
        "RT-01 slots single=1 total=4; timer wrap, half/full-range ambiguity, phase, pause, epoch "
        "and saturation checked"
    );
    return 0;
}

static int test_drain_and_diagnostics(void)
{
    struct board_fixture crossed = {0};
    CHECK(board_init(&crossed, 0U) == 0);
    CHECK(adc_begin(&crossed, 110U, false));
    CHECK(adc_complete(&crossed, 203U, true) == FOC_RUNTIME_ERR_ORDER);
    CHECK(crossed.adc.live.slot_id == 1U && crossed.adc.live.transaction.slot_id == 0U);
    CHECK(crossed.adc.live.missing_slots == 1U && !crossed.adc.live.transaction.h0_called);

    struct board_fixture b = {0};
    CHECK(board_init(&b, 0U) == 0);
    CHECK(!adc_begin(&b, 40U, true));
    CHECK(b.adc.live.last_rejection == FOC_BOARD_ADC_UNDRAINED_PCCE);
    CHECK(adc_complete(&b, 48U, true) == FOC_RUNTIME_ERR_ORDER);
    CHECK(!adc_begin(&b, 50U, false));
    CHECK(adc_begin(&b, 240U, false));
    CHECK(adc_complete(&b, 248U, false) == FOC_RUNTIME_ERR_ORDER);
    CHECK(b.adc.live.transaction.state == FOC_BOARD_ADC_DRAIN);
    CHECK(!adc_begin(&b, 440U, true));
    CHECK(adc_complete(&b, 448U, true) == FOC_RUNTIME_ERR_ORDER);
    CHECK(b.adc.live.transaction.slot_id == 1U && !b.adc.live.transaction.h0_called);
    CHECK(adc_begin(&b, 640U, false));
    struct foc_board_adc_diagnostics view = {.slot_id = 123U};
    CHECK(foc_board_adc_read(&b.adc, &view) == FOC_RUNTIME_ERR_BUSY && view.slot_id == 123U);
    CHECK(adc_complete(&b, 648U, true) == FOC_RUNTIME_ERR_TIMING);
    CHECK(b.adc.live.missing_slots == 3U);
    CHECK(adc_begin(&b, 840U, false));
    CHECK(foc_board_adc_read(&b.adc, &view) == FOC_RUNTIME_OK);
    CHECK(
        view.slot_id == 3U && view.trigger_requests == 2U && view.transaction.completed_us == 648U
    );
    CHECK(view.transaction.slot_id == 3U && view.transaction.diagnostic_load_us == 800U);
    CHECK(
        view.transaction.h0_called && !view.transaction.h0_consumed &&
        !view.transaction.update_accepted
    );
    CHECK(foc_board_adc_read(&b.adc, &view) == FOC_RUNTIME_ERR_BUSY);
    CHECK(adc_complete(&b, 848U, true) == FOC_RUNTIME_ERR_CONTROL);
    CHECK(foc_board_adc_read(&b.adc, &view) == FOC_RUNTIME_OK);
    CHECK(view.slot_id == 4U && view.transaction.h0_consumed && view.h0_consumed == 1U);
    puts(
        "RT-01 stale PCCE and failed clear drain; foreground snapshot remains coherent while a new "
        "token is issued"
    );
    return 0;
}
struct preemption {
    struct board_fixture *board;
    enum foc_runtime_hook_point point;
    uint32_t captured_us;
    uint32_t done_us;
    uint32_t prediction_us;
    uint32_t fault_bits;
    int result;
    unsigned int hits;
    unsigned int max_hits;
};

static void
interrupt_at_hook(struct foc_runtime *runtime, enum foc_runtime_hook_point point, void *argument)
{
    struct preemption *event = argument;
    const unsigned int limit = event->max_hits == 0U ? 1U : event->max_hits;
    if (point != event->point || event->hits >= limit) {
        return;
    }
    const uint32_t offset = event->hits * FOC_BOARD_H0_US;
    event->hits++;
    if (point == FOC_RUNTIME_HOOK_BEFORE_SUBMIT) {
        foc_runtime_request_inhibit(runtime, 0U);
        return;
    }
    if (event->fault_bits != 0U) {
        foc_runtime_request_inhibit(runtime, event->fault_bits);
    }
    event->result = adc_cycle(event->board, event->captured_us + offset, event->done_us + offset);
    event->prediction_us = runtime->angle_prediction_us;
}

static int test_encoder_and_snapshot(void)
{
    struct board_fixture b = {0};
    CHECK(board_init(&b, 0U) == 0);
    CHECK(foc_board_encoder_begin(&b.encoder, 0U));
    CHECK(
        foc_board_encoder_complete(&b.runtime, &b.encoder, 70U, FOC_BOARD_ENCODER_OK, 100U) ==
        FOC_RUNTIME_ERR_INPUT
    );
    for (uint32_t t = 80U; t < 500U; t += FOC_BOARD_H0_US) {
        CHECK(adc_cycle(&b, t, t + 8U) == FOC_RUNTIME_ERR_ALIGNMENT);
    }
    CHECK(foc_board_encoder_begin(&b.encoder, 500U));
    CHECK(
        foc_board_encoder_complete(&b.runtime, &b.encoder, 570U, FOC_BOARD_ENCODER_OK, 100U) ==
        FOC_RUNTIME_OK
    );
    CHECK(adc_cycle(&b, 680U, 688U) == FOC_RUNTIME_IDLE);
    CHECK(adc_cycle(&b, 880U, 888U) == FOC_RUNTIME_IDLE);

    /* Force H0 ahead of H1's release store while publication is in progress.
     * The ADC trigger precedes the nested completion. The 1100-us H1 completion
     * is on the Board Port window boundary. */
    struct preemption event = {
        .board = &b,
        .point = FOC_RUNTIME_HOOK_SENSOR_WRITING,
        .captured_us = 1080U,
        .done_us = 1100U,
    };
    b.runtime.config.hook = interrupt_at_hook;
    b.runtime.config.hook_arg = &event;
    CHECK(foc_board_encoder_begin(&b.encoder, 1000U));
    CHECK(
        foc_board_encoder_complete(&b.runtime, &b.encoder, 1100U, FOC_BOARD_ENCODER_OK, 99U) ==
        FOC_RUNTIME_OK
    );
    CHECK(event.hits == 1U && event.result == FOC_RUNTIME_IDLE && event.prediction_us == 580U);
    CHECK(adc_cycle(&b, 1280U, 1288U) == FOC_RUNTIME_IDLE);
    CHECK(b.runtime.angle_prediction_us == 280U);

    event = (struct preemption){
        .board = &b,
        .point = FOC_RUNTIME_HOOK_TELEMETRY_READING,
        .captured_us = 1480U,
        .done_us = 1488U,
    };
    struct foc_runtime_snapshot view = {.updates = 123U};
    CHECK(foc_runtime_read_snapshot(&b.runtime, &view) == FOC_RUNTIME_ERR_BUSY);
    CHECK(view.updates == 123U && event.hits == 1U && event.result == FOC_RUNTIME_IDLE);
    CHECK(foc_runtime_read_snapshot(&b.runtime, &view) == FOC_RUNTIME_OK);
    CHECK(view.fast_us == 1488U && view.angle_prediction_us == 480U);
    CHECK(adc_cycle(&b, 1680U, 1688U) == FOC_RUNTIME_ERR_ALIGNMENT);
    CHECK(b.runtime.angle_prediction_us == 0U);
    puts("board-h1 old angle age=580us after publish=280us; BUSY and stale checked");
    return 0;
}

static int test_spi_close(void)
{
    /* These are the phase/time observations supplied to the live ISR policy. */
    CHECK(foc_board_spi_on_time(FOC_BOARD_SPI_CS, 8U, UINT32_MAX - 1U, 0U));
    CHECK(!foc_board_spi_on_time(FOC_BOARD_SPI_CS, 8U, UINT32_MAX - 1U, 1U));
    CHECK(foc_board_spi_on_time(FOC_BOARD_SPI_SEND, 20U, 200U, 210U));
    CHECK(!foc_board_spi_on_time(FOC_BOARD_SPI_SEND, 20U, 200U, 211U));
    CHECK(!foc_board_spi_on_time(FOC_BOARD_SPI_SEND, 19U, 200U, 201U));
    CHECK(!foc_board_spi_on_time(FOC_BOARD_SPI_CLOSE, 70U, 900U, 899U));
    for (unsigned int hold = 1U; hold <= 2U; ++hold) {
        struct board_fixture b = {0};
        CHECK(foc_board_init(&b.runtime, &b.output, 0U) == FOC_RUNTIME_OK);
        CHECK(foc_board_encoder_begin(&b.encoder, 0U));
        CHECK(
            foc_board_encoder_complete(&b.runtime, &b.encoder, 70U, FOC_BOARD_ENCODER_OK, 100U) ==
            FOC_RUNTIME_ERR_INPUT
        );
        CHECK(foc_board_encoder_begin(&b.encoder, 500U));
        const uint32_t close_us = 570U;
        const uint32_t rx_us = close_us - hold;
        const enum foc_board_encoder_result transport =
            foc_board_spi_close_result(70U, close_us, close_us, rx_us, true, false, false);
        CHECK(transport == (hold == 2U ? FOC_BOARD_ENCODER_OK : FOC_BOARD_ENCODER_TIMEOUT));
        CHECK(
            foc_board_encoder_complete(&b.runtime, &b.encoder, close_us, transport, 99U) ==
            (hold == 2U ? FOC_RUNTIME_OK : FOC_RUNTIME_ERR_INPUT)
        );
        CHECK(b.encoder.result == transport);
        CHECK(b.encoder.feedback.valid == (hold == 2U));
        CHECK(foc_board_encoder_begin(&b.encoder, 1000U));
        CHECK(
            foc_board_encoder_complete(&b.runtime, &b.encoder, 1101U, FOC_BOARD_ENCODER_OK, 99U) ==
            FOC_RUNTIME_ERR_INPUT
        );
        CHECK(b.encoder.result == FOC_BOARD_ENCODER_TIMEOUT);
        CHECK(b.encoder.losses == (hold == 2U ? 1U : 2U));
        CHECK(atomic_load(&b.runtime.stop_requests) != 0U);
    }
    CHECK(
        foc_board_spi_close_result(70U, 570U, 601U, 568U, true, false, false) ==
        FOC_BOARD_ENCODER_TIMEOUT
    );
    CHECK(
        foc_board_spi_close_result(70U, 570U, 570U, 571U, true, false, false) ==
        FOC_BOARD_ENCODER_TIMEOUT
    );
    CHECK(
        foc_board_spi_close_result(70U, 570U, 570U, 568U, false, false, false) ==
        FOC_BOARD_ENCODER_TIMEOUT
    );
    CHECK(
        foc_board_spi_close_result(70U, 570U, 570U, 568U, true, true, false) ==
        FOC_BOARD_ENCODER_TIMEOUT
    );
    CHECK(
        foc_board_spi_close_result(70U, 570U, 570U, 568U, true, false, true) ==
        FOC_BOARD_ENCODER_FRAME
    );
    puts("board-spi close hold 1us rejects, 2us accepts; frame >100us inhibits");
    return 0;
}

static int prepare_snapshot_collision(struct board_fixture *b)
{
    CHECK(board_init(b, 0U) == 0);
    CHECK(foc_board_encoder_begin(&b->encoder, 0U));
    CHECK(
        foc_board_encoder_complete(&b->runtime, &b->encoder, 70U, FOC_BOARD_ENCODER_OK, 100U) ==
        FOC_RUNTIME_ERR_INPUT
    );
    for (uint32_t t = 80U; t < 500U; t += FOC_BOARD_H0_US) {
        CHECK(adc_cycle(b, t, t + 8U) == FOC_RUNTIME_ERR_ALIGNMENT);
    }
    CHECK(foc_board_encoder_begin(&b->encoder, 500U));
    CHECK(
        foc_board_encoder_complete(&b->runtime, &b->encoder, 570U, FOC_BOARD_ENCODER_OK, 100U) ==
        FOC_RUNTIME_OK
    );
    CHECK(adc_cycle(b, 680U, 688U) == FOC_RUNTIME_IDLE);
    CHECK(adc_cycle(b, 880U, 888U) == FOC_RUNTIME_IDLE);
    return 0;
}

static int test_snapshot_collision(void)
{
    for (unsigned int collisions = 1U; collisions <= 2U; ++collisions) {
        struct board_fixture b = {0};
        CHECK(prepare_snapshot_collision(&b) == 0);
        struct preemption event = {
            .board = &b,
            .point = FOC_RUNTIME_HOOK_CONTROL_SNAPSHOT_READING,
            .captured_us = 1080U,
            .done_us = 1100U,
            .max_hits = collisions,
        };
        b.runtime.config.hook = interrupt_at_hook;
        b.runtime.config.hook_arg = &event;
        CHECK(foc_board_encoder_begin(&b.encoder, 1000U));
        const int status =
            foc_board_encoder_complete(&b.runtime, &b.encoder, 1100U, FOC_BOARD_ENCODER_OK, 99U);
        CHECK(event.hits == collisions);
        CHECK(b.runtime.h1.due_count == 3U);
        if (collisions == 1U) {
            CHECK(event.prediction_us == 580U);
            CHECK(status == FOC_RUNTIME_OK && atomic_load(&b.runtime.stop_requests) == 0U);
            CHECK(adc_cycle(&b, 1280U, 1288U) == FOC_RUNTIME_IDLE);
            CHECK(b.runtime.angle_prediction_us == 280U);
        } else {
            CHECK(status == FOC_RUNTIME_ERR_BUSY && atomic_load(&b.runtime.stop_requests) != 0U);
        }
    }
    puts("RT-03 H1 snapshot: one collision publishes, two collisions keep BUSY and request STOP");
    return 0;
}

static int test_fault_preempts_h1(void)
{
    struct board_fixture b = {0};
    CHECK(prepare_snapshot_collision(&b) == 0);
    const uint32_t prior_session = b.runtime.motion_session;
    struct preemption event = {
        .board = &b,
        .point = FOC_RUNTIME_HOOK_SENSOR_WRITING,
        .captured_us = 1080U,
        .done_us = 1100U,
        .fault_bits = FOC_SAFE_FAULT_WDT,
    };
    b.runtime.config.hook = interrupt_at_hook;
    b.runtime.config.hook_arg = &event;
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    b.adc.trace_cycles = test_trace_clock;
    b.encoder.trace_cycles = test_trace_clock;
    mock_trace_cycles = 0U;
#endif
    CHECK(foc_board_encoder_begin(&b.encoder, 1000U));
    CHECK(
        foc_board_encoder_complete(&b.runtime, &b.encoder, 1100U, FOC_BOARD_ENCODER_OK, 99U) ==
        FOC_RUNTIME_OK
    );
    CHECK(event.hits == 1U && event.result == FOC_RUNTIME_ERR_STOP);
    CHECK((b.runtime.pipeline.safety.latched_faults & FOC_SAFE_FAULT_WDT) != 0U);
    CHECK(b.runtime.motion_session != prior_session && !b.runtime.accepted_outer_seen);
    CHECK(!b.adc.live.transaction.physical_output_applied);
#if defined(FALCONFOC_RT02_TIMING_TRACE)
    const struct foc_board_encoder_trace *trace = &b.encoder.trace;
    CHECK(trace->capture_us == 1000U && trace->close_us == 1100U && trace->seq == 2U);
    CHECK(trace->decode_start_cycles < trace->decode_done_cycles);
    CHECK(trace->decode_done_cycles < trace->h1_start_cycles);
    CHECK(trace->h1_start_cycles < b.adc.live.transaction.trace.h0_start_cycles);
    CHECK(b.adc.live.transaction.trace.h0_finish_cycles < trace->h1_finish_cycles);
#endif
    return 0;
}

static int test_supervision_collision(void)
{
    struct board_fixture b = {0};
    CHECK(board_init(&b, 0U) == 0);
    struct preemption event = {
        .board = &b,
        .point = FOC_RUNTIME_HOOK_CONTROL_SNAPSHOT_READING,
        .captured_us = 40U,
        .done_us = 48U,
    };
    b.runtime.config.hook = interrupt_at_hook;
    b.runtime.config.hook_arg = &event;
    struct foc_runtime_foreground_result fg;
    CHECK(foc_runtime_foreground_poll(&b.runtime, 45U, 0U, 0U, &fg) == FOC_RUNTIME_ERR_BUSY);
    CHECK(event.hits == 1U && !fg.supervised && !fg.watchdog_permitted);
    CHECK((atomic_load(&b.runtime.stop_requests) & FOC_SAFE_FAULT_FOREGROUND_HEARTBEAT) == 0U);
    CHECK(foc_runtime_foreground_poll(&b.runtime, 200U, 0U, 0U, &fg) == FOC_RUNTIME_OK);
    CHECK(fg.supervised);
    CHECK(foc_runtime_foreground_poll(&b.runtime, 551U, 0U, 0U, &fg) == FOC_RUNTIME_ERR_TIMING);
    CHECK((atomic_load(&b.runtime.stop_requests) & FOC_SAFE_FAULT_FOREGROUND_HEARTBEAT) != 0U);
    return 0;
}

/* Deterministic fixed-priority execution for the injected workload only.
 * A preempted IRQ resumes ahead of another IRQ at its own priority. */
struct irq_job {
    unsigned int release;
    unsigned int cost;
    unsigned int priority;
    unsigned int vector;
    unsigned int start;
    unsigned int end;
    unsigned int served;
};

static void run_irq_jobs(struct irq_job *jobs, unsigned int count)
{
    for (unsigned int time = 0U; time < 256U; ++time) {
        struct irq_job *next = NULL;
        for (unsigned int i = 0U; i < count; ++i) {
            struct irq_job *job = &jobs[i];
            if (job->release > time || job->served == job->cost) {
                continue;
            }
            const bool same_priority = next != NULL && job->priority == next->priority;
            const bool resume = same_priority && job->served != 0U && next->served == 0U;
            const bool vector_first = same_priority && job->served == 0U && next->served == 0U &&
                job->vector < next->vector;
            if (next == NULL || job->priority < next->priority || resume || vector_first) {
                next = job;
            }
        }
        if (next != NULL) {
            if (next->served == 0U) {
                next->start = time;
            }
            if (++next->served == next->cost) {
                next->end = time + 1U;
            }
        }
    }
}

static int test_fixed_priority_responses(void)
{
    enum {
        THREAD = 3,
        ADC1 = 18,
        TMR1 = 27,
        SPI1 = 35,
        UART = 37,
        TMR5 = 50
    };
    struct irq_job jobs[] = {
        {.release = 0U, .cost = 14U, .priority = THREAD},
        {.release = 1U, .cost = 9U, .priority = 1U, .vector = TMR5},
        {.release = 4U, .cost = 4U, .priority = 0U, .vector = TMR1},
        {.release = 25U, .cost = 4U, .priority = 1U, .vector = SPI1},
        {.release = 26U, .cost = 15U, .priority = 0U, .vector = ADC1},
        {.release = 26U, .cost = 4U, .priority = 0U, .vector = TMR1},
        {.release = 27U, .cost = 3U, .priority = 2U, .vector = UART},
        {.release = 70U, .cost = 4U, .priority = 1U, .vector = TMR5},
    };
    run_irq_jobs(jobs, sizeof jobs / sizeof jobs[0]);
    CHECK(jobs[1].start == 1U && jobs[1].end == 14U && jobs[2].start == 4U);
    CHECK(!foc_board_spi_on_time(FOC_BOARD_SPI_CS, 1U, jobs[1].start, jobs[1].end));
    CHECK(jobs[4].start == 26U && jobs[5].start == 41U && jobs[5].end == 45U);
    CHECK(jobs[3].start == 25U && jobs[3].end == 48U && jobs[6].start == 48U);
    CHECK(jobs[3].end - jobs[3].start - jobs[3].cost == 19U);
    CHECK(jobs[0].end == 53U);
    CHECK(
        foc_board_spi_close_result(
            70U,
            jobs[7].start,
            jobs[7].end,
            jobs[3].end,
            true,
            false,
            false
        ) == FOC_BOARD_ENCODER_OK
    );

    struct irq_job late[] = {
        {.release = 68U, .cost = 29U, .priority = 0U, .vector = ADC1},
        {.release = 73U, .cost = 4U, .priority = 0U, .vector = TMR1},
        {.release = 70U, .cost = 2U, .priority = 1U, .vector = TMR5},
        {.release = 70U, .cost = 2U, .priority = 1U, .vector = SPI1},
    };
    run_irq_jobs(late, sizeof late / sizeof late[0]);
    CHECK(late[3].start == 101U && late[3].end == late[2].start);
    CHECK(
        foc_board_spi_close_result(
            late[2].start,
            late[2].start,
            late[2].end,
            late[3].end,
            true,
            false,
            false
        ) == FOC_BOARD_ENCODER_TIMEOUT
    );
    CHECK((uint32_t)(9U - (UINT32_MAX - 9U)) == 19U);
    puts("HOST_SIMULATED IRQ: SPI elapsed 23 us = 4 code + 19 nested; queued close 105 us rejects");
    return 0;
}

static uint32_t test_serial_clock(void *argument)
{
    return *(const uint32_t *)argument;
}

static int test_busy_foreground(void)
{
    struct board_fixture b = {0};
    CHECK(board_init(&b, 0U) == 0);
    struct foc_uart_buffer uart = {0};
    struct foc_serial serial;
    foc_serial_init(&serial, &b.runtime);
    uint32_t now = 0U;
    for (unsigned int i = 0U; i < 20U; ++i) {
        if (i != 0U) {
            (void)adc_cycle(&b, now - 160U, now - 152U);
        }
        for (unsigned int n = 0U; n < 40U; ++n) {
            foc_uart_rx_push(&uart, 1U, false);
        }
        struct foc_runtime_foreground_result fg;
        CHECK(foc_runtime_foreground_poll(&b.runtime, now, 0U, 0U, &fg) == FOC_RUNTIME_OK);
        CHECK(fg.supervised);
        const unsigned int previous = atomic_load(&uart.rx_tail);
        foc_serial_poll(&serial, &uart, &b.runtime, test_serial_clock, &now);
        const unsigned int consumed = atomic_load(&uart.rx_tail) - previous;
        CHECK(consumed <= FOC_SERIAL_RX_DEFAULT && (i != 0U || consumed == FOC_SERIAL_RX_DEFAULT));
        now += FOC_BOARD_H0_US;
    }
    CHECK(b.runtime.supervision.due_count == 20U && b.runtime.supervision.late_count == 0U);
    CHECK(atomic_load(&uart.rx_lost) > 0U);
    CHECK(!(b.runtime.pipeline.safety.latched_faults & FOC_SAFE_FAULT_FOREGROUND_HEARTBEAT));
    return 0;
}

static int test_delayed_and_over_budget(void)
{
    struct board_fixture b = {0};
    CHECK(board_init(&b, 0U) == 0);
    const struct foc_runtime_fast_input costly = {.now_us = 48U, .cost_us = 101U};
    CHECK(foc_runtime_h0_step(&b.runtime, &costly) == FOC_RUNTIME_ERR_TIMING);
    CHECK(b.runtime.h0.budget_count == 1U && b.runtime.h0.late_count == 0U);
    CHECK(foc_board_encoder_begin(&b.encoder, 0U));
    CHECK(
        foc_board_encoder_complete(&b.runtime, &b.encoder, 101U, FOC_BOARD_ENCODER_TIMEOUT, 0U) ==
        FOC_RUNTIME_ERR_INPUT
    );
    CHECK(b.encoder.losses == 1U && !b.encoder.feedback.valid);
    CHECK(adc_begin(&b, 240U, false));
    CHECK(adc_complete(&b, 321U, true) == FOC_RUNTIME_ERR_ORDER);
    CHECK(b.adc.live.transaction.rejection == FOC_BOARD_ADC_LATE);
    CHECK(atomic_load(&b.runtime.stop_requests) & FOC_SAFE_FAULT_SAMPLE_TIMEOUT);
    CHECK(!b.adc.live.transaction.physical_output_applied);
    return 0;
}

static int test_encoder_gap(void)
{
    struct board_fixture b = {0};
    CHECK(prepare_snapshot_collision(&b) == 0);
    for (uint32_t t = 1000U; t <= 3000U; t += FOC_BOARD_H1_US) {
        CHECK(foc_board_encoder_begin(&b.encoder, t));
        CHECK(
            foc_board_encoder_complete(
                &b.runtime,
                &b.encoder,
                t + 70U,
                FOC_BOARD_ENCODER_TIMEOUT,
                0U
            ) == FOC_RUNTIME_ERR_INPUT
        );
        CHECK(!b.encoder.feedback.valid);
    }
    CHECK(b.encoder.losses == 5U);
    CHECK(foc_board_encoder_begin(&b.encoder, 3500U));
    CHECK(
        foc_board_encoder_complete(&b.runtime, &b.encoder, 3570U, FOC_BOARD_ENCODER_OK, 101U) ==
        FOC_RUNTIME_ERR_INPUT
    );
    CHECK(b.encoder.continuity_lost && b.encoder.losses == 6U);
    CHECK(foc_board_encoder_begin(&b.encoder, 4000U));
    CHECK(
        foc_board_encoder_complete(&b.runtime, &b.encoder, 4070U, FOC_BOARD_ENCODER_OK, 102U) ==
        FOC_RUNTIME_ERR_INPUT
    );
    CHECK(!b.encoder.feedback.valid && atomic_load(&b.runtime.stop_requests) != 0U);
    return 0;
}

static int test_output_interleavings(void)
{
    for (unsigned int scenario = 0U; scenario < 4U; ++scenario) {
        struct board_fixture b = {0};
        CHECK(board_init(&b, 0U) == 0);
#if defined(FALCONFOC_RT02_TIMING_TRACE)
        b.adc.trace_cycles = test_trace_clock;
        mock_trace_cycles = 0U;
#endif
        struct preemption event = {.board = &b, .point = FOC_RUNTIME_HOOK_BEFORE_SUBMIT};
        if (scenario == 1U) {
            b.runtime.config.hook = interrupt_at_hook;
            b.runtime.config.hook_arg = &event;
        }
        const struct foc_pipeline_command command = {{0.1f, 0.5f}, 0U, 0U};
        CHECK(foc_runtime_publish_command(&b.runtime, &command, 0U) == FOC_RUNTIME_OK);
        for (uint32_t i = 0U; i < 7U; ++i) {
            const uint32_t t = i * FOC_BOARD_H0_US + 80U;
            if (i == 0U || i == 3U || i == 5U) {
                const uint32_t captured = i == 0U ? 0U : (i == 3U ? 500U : 1000U);
                CHECK(foc_board_encoder_begin(&b.encoder, captured));
                CHECK(
                    foc_board_encoder_complete(
                        &b.runtime,
                        &b.encoder,
                        captured + 70U,
                        FOC_BOARD_ENCODER_OK,
                        100U
                    ) == (i == 0U ? FOC_RUNTIME_ERR_INPUT : FOC_RUNTIME_OK)
                );
            }
            if (i == 4U || i == 5U) {
                const struct foc_runtime_lifecycle_request request = {
                    .action = i == 4U ? FOC_RUNTIME_ACTION_ARM : FOC_RUNTIME_ACTION_START,
                    .source_session = b.runtime.motion_session,
                    .request_sequence = i,
                    .produced_us = t - 1U,
                };
                CHECK(
                    foc_runtime_publish_lifecycle_request(&b.runtime, &request, t - 1U) ==
                    FOC_RUNTIME_OK
                );
            }
            CHECK(adc_begin(&b, t, false));
            const uint32_t session = b.runtime.motion_session;
            if (i == 6U && scenario == 3U) {
                foc_runtime_request_inhibit(&b.runtime, FOC_SAFE_FAULT_WDT);
                const struct foc_board_adc_event late = foc_adc_test_event(&b.clock, t + 81U);
                const uint16_t invalid[] = {1241U, 4096U, 2048U, 2048U};
                CHECK(
                    foc_board_adc_complete(
                        &b.runtime,
                        &b.adc,
                        &b.output,
                        invalid,
                        4U,
                        &late,
                        true
                    ) == FOC_RUNTIME_ERR_ORDER
                );
                struct foc_runtime_snapshot snapshot;
                CHECK(foc_runtime_read_snapshot(&b.runtime, &snapshot) == FOC_RUNTIME_OK);
                const uint32_t faults = FOC_SAFE_FAULT_WDT | FOC_SAFE_FAULT_SAMPLE_INVALID |
                    FOC_SAFE_FAULT_SAMPLE_TIMEOUT;
                CHECK((snapshot.faults & faults) == faults && !snapshot.output_confirmed);
                CHECK(!b.adc.live.transaction.h0_called);
                CHECK(
                    adc_cycle(&b, t + FOC_BOARD_H0_US, t + FOC_BOARD_H0_US + 8U) ==
                    FOC_RUNTIME_ERR_TIMING
                );
                CHECK((b.runtime.pipeline.safety.latched_faults & faults) == faults);
                break;
            }
            if (i == 6U && scenario == 2U) {
                foc_runtime_revoke_command(&b.runtime);
            }
            const int status = adc_complete(&b, t + 8U, true);
            if (i == 4U || i == 5U) {
                CHECK(status == FOC_RUNTIME_IDLE);
            } else if (i == 6U) {
                CHECK(status == (scenario == 0U ? FOC_RUNTIME_ERR_CONTROL : FOC_RUNTIME_ERR_STOP));
                CHECK(
                    b.adc.live.transaction.slot_id == 6U && b.adc.live.transaction.generation == 6U
                );
                CHECK(b.adc.live.transaction.update_declined == (scenario == 0U));
#if defined(FALCONFOC_RT02_TIMING_TRACE)
                const struct foc_board_adc_trace *trace = &b.adc.live.transaction.trace;
                CHECK((trace->points & FOC_BOARD_TRACE_ADAPTER) != 0U);
                CHECK(
                    trace->adapter_accepted && trace->adapter_result == FOC_BOARD_INHIBIT_SOFTWARE
                );
                CHECK(((trace->points & FOC_BOARD_TRACE_UPDATE) != 0U) == (scenario == 0U));
                if (scenario == 0U) {
                    CHECK(
                        !trace->update_accepted &&
                        trace->update_result == FOC_BOARD_UPDATE_NOT_APPLIED
                    );
                    CHECK(trace->h0_start_cycles < trace->update_start_cycles);
                    CHECK(trace->update_start_cycles < trace->update_return_cycles);
                    CHECK(trace->update_return_cycles < trace->adapter_start_cycles);
                }
                CHECK(trace->adapter_return_cycles < trace->h0_finish_cycles);
#endif
                CHECK(
                    !b.adc.live.transaction.update_accepted &&
                    !b.adc.live.transaction.physical_output_applied
                );
                if (scenario == 2U) {
                    CHECK(
                        b.runtime.motion_session != session && !b.adc.live.transaction.h0_consumed
                    );
                }
            }
            struct foc_runtime_foreground_result fg;
            (void)foc_runtime_foreground_poll(&b.runtime, t + 9U, 0U, 0U, &fg);
        }
        CHECK(event.hits == (scenario == 1U) && b.output.declined_updates == (scenario == 0U));
        CHECK(
            b.output.result == FOC_BOARD_INHIBIT_SOFTWARE && b.runtime.pipeline.mock.updates == 0U
        );
        CHECK(b.runtime.pipeline.current.integral_v.d == 0.0f);
        CHECK(b.runtime.pipeline.current.integral_v.q == 0.0f);
        CHECK(
            b.runtime.pipeline.safety.state ==
            (scenario == 3U ? FOC_SAFE_STATE_FAULT_LATCHED : FOC_SAFE_STATE_DISABLED)
        );
    }
    puts(
        "RT-01 adapter refusal, guard STOP, pending STOP and late-frame/WDT preserve PI and output "
        "lock"
    );
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */
int main(void)
{
    return test_snapshot_collision() || test_fault_preempts_h1() || test_supervision_collision() ||
        test_encoder_gap() || test_fixed_priority_responses() || test_busy_foreground() ||
        test_delayed_and_over_budget() || test_legacy_edges() || test_load_contract() ||
        test_adc_events() || test_slots_and_clock() || test_drain_and_diagnostics() ||
        test_encoder_and_snapshot() || test_spi_close() || test_output_interleavings() ||
        test_trigger_candidates() || test_rank_apertures() || test_shadow_deadlines() ||
        test_optional_trace();
}

/* ---------------------------------------- end of file ----------------------------------------- */
