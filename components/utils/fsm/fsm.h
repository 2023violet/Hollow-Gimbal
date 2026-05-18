/**
 * @file    fsm.h
 * @brief   Lightweight table-driven finite state machine helper
 * @version v1.1.0
 * @date    2026.04.24
 *
 * Design overview
 * ---------------
 * `fsm` is a small table-driven finite-state-machine helper for portable
 * embedded control logic. State and transition lookups are linear by design so
 * the component stays deterministic, dependency-light, and easy to audit for
 * small to medium state graphs.
 *
 * Usage example
 * -------------
 * static const struct fsm_state_node g_states[] = {
 *     { STATE_IDLE, _on_idle_enter, _on_idle_exit },
 *     { STATE_RUN,  _on_run_enter,  _on_run_exit  },
 * };
 *
 * static const struct fsm_transition g_transitions[] = {
 *     { STATE_IDLE, EVT_START, STATE_RUN, _action_start },
 * };
 *
 * struct fsm motor_fsm = {0};
 * fsm_init(&motor_fsm, g_states, FSM_ARRAY_SIZE(g_states), g_transitions,
 *          FSM_ARRAY_SIZE(g_transitions), STATE_IDLE, motor_dev_ptr);
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - The caller owns the `struct fsm` instance and the state/transition tables.
 * - Tables must remain valid for the full lifetime of the FSM instance.
 * - The helper is not re-entrant; callers serialize access externally when one
 *   instance may be touched from multiple contexts.
 * - @note Not thread-safe. Caller must ensure external serialization. Not
 *   ISR-safe.
 *
 * Blocking / allocation
 * ---------------------
 * - No blocking and no dynamic allocation.
 * - Dispatch runs synchronously in the caller's current context and may invoke
 *   user callbacks directly.
 *
 * Failure / recovery
 * ------------------
 * - `fsm_init()` rejects malformed tables with `-EINVAL`.
 * - `fsm_dispatch_event()` returns `-ENOENT` when no rule matches the current
 *   state and event.
 * - Re-entrant `fsm_dispatch_event()` calls on the same instance are rejected
 *   with `-EAGAIN` while exit/enter callbacks are running.
 * - `fsm_force_state()` exists for explicit recovery flows that must bypass
 *   the normal transition table.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Macros -------------------------------------------------------------------------------- */

/**
 * @brief  Sentinel value used for "no event pending".
 */
#define FSM_EVENT_NONE    ((uint16_t)0xFFFFU)

/**
 * @brief  Sentinel value returned when no valid state is available.
 */
#define FSM_STATE_INVALID ((uint16_t)0xFFFFU)

/**
 * @brief  Return the element count of a statically declared array.
 *
 * @param  arr  Statically declared array.
 */
#define FSM_ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief  State descriptor with optional enter and exit callbacks.
 */
struct fsm_state_node {
    /**
     * @brief  Unique logical state identifier.
     */
    uint16_t state;

    /**
     * @brief  Callback invoked when the FSM enters this state.
     */
    void (*enter_cb)(void *user_data);

    /**
     * @brief  Callback invoked when the FSM leaves this state.
     */
    void (*exit_cb)(void *user_data);
};

/**
 * @brief  Transition rule mapping one `(state, event)` pair to a next state.
 */
struct fsm_transition {
    /**
     * @brief  State in which this rule is active.
     */
    uint16_t current_state;

    /**
     * @brief  Event that triggers this rule.
     */
    uint16_t event;

    /**
     * @brief  Target state entered when the transition succeeds.
     */
    uint16_t next_state;

    /**
     * @brief  Optional guard and side-effect callback.
     *
     * Returning false vetoes the transition without treating it as an error.
     */
    bool (*action_cb)(void *user_data);
};

/**
 * @brief  FSM control block.
 *
 * All fields are written by fsm_init(). Callers should treat them as runtime
 * state owned by the helper after initialization.
 */
struct fsm {
    /**
     * @brief  Currently active state ID.
     */
    uint16_t current_state;

