/**
 * @file    test_foc_baseline.c
 * @brief   PERF-01A deterministic current-loop witness and separate Host benchmark.
 * @version v0.1.0
 * @date    2026.11.12
 *
 * Test role
 * ---------
 * Golden/benchmark builds link unchanged production translation units. The
 * PERF_COUNTS build alone includes them with test-local math wrappers and
 * function-entry instrumentation. Its time must never be used as a benchmark.
 * Device compares are software mappings; no hardware or Runtime is invoked.
 */

/* Includes --------------------------------------------------------------------- */
#include "foc_current.h"
#include "foc_device.h"
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

/* Private Types ---------------------------------------------------------------- */
enum call_index {
    WRAP,
    SIN,
    COS,
    ATAN,
    SQRT,
    HYPOT,
    CLARKE,
    PARK,
    INVERSE,
    SVPWM,
    CONFIG,
    MODULATE,
    LIMIT,
    ADMIT,
    CURRENT,
    CALL_COUNT
};

/* Private State ---------------------------------------------------------------- */
static uint32_t calls[CALL_COUNT];
static volatile float sink;

#ifdef PERF_COUNTS
/* Instrumented source copy only. Includes are guarded before macro substitution. */
static float count_fmod(float x, float y)
{
    ++calls[WRAP];
    return fmodf(x, y);
}
static float count_sin(float x)
{
    ++calls[SIN];
    return sinf(x);
}
static float count_cos(float x)
{
    ++calls[COS];
    return cosf(x);
}
static float count_atan(float y, float x)
{
    ++calls[ATAN];
    return atan2f(y, x);
}
static float count_sqrt(float x)
{
    ++calls[SQRT];
    return sqrtf(x);
}
static float count_hypot(float x, float y)
{
    ++calls[HYPOT];
    return hypotf(x, y);
}
#define fmodf  count_fmod
#define sinf   count_sin
#define cosf   count_cos
#define atan2f count_atan
#define sqrtf  count_sqrt
#define hypotf count_hypot
#include "../src/foc_core.c"
#include "../src/foc_current.c"
#undef fmodf
#undef sinf
#undef cosf
#undef atan2f
#undef sqrtf
#undef hypotf

/* Hooks must never instrument themselves. This build disables inlining. */
void __attribute__((no_instrument_function)) __cyg_profile_func_enter(void *fn, void *caller)
{
    (void)caller;
#define COUNT_FN(symbol, index)             \
    if (fn == (void *)(uintptr_t)&symbol) { \
        ++calls[index];                     \
    }
    COUNT_FN(foc_clarke, CLARKE)
    COUNT_FN(foc_park, PARK)
    COUNT_FN(foc_inverse_park, INVERSE)
    COUNT_FN(foc_svpwm, SVPWM)
    COUNT_FN(foc_config_validate, CONFIG)
    COUNT_FN(foc_current_modulate, MODULATE)
    COUNT_FN(foc_current_limit_vector, LIMIT)
    COUNT_FN(foc_current_admit_integral, ADMIT)
    COUNT_FN(foc_current_step, CURRENT)
#undef COUNT_FN
}
void __attribute__((no_instrument_function)) __cyg_profile_func_exit(void *fn, void *caller)
{
    (void)fn;
    (void)caller;
}
#endif

/* Private Helpers -------------------------------------------------------------- */
static struct foc_current_config settings(void)
{
    return (
        struct foc_current_config
    ){{1.0f, 2.5f, 0.28f, 0.72f, 0.001f}, {1.08f, 1.35f}, {540.0f, 540.0f}, 2.5f};
}

static struct foc_current_input sample(float angle, float d, float q)
{
    return (struct foc_current_input){{0.02f, -0.01f, -0.01f}, angle, {d, q}, 12.0f, 0.0001f};
}

