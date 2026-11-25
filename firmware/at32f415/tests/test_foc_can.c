/**
 * @file    test_foc_can.c
 * @brief   Exercise the CANSimple adapter against the real serial admission path.
 * @version v0.1.0
 * @date    2026.11.24
 *
 * Test role
 * ---------
 * Push frames as the CAN IRQ does, poll Foreground and inspect the frames handed to a
 * capturing port: IDs, payload layouts, cyclic timing, refusals and counters.
 */

/* Includes --------------------------------------------------------------------- */

#include "foc_board.h"
#include "foc_can.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Private Macros --------------------------------------------------------------- */

#define CHECK(x)                                            \
    do {                                                    \
        if (!(x)) {                                         \
            fprintf(stderr, "line %d: %s\n", __LINE__, #x); \
            return 1;                                       \
        }                                                   \
    } while (0)

#define SERIAL_NUMBER UINT64_C(0x0000A1B2C3D4E5F6)

/* Private Types ---------------------------------------------------------------- */

struct fixture {
    struct foc_runtime runtime;
    struct foc_board_output output;
    struct foc_serial serial;
    struct foc_can_buffer buffer;
    struct foc_can can;
    struct foc_can_frame sent[64];
    unsigned int count;
    bool accept;
    uint32_t time;
};

/* Private Helpers -------------------------------------------------------------- */

static bool capture(void *context, const struct foc_can_frame *frame)
{
    struct fixture *test = context;
    if (!test->accept || test->count == sizeof test->sent / sizeof test->sent[0]) {
        return false;
    }
    test->sent[test->count++] = *frame;
    return true;
}

static uint32_t link_state(const void *context)
{
    (void)context;
    return FOC_CAN_STATE_ACTIVE;
}

static int init(struct fixture *test)
{
    memset(test, 0, sizeof *test);
    CHECK(foc_board_init(&test->runtime, &test->output, 0) == FOC_RUNTIME_OK);
    foc_serial_init(&test->serial, &test->runtime);
    const struct foc_can_port port = {
        .context = test,
        .send = capture,
        .state = link_state,
        .serial_number = SERIAL_NUMBER,
    };
    foc_can_init(&test->can, &test->buffer, &port);
    const struct foc_serial_can_link link = {&test->can, foc_can_link_status};
    foc_serial_attach_can_link(&test->serial, &link);
    test->accept = true;
    return 0;
}

static void poll(struct fixture *test)
{
    foc_can_poll(&test->can, &test->serial, &test->runtime, test->time);
}

static void push(
    struct fixture *test,
    uint8_t node,
    uint8_t command,
    bool rtr,
    const uint8_t *data,
    uint8_t length
)
{
    struct foc_can_frame frame = {
        .id = (uint16_t)(node << 5 | command),
        .length = length,
        .rtr = rtr,
    };
    if (length != 0U) {
        memcpy(frame.data, data, length);
    }
    foc_can_rx_push(&test->buffer, &frame);
}

static float get_f32(const uint8_t *data)
{
    uint32_t bits = (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 |
        (uint32_t)data[3] << 24;
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

static uint32_t get_u32(const uint8_t *data)
{
    return (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 |
        (uint32_t)data[3] << 24;
}

static unsigned int count_id(const struct fixture *test, uint16_t id)
{
    unsigned int found = 0U;
    for (unsigned int index = 0U; index < test->count; ++index) {
        found += test->sent[index].id == id;
    }
    return found;
}

/* Test Cases ------------------------------------------------------------------- */

static int test_address_and_ids(void)
{
    static struct fixture test;
    CHECK(init(&test) == 0);
    /* Discovery: an RTR Address to the broadcast node gets the serial number back. */
    push(&test, 0x3F, FOC_CAN_ADDRESS, true, NULL, 0U);
    poll(&test);
    CHECK(test.count == 1U && test.sent[0].id == 0x7E6U && test.sent[0].length == 8U);
    CHECK(
        test.sent[0].data[0] == 0x3FU && test.sent[0].data[1] == 0xF6U &&
        test.sent[0].data[6] == 0xA1U && test.sent[0].data[7] == 0U
    );
    /* An unaddressed device sends no cyclic frames. */
    test.count = 0U;
    test.time = 500000U;
    poll(&test);
    CHECK(test.count == 0U);
    /* Node 4: Get_Bus_Voltage_Current is ID 0x97 (4 << 5 | 0x17), NaN without a readback. */
    test.serial.can.node_id = 4U;
    push(&test, 4U, FOC_CAN_GET_BUS_VOLTAGE_CURRENT, true, NULL, 0U);
    push(&test, 5U, FOC_CAN_GET_VERSION, true, NULL, 0U); /* another node: ignored */
    test.serial.can.heartbeat_ms = 0U;
    test.serial.can.estimates_ms = 0U;
    poll(&test);
    CHECK(test.count == 1U && test.sent[0].id == 0x97U && isnan(get_f32(test.sent[0].data)));
    /* Get_Version over an empty data frame. */
    test.count = 0U;
    push(&test, 4U, FOC_CAN_GET_VERSION, false, NULL, 0U);
    poll(&test);
    CHECK(test.count == 1U && test.sent[0].id == 0x80U && test.sent[0].data[0] == 2U);
    /* Broadcast Get_ requests are not answered by an addressed node. */
    test.count = 0U;
    push(&test, 0x3F, FOC_CAN_GET_VERSION, true, NULL, 0U);
    poll(&test);
    CHECK(test.count == 0U);
    return 0;
}

static int test_cyclic(void)
{
    static struct fixture test;
    CHECK(init(&test) == 0);
    test.serial.can.node_id = 2U;
    test.time = 1000000U;
    poll(&test);
    CHECK(count_id(&test, 0x41U) == 1U && count_id(&test, 0x49U) == 1U);
    for (unsigned int step = 1U; step <= 100U; ++step) {
        test.time = 1000000U + step * 1000U;
        poll(&test);
    }
    /* 100 ms: one more heartbeat (7 bytes) and ten more estimates. */
    CHECK(count_id(&test, 0x41U) == 2U && count_id(&test, 0x49U) == 11U);
    for (unsigned int index = 0U; index < test.count; ++index) {
        if (test.sent[index].id == 0x41U) {
            CHECK(test.sent[index].length == 7U && test.sent[index].data[6] == 1U);
        }
    }
    return 0;
}

static int test_refusals_and_counters(void)
{
    static struct fixture test;
    CHECK(init(&test) == 0);
    test.serial.can.node_id = 1U;
    test.serial.can.heartbeat_ms = 0U;
    test.serial.can.estimates_ms = 0U;
    /* The locked target refuses closed loop: procedure INVALID_STATE, one refusal. */
    const uint8_t closed_loop[4] = {FOC_CAN_AXIS_CLOSED_LOOP};
    push(&test, 1U, FOC_CAN_SET_AXIS_STATE, false, closed_loop, 4U);
    /* Wrong length and feedforward are refused too. */
    push(&test, 1U, FOC_CAN_SET_INPUT_VEL, false, closed_loop, 4U);
    const uint8_t position[8] = {0, 0, 0, 0, 1, 0, 0, 0};
    push(&test, 1U, FOC_CAN_SET_INPUT_POS, false, position, 8U);
    push(&test, 1U, FOC_CAN_HEARTBEAT, true, NULL, 0U);
    poll(&test);
    CHECK(
        test.count == 1U && test.sent[0].id == 0x21U && test.sent[0].data[4] == 1U &&
        test.sent[0].data[5] == FOC_CAN_PROCEDURE_INVALID_STATE
    );
    CHECK(((foc_can_link_status(&test.can) >> 8) & 0xFFU) == 3U);
    CHECK((foc_can_link_status(&test.can) & 0xFFU) == FOC_CAN_STATE_ACTIVE);
    /* SDO read of ID 120 returns the node; an unknown ID returns INVALID_ARGUMENT. */
    test.count = 0U;
    const uint8_t read_node[8] = {0, 120, 0, 0, 0, 0, 0, 0};
    const uint8_t read_unknown[8] = {0, 0xE7, 0x03, 0, 0, 0, 0, 0};
    push(&test, 1U, FOC_CAN_RX_SDO, false, read_node, 8U);
    push(&test, 1U, FOC_CAN_RX_SDO, false, read_unknown, 8U);
    poll(&test);
    CHECK(
        test.count == 2U && test.sent[0].id == 0x25U && test.sent[0].data[0] == 0U &&
        test.sent[0].data[1] == 120U && get_u32(test.sent[0].data + 4) == 1U
    );
    CHECK(test.sent[1].data[0] == FOC_PROTO_INVALID_ARGUMENT && test.sent[1].data[1] == 0xE7U);
    /* Estop sets ESTOP_REQUESTED until Clear_Errors. */
    test.count = 0U;
    push(&test, 1U, FOC_CAN_ESTOP, false, NULL, 0U);
    push(&test, 1U, FOC_CAN_GET_ERROR, true, NULL, 0U);
    const uint8_t identify[1] = {0};
    push(&test, 1U, FOC_CAN_CLEAR_ERRORS, false, identify, 1U);
    push(&test, 1U, FOC_CAN_GET_ERROR, true, NULL, 0U);
    poll(&test);
    CHECK(test.count == 2U);
    CHECK((get_u32(test.sent[0].data) & FOC_CAN_ERROR_ESTOP_REQUESTED) != 0U);
    CHECK((get_u32(test.sent[1].data) & FOC_CAN_ERROR_ESTOP_REQUESTED) == 0U);
    /* RX overflow and TX queue overflow both count as dropped frames. */
    for (unsigned int index = 0U; index <= FOC_CAN_RX_CAPACITY; ++index) {
        push(&test, 9U, FOC_CAN_GET_VERSION, true, NULL, 0U);
    }
    CHECK((foc_can_link_status(&test.can) >> 16) == 1U);
    test.accept = false;
    for (unsigned int round = 0U; round < 5U; ++round) {
        for (unsigned int index = 0U; index < FOC_CAN_RX_BUDGET; ++index) {
            push(&test, 1U, FOC_CAN_GET_VERSION, true, NULL, 0U);
        }
        poll(&test);
    }
    CHECK((foc_can_link_status(&test.can) >> 16) > 1U);
    return 0;
}

static int test_error_map(void)
{
    CHECK(foc_can_errors_from_faults(0U) == 0U);
    CHECK(
        foc_can_errors_from_faults(FOC_SAFE_FAULT_COMMAND_TIMEOUT) ==
        FOC_CAN_ERROR_WATCHDOG_TIMER_EXPIRED
    );
    CHECK(
        foc_can_errors_from_faults(FOC_SAFE_FAULT_DRIVER | FOC_SAFE_FAULT_OVER_VOLTAGE) ==
        (FOC_CAN_ERROR_DRV_FAULT | FOC_CAN_ERROR_DC_BUS_OVER_VOLTAGE)
    );
    CHECK(foc_can_errors_from_faults(1U << 20) == FOC_CAN_ERROR_SYSTEM_LEVEL);
    return 0;
}

/* MIT (ID 128 = 1): raw-ID frames to the node, a reply to the master ID for each one,
 * special commands, refusals outside motor mode and both reply formats. */
static void push_raw(struct fixture *test, uint16_t id, const uint8_t *data, uint8_t length)
{
    struct foc_can_frame frame = {.id = id, .length = length};
    memcpy(frame.data, data, length);
    foc_can_rx_push(&test->buffer, &frame);
}

static int test_mit(void)
{
    static struct fixture test;
    CHECK(init(&test) == 0);
    test.serial.can.protocol = FOC_CAN_PROTOCOL_MIT;
    test.serial.can.node_id = 5U;
    test.serial.can.master_id = 0x10U;
    /* No cyclic CANSimple frames in MIT; a CANSimple frame to the node is ignored. */
    test.time = 500000U;
    push(&test, 5U, FOC_CAN_GET_VERSION, true, NULL, 0U);
    poll(&test);
    CHECK(test.count == 0U);

    /* A command outside motor mode is refused but still answered: original 6 bytes. */
    const uint8_t command[8] = {0x80U, 0x00U, 0x80U, 0x00U, 0x00U, 0x00U, 0x08U, 0x00U};
    push_raw(&test, 5U, command, 8U);
    push_raw(&test, 6U, command, 8U); /* another node */
    poll(&test);
    CHECK(test.count == 1U && test.sent[0].id == 0x10U && test.sent[0].length == 6U);
    /* At rest the position, velocity and torque codes sit mid-range. */
    const uint8_t rest[6] = {5U, 0x80U, 0x00U, 0x80U, 0x08U, 0x00U};
    CHECK(memcmp(test.sent[0].data, rest, sizeof rest) == 0);
    CHECK((foc_can_link_status(&test.can) >> 8 & 0xFFU) == 1U);

    /* DaMiao replies: 8 bytes, error nibble 0 (disabled) above the node. */
    test.serial.can.mit_reply = FOC_CAN_MIT_REPLY_DAMIAO;
    const uint8_t clear[8] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFBU};
    push_raw(&test, 5U, clear, 8U);
    poll(&test);
    CHECK(test.count == 2U && test.sent[1].length == 8U && test.sent[1].data[0] == 0x05U);
    CHECK(test.sent[1].data[6] == 0U && test.sent[1].data[7] == 0U);

    /* Zero while DISABLED sets the output zero to the present position (0 here). */
    const uint8_t zero[8] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFEU};
    test.serial.output_zero_rad = 0.25f;
    push_raw(&test, 5U, zero, 8U);
    poll(&test);
    CHECK(test.count == 3U && test.serial.output_zero_rad == 0.0f);

    /* Enter without an identified flux: IMPEDANCE is unavailable, the attempt times out. */
    const uint8_t enter[8] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFCU};
    push_raw(&test, 5U, enter, 8U);
    poll(&test);
    CHECK(test.count == 4U && test.can.mit_stage == 1U);
    test.time += 300000U;
    poll(&test);
    CHECK(test.can.mit_stage == 0U && (foc_can_link_status(&test.can) >> 8 & 0xFFU) == 2U);

    /* Exit answers too, and the CANSimple heartbeat returns with protocol 0. */
    const uint8_t leave[8] = {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFDU};
    push_raw(&test, 5U, leave, 8U);
    poll(&test);
    CHECK(test.count == 5U);
    test.serial.can.protocol = FOC_CAN_PROTOCOL_CANSIMPLE;
    test.time += 200000U;
    poll(&test);
    CHECK(count_id(&test, (uint16_t)(5U << 5 | FOC_CAN_HEARTBEAT)) == 1U);
    return 0;
}

/* Test Entry ------------------------------------------------------------------- */

int main(void)
{
    if (test_address_and_ids() != 0 || test_cyclic() != 0 || test_refusals_and_counters() != 0 ||
        test_error_map() != 0 || test_mit() != 0) {
        return 1;
    }
    puts("test_foc_can: PASS");
    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
