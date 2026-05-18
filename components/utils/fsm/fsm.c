/**
 * @file    fsm.c
 * @brief   Lightweight table-driven finite state machine implementation
 * @version v1.1.0
 * @date    2026.04.24
 *
 * Implementation role
 * -------------------
 * - pairs with `fsm.h`
 * - validates the static table contract once at init time
 * - keeps runtime dispatch focused on first-match lookup and callback routing
 *
 * Key collaborators
 * -----------------
 * - `fsm.h`: public table definitions and caller-facing lifecycle contract
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "fsm.h"
#include <errno.h>

/* Private Helper Declarations ------------------------------------------------------------------ */

/**
 * @brief  Find one state descriptor in the active FSM table.
 *
 * @param  fsm    FSM instance.
 * @param  state  State ID to search for.
 *
 * @return Matching state descriptor on success, or `NULL` when the state is
 *         not present.
 */
static const struct fsm_state_node *_fsm_find_state(const struct fsm *fsm, uint16_t state);

/**
 * @brief  Find one state descriptor in an arbitrary state table.
 */
static const struct fsm_state_node *_fsm_find_state_in_table(
    const struct fsm_state_node *state_table,
    size_t                       state_count,
    uint16_t                     state);

/**
 * @brief  Find the first transition matching one `(state, event)` pair.
 */
static const struct fsm_transition *_fsm_find_transition(const struct fsm *fsm,
                                                         uint16_t          state,
                                                         uint16_t          event);

/**
 * @brief  Apply the actual state change, including exit and enter callbacks.
 */
static void _fsm_do_transition(struct fsm                  *fsm,
                               const struct fsm_state_node *old_node,
                               const struct fsm_state_node *new_node,
                               uint16_t                     next_state);

/**
 * @brief  Validate the static FSM table contract.
 */
static int _fsm_validate_tables(const struct fsm_state_node *state_table,
                                size_t                       state_count,
                                const struct fsm_transition *trans_table,
                                size_t                       trans_count,
                                uint16_t                     init_state);

/* Public Functions ----------------------------------------------------------------------------- */

int fsm_init(struct fsm                  *fsm,
             const struct fsm_state_node *state_table,
             size_t                       state_count,
             const struct fsm_transition *trans_table,
             size_t                       trans_count,
             uint16_t                     init_state,
             void                        *user_data)
{
    const struct fsm_state_node *node;

    if (fsm == NULL || state_table == NULL) {
        return -EINVAL;
    }

    if ((state_count == 0U) || ((trans_count != 0U) && (trans_table == NULL))) {
        return -EINVAL;
    }
    if (_fsm_validate_tables(state_table, state_count, trans_table, trans_count, init_state) != 0) {
        return -EINVAL;
    }

    fsm->state_table   = state_table;
    fsm->state_count   = state_count;
    fsm->trans_table   = trans_table;
    fsm->trans_count   = trans_count;
    fsm->user_data     = user_data;
    fsm->is_transitioning = false;
    fsm->current_state = FSM_STATE_INVALID;

    node = _fsm_find_state(fsm, init_state);

    fsm->current_state = init_state;

    if (node->enter_cb != NULL) {
        fsm->is_transitioning = true;
        node->enter_cb(fsm->user_data);
        fsm->is_transitioning = false;
    }

    return 0;
}

int fsm_dispatch_event(struct fsm *fsm, uint16_t event)
{
    const struct fsm_state_node *old_node;
    const struct fsm_transition *trans;
    const struct fsm_state_node *new_node;

    if (fsm == NULL) {
        return -EINVAL;
    }
    if (fsm->is_transitioning) {
        return -EAGAIN;
    }

    old_node = _fsm_find_state(fsm, fsm->current_state);
    if (old_node == NULL) {
        return -EINVAL;
    }

    trans = _fsm_find_transition(fsm, fsm->current_state, event);
    if (trans == NULL) {
        return -ENOENT;
    }

    /*
     * A false action callback is an intentional veto, not an execution error.
     * The FSM simply stays in the current state.
     */
    if ((trans->action_cb != NULL) && !trans->action_cb(fsm->user_data)) {
        return 0;
    }

    new_node = _fsm_find_state(fsm, trans->next_state);
    if (new_node == NULL) {
        return -EINVAL;
    }

    _fsm_do_transition(fsm, old_node, new_node, trans->next_state);

    return 0;
}