static int record(
    const char *name,
    unsigned int step,
    struct foc_current_context *ctx,
    const struct foc_current_input *input,
    int expected,
    int count_mode
)
{
    const struct foc_current_context before = *ctx;
    struct foc_current_output out;
    memset(&out, 0xa5, sizeof(out));
    memset(calls, 0, sizeof(calls));
    const int status = foc_current_step(ctx, input, &out);
    uint32_t snapshot[CALL_COUNT];
    memcpy(snapshot, calls, sizeof(snapshot));
    if (status != expected) {
        fprintf(stderr, "%s[%u] status=%d expected=%d\n", name, step, status, expected);
        return 1;
    }
    if (status != FOC_STATUS_OK) {
        const struct foc_current_output zero = {0};
        if (memcmp(&zero, &out, sizeof(out)) != 0 || memcmp(&before, ctx, sizeof(before)) != 0) {
            fprintf(stderr, "%s[%u] failure atomicity violated\n", name, step);
            return 1;
        }
    }
    if (count_mode) {
#ifdef PERF_COUNTS
        const bool corrected = strcmp(name, "low_vbus") == 0 || strcmp(name, "vector_limit") == 0 ||
            strcmp(name, "duty_limit") == 0;
        const bool no_correction =
            strcmp(name, "normal") == 0 || strcmp(name, "saturated_no_correction") == 0;

        if ((no_correction || corrected) &&
            (snapshot[WRAP] != 1U || snapshot[SIN] != 1U || snapshot[COS] != 1U ||
             snapshot[ATAN] != 0U || snapshot[CONFIG] != 0U)) {
            fprintf(
                stderr,
                "%s[%u] optimized call contract not met: fmod=%u sin=%u cos=%u atan=%u config=%u\n",
                name,
                step,
                snapshot[WRAP],
                snapshot[SIN],
                snapshot[COS],
                snapshot[ATAN],
                snapshot[CONFIG]
            );
            return 1;
        }
#endif
        printf("%s,%u,%d,%u", name, step, status, out.svpwm.saturated);
        for (unsigned int i = 0; i < CALL_COUNT; ++i) {
            printf(",%u", snapshot[i]);
        }
        putchar('\n');
        return 0;
    }
    const struct foc_output core = {.output_enable = out.output_enable, .svpwm = out.svpwm};
    const struct foc_device_pwm_config pwm = {29999U, FOC_DEVICE_PHASE_ORDER_ABC};
    struct foc_device_pwm_request request;
    const int mapped = foc_device_map_output(&core, &pwm, &request);
    if (mapped != FOC_DEVICE_STATUS_OK) {
        return 1;
    }
    const struct foc_current_config *cfg = &ctx->config;
    printf(
        "%s,%u,%d,%u,%u,%u,%u,%u,%u,%u,%u,%u",
        name,
        step,
        status,
        out.output_enable,
        out.target_limited,
        out.svpwm.sector,
        out.svpwm.saturated,
        request.action,
        request.channel1_compare,
        request.channel2_compare,
        request.channel3_compare,
        before.initialized
    );
    const float fields[] = {
        cfg->pwm.vbus_min_v,
        cfg->pwm.voltage_limit_v,
        cfg->pwm.duty_min,
        cfg->pwm.duty_max,
        cfg->pwm.max_dt_s,
        cfg->kp_v_per_a.d,
        cfg->kp_v_per_a.q,
        cfg->ki_v_per_a_s.d,
        cfg->ki_v_per_a_s.q,
        cfg->current_limit_a,
        input->phase_current_a.a,
        input->phase_current_a.b,
        input->phase_current_a.c,
        input->theta_elec_rad,
        input->target_a.d,
        input->target_a.q,
        input->vbus_v,
        input->dt_s,
        before.integral_v.d,
        before.integral_v.q,
        out.measured_dq_a.d,
        out.measured_dq_a.q,
        out.voltage_dq_v.d,
        out.voltage_dq_v.q,
        out.svpwm.duty_a,
        out.svpwm.duty_b,
        out.svpwm.duty_c,
        ctx->integral_v.d,
        ctx->integral_v.q
    };
    for (unsigned int i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
        printf(",%.9g", fields[i]);
    }
    putchar('\n');
    return 0;
}

