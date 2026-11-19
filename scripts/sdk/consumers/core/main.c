/**
 * @file main.c
 * @brief Compile and run Core as a standalone C11 dependency.
 */

#include "foc_impedance.h"

#include <math.h>
#include <stdio.h>

int main(void)
{
    const struct foc_impedance_config config = {
        .stiffness_nm_per_rad = 0.6f,
        .damping_nm_s_per_rad = 0.04f,
        .torque_constant_nm_per_a = 0.15f,
        .torque_limit_nm = 0.3f,
    };
    const struct foc_impedance_input input = {
        .position_error_rad = 0.10f,
        .live_iq_limit_a = 2.0f,
    };
    struct foc_impedance_output result;
    if (foc_impedance_step(&config, &input, &result) != 0 || !result.valid || result.limited ||
        fabsf(result.iq_ref_a - 0.4f) > 1e-6f) {
        fputs("Core IMPEDANCE smoke failed\n", stderr);
        return 1;
    }
    printf("Core-only IMPEDANCE PASS iq_ref_A=%.6f\n", (double)result.iq_ref_a);
    return 0;
}
