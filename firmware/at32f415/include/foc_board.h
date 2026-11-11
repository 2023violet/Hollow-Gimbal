/**
 * @file    foc_board.h
 * @brief   Host-testable Board1 configuration and locked V2 port.
 * @version v0.1.0
 * @date    2026.11.10
 *
 * Design overview
 * ---------------
 * Translate Board1 samples and output requests at the existing Runtime boundary.
 *
 * Usage example
 * -------------
 * Initialize a static runtime with foc_board_init before starting its producers.
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * Initialize once before IRQs. ADC and encoder producers own separate state.
 *
 * Failure / recovery
 * ------------------
 * Initialization errors leave the caller without permission to start producers.
 */
#pragma once

#include "foc_runtime.h"

/* Public Types ---------------------------------------------------------------- */

/** Board schedule and range constants shared with the IRQ binding. */
enum foc_board_limits {
    /** 150 MHz timer input after the APB timer multiplier. */
    FOC_BOARD_TIMER_HZ = 150000000,
    /** Counter ticks per second on TMR2 and TMR5. */
    FOC_BOARD_US_HZ = 1000000,
    /** Nominal TMR1 event interval in microseconds. */
    FOC_BOARD_H0_US = 200,
    /** Nominal TMR5 event interval in microseconds. */
    FOC_BOARD_H1_US = 500,
    /** TMR1 period and maximum diagnostic compare. */
    FOC_BOARD_COMPARE_MAX = FOC_BOARD_TIMER_HZ / FOC_BOARD_US_HZ * FOC_BOARD_H0_US - 1,
};

/** Last disposition of a complete output request, with software scope only. */
enum foc_board_output_result {
    /** INHIBIT was accepted as software policy for this locked image. */
    FOC_BOARD_INHIBIT_SOFTWARE,
    /** An eligible UPDATE was checked but never applied. */
    FOC_BOARD_UPDATE_NOT_APPLIED,
    /** ZERO_DUTY cannot be executed by the locked image. */
    FOC_BOARD_ZERO_DUTY_LOCKED,
    /** ACTIVE_BRAKE cannot be executed by the locked image. */
    FOC_BOARD_ACTIVE_BRAKE_LOCKED,
    /** The action or its payload failed validation. */
    FOC_BOARD_OUTPUT_INVALID,
};

/** H0-owned attempt journal, separate from the Pipeline result. */
struct foc_board_output {
    /** Last complete action received. */
    struct foc_device_pwm_request request;
    /** Disposition of request. Never establishes physical confirmation. */
    enum foc_board_output_result result;
    /** Number of valid UPDATEs declined by the board. */
    uint32_t declined_updates;
};

/** H0-owned acquisition token, shared by two non-preempting IRQ handlers. */
struct foc_board_adc {
    /** Software trigger time in the common microsecond epoch. */
    uint32_t captured_us;
    /** Sequence issued for the next acquisition. */
    uint32_t seq;
    /** True while awaiting one complete injected frame. */
    bool pending;
    /** A timed-out conversion must be drained before a new trigger. */
    bool expired;
};

/** Why the latest encoder transaction did not publish usable feedback. */
enum foc_board_encoder_result {
    /** Complete and timely 16-bit response. */
    FOC_BOARD_ENCODER_OK,
    /** No response by the bounded window. */
    FOC_BOARD_ENCODER_TIMEOUT,
    /** Peripheral reported an error or frame count was wrong. */
    FOC_BOARD_ENCODER_FRAME,
    /** Previous transaction still occupied the window. */
    FOC_BOARD_ENCODER_OVERLAP,
    /** Time or angle displacement was outside the permitted range. */
    FOC_BOARD_ENCODER_DISCONTINUITY,
};

/** H1-owned transaction and continuous mechanical position state. */
struct foc_board_encoder {
    /** Time of current frame start in microseconds. */
    uint32_t captured_us;
    /** Time of last accepted mechanical observation. */
    uint32_t previous_us;
    /** Sequence of completed or failed observations. */
    uint32_t seq;
    /** Consecutive unsuccessful transactions, saturating. */
    uint32_t losses;
    /** Last accepted raw 16-bit angle. */
    uint16_t previous_code;
    /** Continuous mechanical angle in radians relative to the sensor's raw zero. */
    float position_rad;
    /** Latest transaction disposition. */
    enum foc_board_encoder_result result;
    /** True between transport start and its single completion. */
    bool pending;
    /** True once there is a mechanical reference for velocity estimation. */
    bool previous_seen;
    /** True after a gap makes the continuous position ambiguous for this session. */
    bool continuity_lost;
};

/* Public Functions ------------------------------------------------------------- */

/** Initialize the reference control instance at the common microsecond epoch. */
int foc_board_init(struct foc_runtime *runtime, struct foc_board_output *output, uint32_t now_us);

/** Convert an inclusive half-range-valid interval to seconds. */
bool foc_board_elapsed_s(uint32_t now_us, uint32_t previous_us, uint32_t limit_us, float *seconds);

/** Reserve one conversion token. A busy token requests stop and is never replaced. */
bool foc_board_adc_begin(struct foc_runtime *runtime, struct foc_board_adc *adc, uint32_t now_us);

/** Consume one group completion. Exactly four 12-bit ranks can enter H0. */
int foc_board_adc_complete(struct foc_runtime *runtime,
                           struct foc_board_adc *adc,
                           const uint16_t *ranks,
                           unsigned int count,
                           uint32_t now_us);

/** Start one encoder transaction. Overlap invalidates the outstanding response. */
bool foc_board_encoder_begin(struct foc_board_encoder *encoder, uint32_t now_us);

/** Complete or fail one transaction and publish its quality to H1. */
int foc_board_encoder_complete(struct foc_runtime *runtime,
                               struct foc_board_encoder *encoder,
                               uint32_t now_us,
                               enum foc_board_encoder_result transport,
                               uint16_t code);

/** Validate a complete request and record its locked disposition. */
bool foc_board_output_submit(void *argument, const struct foc_device_pwm_request *request);

/* ---------------------------------------- end of file ----------------------------------------- */