static int dataset(int count_mode)
{
    puts(
        count_mode ? "case,step,status,saturated,fmodf,sinf,cosf,atan2f,sqrtf,hypotf,clarke,park,"
                     "inverse,svpwm,config,modulate,limit,admit,current"
                   : "case,step,status,enable,target_limited,sector,saturated,action,compare1,"
                     "compare2,compare3,initialized,vbus_min,voltage_limit,duty_min,duty_max,max_"
                     "dt,kp_d,kp_q,ki_d,ki_q,current_limit,ia,ib,ic,theta,target_d,target_q,vbus,"
                     "dt,integral_before_d,integral_before_q,measured_d,measured_q,voltage_d,"
                     "voltage_q,duty_a,duty_b,duty_c,integral_d,integral_q"
    );
    struct foc_current_config cfg = settings();
    struct foc_current_context ctx = {0};
    const float tau = 6.28318530717958647692f;
    /* Independent states across a full revolution; all four dq sign quadrants. */
    for (unsigned int i = 0; i < 1024; ++i) {
        const float angle = tau * (float)i / 1024.0f;
        const struct foc_current_input in =
            sample(angle, (i & 1) ? -0.15f : 0.15f, (i & 2) ? -0.2f : 0.2f);
        if (foc_current_init(&ctx, &cfg) != 0 ||
            record("angle_sweep", i, &ctx, &in, 0, count_mode)) {
            return 1;
        }
    }
    for (unsigned int i = 0; i <= 4; ++i) {
        for (int side = -1; side <= 1; ++side) {
            const float boundary = tau * (float)i / 4.0f;
            const float angle = side < 0 ? nextafterf(boundary, -INFINITY)
                : side > 0               ? nextafterf(boundary, INFINITY)
                                         : boundary;
            const struct foc_current_input in = sample(angle, -0.1f, 0.3f);
            if (foc_current_init(&ctx, &cfg) != 0 ||
                record(
                    "angle_boundary",
                    i * 3U + (unsigned int)(side + 1),
                    &ctx,
                    &in,
                    0,
                    count_mode
                )) {
                return 1;
            }
        }
    }
    const char *names[] =
        {"normal", "low_vbus", "vector_limit", "saturated_no_correction", "duty_limit"};
    for (unsigned int test = 0; test < 5; ++test) {
        cfg = settings();
        struct foc_current_input in = sample(0.73f, 0.0f, 0.2f);
        if (test == 1) {
            in.vbus_v = 1.2f;
            in.target_a.q = 2.0f;
        }
        if (test == 2) {
            in.target_a = (struct foc_vector_dq){2, 2};
        }
        if (test == 3) {
            cfg.ki_v_per_a_s = (struct foc_vector_dq){0, 0};
            in.target_a.q = 2.5f;
        }
        if (test == 4) {
            cfg.pwm.duty_min = 0.49f;
            cfg.pwm.duty_max = 0.51f;
        }
        if (foc_current_init(&ctx, &cfg) != 0 || record(names[test], 0, &ctx, &in, 0, count_mode)) {
            return 1;
        }
    }
    cfg = settings();
    if (foc_current_init(&ctx, &cfg) != 0) {
        return 1;
    }
    for (unsigned int i = 0; i < 3072; ++i) {
        const char *name = i < 1024 ? "long_saturation" : i < 2048 ? "target_reverse" : "release";
        struct foc_current_input in = sample(0.4f, 0, i < 1024 ? 2.5f : i < 2048 ? -2.5f : 0);
        /* Simulated measured current changes during the release segment. */
        if (i >= 2048) {
            in.phase_current_a = (struct foc_vector_abc){0, 0, 0};
        }
        if (record(name, i, &ctx, &in, 0, count_mode)) {
            return 1;
        }
    }
    for (unsigned int i = 0; i < 10; ++i) {
        struct foc_current_input in = sample(0.4f, 0, 0.2f);
        int expected = FOC_STATUS_ERR_INVALID_DT;
        if (i == 0) {
            in.dt_s = 0;
        }
        if (i == 1) {
            in.dt_s = -0.001f;
        }
        if (i == 2) {
            in.dt_s = NAN;
        }
        if (i == 3) {
            in.dt_s = 0.002f;
        }
        if (i == 4) {
            in.vbus_v = 0;
            expected = FOC_STATUS_ERR_INVALID_VBUS;
        }
        if (i == 5) {
            in.vbus_v = INFINITY;
            expected = FOC_STATUS_ERR_INVALID_VBUS;
        }
        if (i == 6) {
            in.target_a.d = NAN;
            expected = FOC_STATUS_ERR_NONFINITE;
        }
        if (i == 7) {
            in.theta_elec_rad = INFINITY;
            expected = FOC_STATUS_ERR_NONFINITE;
        }
        if (i == 8) {
            in.phase_current_a.b = NAN;
            expected = FOC_STATUS_ERR_NONFINITE;
        }
        if (i == 9) {
            ctx.initialized = 0;
            expected = FOC_STATUS_ERR_NOT_INITIALIZED;
        }
        if (record("invalid", i, &ctx, &in, expected, count_mode)) {
            return 1;
        }
    }
    return 0;
}

