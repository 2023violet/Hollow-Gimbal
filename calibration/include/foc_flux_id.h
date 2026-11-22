/**
 * @file    foc_flux_id.h
 * @brief   Identify PMSM magnet flux from coherent signed terminal intervals.
 * @version v0.1.0
 * @date    2026.11.20
 *
 * Design overview
 * ---------------
 * Fit the window-averaged q-axis voltage equation using independently
 * qualified R/L, d/q current, applied terminal voltage and electrical speed.
 *
 * Usage example
 * -------------
 * Initialize one capture, push adjacent intervals, then finish after complete
 * windows in both directions. Review the result separately from Runtime admission.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * The caller owns and serializes the context in a cold calibration domain.
 * Evidence flags state what the acquisition owner witnessed.
 *
 * Failure / recovery
 * ------------------
 * An invalid interval or cancellation revokes the capture. Start another
 * capture with fresh identity and evidence. Results confer no output permission.
 */

#pragma once

/* Includes --------------------------------------------------------------------- */

#include "foc_motor_id.h"

/* Public Types ----------------------------------------------------------------- */

enum foc_flux_id_provenance {
    FOC_FLUX_ID_PROVENANCE_NONE,
    FOC_FLUX_FROM_MEASUREMENT,
    FOC_KT_DERIVED_FROM_FLUX,
    FOC_KT_FROM_INDEPENDENT_TORQUE,
};

/** IDs refer to the actual setup, measurement instruments and dependency revisions. */
struct foc_flux_id_identity {
    uint32_t session;
    uint32_t capture;
    uint32_t motor;
    uint32_t dq_frame;
    uint32_t current_chain;
    uint32_t voltage_chain;
    uint32_t speed_chain;
    uint32_t clock;
    uint32_t rl_revision;
    uint32_t poles_revision;
    uint32_t phase_order;
    uint16_t pole_pairs;
};

struct foc_flux_id_config {
    struct foc_flux_id_identity identity;
    struct foc_motor_rl_model rl;
    bool rl_qualified;
    enum foc_motor_id_source source;
    enum foc_motor_id_voltage_source voltage_source;
    uint32_t interval_us;
    uint32_t intervals_per_window;
    uint32_t min_windows_per_direction;
    double max_abs_current_a;
    double max_abs_vq_v;
    double max_abs_speed_rad_s;
    double min_abs_window_speed_rad_s;
    double min_speed_span_rad_s;
    double min_speed_rms_rad_s;
    double min_flux_wb;
    double max_flux_wb;
    double max_residual_rms_v;
    double max_direction_difference_relative;
};

/** Adjacent current apertures with applied Vq and speed over the same half-open interval. */
struct foc_flux_id_interval {
    struct foc_flux_id_identity identity;
    enum foc_motor_id_source source;
    enum foc_motor_id_voltage_source voltage_source;
    uint32_t sequence;
    uint32_t begin_us;
    uint32_t end_us;
    uint32_t voltage_begin_us;
    uint32_t voltage_end_us;
    uint32_t speed_begin_us;
    uint32_t speed_end_us;
    double id_begin_a;
    double iq_begin_a;
    double id_end_a;
    double iq_end_a;
    /** Signed mean electrical speed, in electrical rad/s. */
    double mean_omega_e_rad_s;
    /** Time-average applied q voltage in the same amplitude-invariant d/q frame. */
    double mean_vq_v;
    bool current_valid;
    bool voltage_valid;
    bool speed_valid;
    bool frame_confirmed;
    bool output_confirmed;
    /** Actual terminal observation or independently validated reconstructed terminal voltage. */
    bool terminal_voltage_witnessed;
    bool saturated;
};

struct foc_flux_id_direction {
    uint32_t windows;
    double flux_wb;
    double residual_rms_v;
    double minimum_abs_speed_rad_s;
    double maximum_abs_speed_rad_s;
};

struct foc_flux_id_result {
    struct foc_flux_id_identity identity;
    struct foc_motor_rl_model rl;
    enum foc_motor_id_source source;
    enum foc_motor_id_voltage_source voltage_source;
    enum foc_calibration_state state;
    enum foc_calibration_failure failure;
    enum foc_flux_id_provenance flux_provenance;
    enum foc_flux_id_provenance kt_provenance;
    uint32_t first_us;
    uint32_t last_us;
    uint32_t intervals;
    struct foc_flux_id_direction positive;
    struct foc_flux_id_direction negative;
    double residual_rms_v;
    double omega_e_rms_rad_s;
    double direction_difference_relative;
    float flux_wb;
    /** Volts of open-circuit phase-to-neutral peak per mechanical rad/s. */
    float ke_phase_peak_v_per_mechanical_rad_s;
    /** Magnet torque at Id=0 per peak q ampere, not an independent torque measurement. */
    float kt_nm_per_peak_q_amp;
};

/** Streaming storage for two signed fits, no sample buffer. */
struct foc_flux_id_fit {
    double speed2;
    double voltage2;
    double voltage_speed;
};

struct foc_flux_id {
    struct foc_flux_id_config config;
    struct foc_flux_id_result result;
    struct foc_flux_id_fit fits[2];
    double voltage_sum;
    double speed_sum;
    double window_iq_begin_a;
    double previous_id_a;
    double previous_iq_a;
    uint32_t last_sequence;
    uint32_t window_intervals;
    /** Set by foc_flux_id_break(): the next interval starts a new contiguous segment. */
    bool segment_break;
};

/* Public Functions ------------------------------------------------------------- */

enum foc_calibration_failure foc_flux_id_init(struct foc_flux_id *context,
                                              const struct foc_flux_id_config *config);
enum foc_calibration_failure foc_flux_id_push(struct foc_flux_id *context,
                                              const struct foc_flux_id_interval *sample);
enum foc_calibration_failure foc_flux_id_finish(struct foc_flux_id *context,
                                                struct foc_flux_id_result *result);
void foc_flux_id_cancel(struct foc_flux_id *context);
/**
 * End the current contiguous segment. The open partial window is discarded and
 * the next interval may begin at any later time and sequence, for example after
 * a physical reversal or speed change settles. Completed windows are kept.
 */
void foc_flux_id_break(struct foc_flux_id *context);

/* ---------------------------------------- end of file ----------------------------------------- */