uint16_t fsm_current_state_get(const struct fsm *fsm)
{
    if (fsm == NULL) {
        return FSM_STATE_INVALID;
    }

    return fsm->current_state;
}

int fsm_force_state(struct fsm *fsm, uint16_t next_state)
{
    const struct fsm_state_node *old_node;
    const struct fsm_state_node *new_node;

    if (fsm == NULL) {
        return -EINVAL;
    }
    if (fsm->is_transitioning) {
        return -EAGAIN;
    }

    old_node = _fsm_find_state(fsm, fsm->current_state);
    new_node = _fsm_find_state(fsm, next_state);
    if (new_node == NULL) {
        return -EINVAL;
    }

    _fsm_do_transition(fsm, old_node, new_node, next_state);

    return 0;
}

/* Private Helper Implementations --------------------------------------------------------------- */

static const struct fsm_state_node *_fsm_find_state(const struct fsm *fsm, uint16_t state)
{
    return _fsm_find_state_in_table(fsm->state_table, fsm->state_count, state);
}

static const struct fsm_state_node *_fsm_find_state_in_table(
    const struct fsm_state_node *state_table,
    size_t                       state_count,
    uint16_t                     state)
{
    for (size_t i = 0U; i < state_count; i++) {
        if (state_table[i].state == state) {
            return &state_table[i];
        }
    }

    return NULL;
}

static const struct fsm_transition *_fsm_find_transition(const struct fsm *fsm,
                                                         uint16_t          state,
                                                         uint16_t          event)
{
    for (size_t i = 0U; i < fsm->trans_count; i++) {
        if ((fsm->trans_table[i].current_state == state) && (fsm->trans_table[i].event == event)) {
            return &fsm->trans_table[i];
        }
    }

    return NULL;
}

static void _fsm_do_transition(struct fsm                  *fsm,
                               const struct fsm_state_node *old_node,
                               const struct fsm_state_node *new_node,
                               uint16_t                     next_state)
{
    if (next_state == fsm->current_state) {
        return;
    }

    if ((old_node != NULL) && (old_node->exit_cb != NULL)) {
        fsm->is_transitioning = true;
        old_node->exit_cb(fsm->user_data);
        fsm->is_transitioning = false;
    }

    /*
     * Commit the new state before the enter callback so any callback-side
     * read of current_state observes the post-transition value.
     */
    fsm->current_state = next_state;

    if ((new_node != NULL) && (new_node->enter_cb != NULL)) {
        fsm->is_transitioning = true;
        new_node->enter_cb(fsm->user_data);
        fsm->is_transitioning = false;
    }
}

static int _fsm_validate_tables(const struct fsm_state_node *state_table,
                                size_t                       state_count,
                                const struct fsm_transition *trans_table,
                                size_t                       trans_count,
                                uint16_t                     init_state)
{
    size_t i;
    size_t j;

    if (_fsm_find_state_in_table(state_table, state_count, init_state) == NULL) {
        return -EINVAL;
    }

    for (i = 0U; i < state_count; i++) {
        for (j = i + 1U; j < state_count; j++) {
            if (state_table[i].state == state_table[j].state) {
                return -EINVAL;
            }
        }
    }

    for (i = 0U; i < trans_count; i++) {
        if ((_fsm_find_state_in_table(state_table, state_count, trans_table[i].current_state) ==
             NULL) ||
            (_fsm_find_state_in_table(state_table, state_count, trans_table[i].next_state) ==
             NULL)) {
            return -EINVAL;
        }
    }

    return 0;
}

/* ---------------------------------------- end of file ----------------------------------------- */