static double now_seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER count, frequency;
    QueryPerformanceCounter(&count);
    QueryPerformanceFrequency(&frequency);
    return (double)count.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec time;
    timespec_get(&time, TIME_UTC);
    return (double)time.tv_sec + (double)time.tv_nsec * 1e-9;
#endif
}

static int benchmark(void)
{
    /* Same 256 fixed independent inputs and initial states in every repetition. */
    struct foc_current_input inputs[256];
    const unsigned int repetitions = 200000;
    const char *names[] = {"normal", "saturated_correction"};
    puts("path,batch,iterations,ns_per_step,checksum");
    for (unsigned int path = 0; path < 2; ++path) {
        const struct foc_current_config cfg = settings();
        struct foc_current_context initial;
        if (foc_current_init(&initial, &cfg) != 0) {
            return 1;
        }
        for (unsigned int i = 0; i < 256; ++i) {
            inputs[i] =
                sample(6.28318530718f * (float)i / 256.0f, path ? 0 : 0.1f, path ? 2.5f : 0.2f);
        }
        for (unsigned int batch = 0; batch < 16; ++batch) {
            float checksum = 0;
            const double start = now_seconds();
            for (unsigned int i = 0; i < repetitions; ++i) {
                struct foc_current_context ctx = initial;
                struct foc_current_output out;
                if (foc_current_step(&ctx, &inputs[i & 255U], &out) != 0) {
                    return 1;
                }
                checksum += out.svpwm.duty_a + out.voltage_dq_v.q + ctx.integral_v.q;
            }
            const double elapsed = now_seconds() - start;
            sink = checksum;
            /* Batch zero is warm-up and intentionally omitted. */
            if (batch > 0) {
                printf(
                    "%s,%u,%u,%.6f,%.9g\n",
                    names[path],
                    batch,
                    repetitions,
                    elapsed * 1e9 / repetitions,
                    sink
                );
            }
        }
    }
    return 0;
}

/* Public Functions ------------------------------------------------------------- */
int main(int argc, char **argv)
{
    if (argc != 2) {
        return 2;
    }
    if (strcmp(argv[1], "--golden") == 0) {
        return dataset(0);
    }
#ifdef PERF_COUNTS
    if (strcmp(argv[1], "--counts") == 0) {
        return dataset(1);
    }
#else
    if (strcmp(argv[1], "--bench") == 0) {
        return benchmark();
    }
#endif
    return 2;
}

/* ---------------------------------------- end of file ----------------------------------------- */
