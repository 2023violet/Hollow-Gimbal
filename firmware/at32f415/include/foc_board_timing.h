/**
 * @file    foc_board_timing.h
 * @brief   Board1 ADC route, aperture and future PWM load contracts.
 * @version v0.1.0
 * @date    2026.11.19
 *
 * Design overview
 * ---------------
 * Describe Board1's existing scan and validate a future carrier/load timeline.
 *
 * Usage example
 * -------------
 * Compute rank apertures from effective SOC, check each phase window, then
 * stage a candidate from the completed token and report a qualified load event.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Host model or serialized board owner; zero-initialize each load instance.
 * Contract software does not write ADC, timer or power-stage registers.
 *
 * Failure / recovery
 * ------------------
 * Invalid staging or a wrong load event discards the pending candidate.
 */
#pragma once

#include "foc_board.h"

/* Public Types ---------------------------------------------------------------- */

/** CTRL2 selectors used by the AT32F415 SDK; TRGOUT has no compare-qualified plan here. */
enum foc_board_adc_route {
    FOC_BOARD_ADC_TMR1_TRGOUT = 0x00,
    FOC_BOARD_ADC_TMR1_CH4 = 0x01,
    FOC_BOARD_ADC_REMAPPED_CH4 = 0x06,
    FOC_BOARD_ADC_SOFTWARE = 0x07,
};

struct foc_board_adc_route_config {
    uint32_t ctrl2_mask;
    uint32_t ctrl2;
    uint32_t iomux_remap7_mask;
    uint32_t iomux_remap7;
};

struct foc_board_aperture {
    uint32_t start_ns;
    uint32_t end_ns;
};

enum foc_board_rank {
    FOC_BOARD_RANK_VBUS,
    FOC_BOARD_RANK_IC,
    FOC_BOARD_RANK_IB,
    FOC_BOARD_RANK_IA,
    FOC_BOARD_RANK_COUNT
};

struct foc_board_adc_scan {
    struct foc_board_aperture rank[FOC_BOARD_RANK_COUNT];
    uint32_t complete_ns;
};

/** Phase is chronological within the carrier. In centered mode the peak
 * at period/2 separates up- and down-count; phase zero denotes the valley. */
struct foc_board_carrier_stamp {
    uint32_t cycle;
    uint32_t phase_ns;
};

struct foc_board_load_plan {
    uint32_t period_ns;
    uint32_t setup_ns;
    struct foc_board_carrier_stamp load;
};

/** Four times come from one qualified cycle timeline. The three apertures
 * and VBUS validity must be checked by the caller before asserting window_valid. */
struct foc_board_shadow_attempt {
    uint32_t sample_cycle;
    uint32_t generation;
    struct foc_board_carrier_stamp eoc;
    struct foc_board_carrier_stamp h0_start;
    struct foc_board_carrier_stamp h0_finish;
    struct foc_board_carrier_stamp shadow_write;
    bool window_valid;
};

struct foc_board_shadow {
    struct foc_board_carrier_stamp load;
    struct foc_board_carrier_stamp applied;
    uint32_t last_sample_cycle;
    uint32_t last_generation;
    bool sample_seen;
    bool pending;
    bool applied_seen;
};

enum foc_board_shadow_result {
    FOC_BOARD_SHADOW_INVALID,
    FOC_BOARD_SHADOW_DEADLINE,
    FOC_BOARD_SHADOW_STAGED,
    FOC_BOARD_SHADOW_MODEL_APPLIED,
};

enum foc_board_spi_edge {
    FOC_BOARD_SPI_CS,
    FOC_BOARD_SPI_SEND,
    FOC_BOARD_SPI_CLOSE,
};

enum {
    FOC_BOARD_SPI_SEND_US = 20U,
    FOC_BOARD_SPI_CLOSE_US = 70U,
};

/* Public Functions ------------------------------------------------------------- */

/** Check the service phase and the maximum elapsed time from its TMR2
 * bracket. Use a post-action observation to reject delayed side effects. */
bool foc_board_spi_on_time(enum foc_board_spi_edge edge,
                           uint32_t phase_us,
                           uint32_t before_phase_us,
                           uint32_t observed_us);

/** Require a serviced RX, idle bus, no sticky errors and at least two
 * ticks of hold at the pre-release check. */
enum foc_board_encoder_result foc_board_spi_close_result(uint32_t phase_us,
                                                         uint32_t before_phase_us,
                                                         uint32_t checked_us,
                                                         uint32_t rx_us,
                                                         bool received,
                                                         bool busy,
                                                         bool error);

/** Encode a fixed route without touching peripheral registers. */
bool foc_board_adc_route_config(enum foc_board_adc_route route,
                                struct foc_board_adc_route_config *config);

/** Model only: require the independently attributed CH4 event at the
 * programmed up-count phase. PCCE alone cannot supply this witness. */
bool foc_board_adc_candidate_begin(struct foc_runtime *runtime,
                                   struct foc_board_adc *adc,
                                   const struct foc_board_adc_event *compare,
                                   bool unique_compare_witness,
                                   bool pcce_pending);

/** VBUS/IC/IB/IA at nominal 12.5 MHz, from effective SOC after synchronization. */
bool foc_board_adc_scan_at(uint32_t effective_soc_ns, struct foc_board_adc_scan *scan);

/** Ideal centered PWM with high-side on near zero. Pass the up-count
 * switching phase and effective post-/pre-edge guards for this leg. */
bool foc_board_low_side_window(uint32_t period_ns,
                               uint32_t compare_ns,
                               uint32_t after_edge_ns,
                               uint32_t before_edge_ns,
                               struct foc_board_aperture *window);

/** Match the IC, IB and IA apertures to their corresponding leg windows. */
bool foc_board_adc_current_window(const struct foc_board_adc_scan *scan,
                                  const struct foc_board_aperture windows[3]);

/** Validate the ordered H0-to-load timeline at the observed final shadow write. */
enum foc_board_shadow_result foc_board_shadow_check_timing(
    const struct foc_board_load_plan *plan,
    const struct foc_board_shadow_attempt *attempt);

/** Stage once per sampled carrier, after complete H0 and adapter acceptance. */
enum foc_board_shadow_result foc_board_shadow_stage(struct foc_board_shadow *shadow,
                                                    const struct foc_board_load_plan *plan,
                                                    const struct foc_board_adc_transaction *token,
                                                    const struct foc_board_shadow_attempt *attempt);

/** Retire the candidate at the exact qualified load, after rechecking output
 * permission (including intervening STOP or FAULT). */
enum foc_board_shadow_result foc_board_shadow_load(struct foc_board_shadow *shadow,
                                                   struct foc_board_carrier_stamp observed,
                                                   bool witness_valid,
                                                   bool output_allowed);

/* ---------------------------------------- end of file ----------------------------------------- */