    /**
     * @brief  Pointer to the state descriptor table.
     */
    const struct fsm_state_node *state_table;

    /**
     * @brief  Number of entries in @ref state_table.
     */
    size_t state_count;

    /**
     * @brief  Pointer to the transition descriptor table.
     */
    const struct fsm_transition *trans_table;

    /**
     * @brief  Number of entries in @ref trans_table.
     */
    size_t trans_count;

    /**
     * @brief  Opaque context forwarded to callbacks.
     */
    void *user_data;

    /**
     * @brief  Internal guard set while exit/enter callbacks are running.
     */
    bool is_transitioning;
};

/* Public Functions ----------------------------------------------------------------------------- */

/* Lifecycle ------------------------------------------------------------------------------------ */

/**
 * @brief  Initialize the FSM and enter the initial state.
 *
 * The full table contract is validated during initialization so runtime
 * dispatch does not silently jump into unknown states.
 *
 * @param  fsm          Control block to initialize. Must not be NULL.
 * @param  state_table  Array of state descriptors. Must not be NULL.
 * @param  state_count  Number of state descriptors. Must be non-zero.
 * @param  trans_table  Array of transition descriptors. May be NULL only when
 *                      @p trans_count is 0.
 * @param  trans_count  Number of transition descriptors.
 * @param  init_state   Initial state. Must exist in @p state_table.
 * @param  user_data    Opaque context forwarded to callbacks. May be NULL.
 *
 * @retval 0        Initialization succeeded.
 * @retval -EINVAL  Arguments are invalid, duplicate states exist, the initial
 *                  state is missing, or a transition references an unknown
 *                  state.
 *
 * @note   The initial state's `enter_cb` runs before the function returns.
 */
int fsm_init(struct fsm                  *fsm,
             const struct fsm_state_node *state_table,
             size_t                       state_count,
             const struct fsm_transition *trans_table,
             size_t                       trans_count,
             uint16_t                     init_state,
             void                        *user_data);

/* Runtime Step --------------------------------------------------------------------------------- */

/**
 * @brief  Dispatch one event through the transition table.
 *
 * Dispatch flow:
 * 1. Find the first matching `(current_state, event)` rule.
 * 2. Run `action_cb()` if present.
 * 3. If the action vetoes, keep the current state and return success.
 * 4. For non-self-transitions, run `exit_cb()`, update state, then run `enter_cb()`.
 *
 * @param  fsm    Initialized FSM instance. Must not be NULL.
 * @param  event  Event ID to dispatch.
 *
 * @retval 0        Transition handled successfully, including the intentional
 *                  veto case.
 * @retval -EINVAL  @p fsm is NULL.
 * @retval -EAGAIN  The same FSM instance is already inside a transition
 *                  callback and does not support recursive dispatch.
 * @retval -ENOENT  No matching transition exists for the current state/event.
 *
 * @note   Self-transitions do not invoke exit or enter callbacks.
 * @note   Recursive dispatch from `on_enter` or `on_exit` is rejected with
 *         `-EAGAIN`.
 */
int fsm_dispatch_event(struct fsm *fsm, uint16_t event);

/* Snapshot / Recovery -------------------------------------------------------------------------- */

/**
 * @brief  Return the current FSM state.
 *
 * @param  fsm  Initialized FSM instance. Must not be NULL.
 *
 * @return Current state ID, or `FSM_STATE_INVALID` when @p fsm is NULL.
 */
uint16_t fsm_current_state_get(const struct fsm *fsm);

/**
 * @brief  Force a state change without consulting the transition table.
 *
 * Intended for recovery flows such as fault fallback or emergency stop logic.
 *
 * @param  fsm         Initialized FSM instance. Must not be NULL.
 * @param  next_state  Target state. Must exist in the state table.
 *
 * @retval 0        Force-state transition succeeded.
 * @retval -EINVAL  @p fsm is NULL or @p next_state is unknown.
 * @retval -EAGAIN  The FSM is currently inside a transition callback.
 */
int fsm_force_state(struct fsm *fsm, uint16_t next_state);

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
