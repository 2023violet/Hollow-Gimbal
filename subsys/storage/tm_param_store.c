/**
 * @file    tm_param_store.c
 * @brief   Themis A/B persistent parameter store above tiny_param and tm_flash_device
 * @version v1.1.0
 * @date    2026.04.07
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "tm_param_store.h"
#include <errno.h>
#include <stddef.h>
#include <string.h>

/* Private Macros ------------------------------------------------------------------------------- */

#define TM_PARAM_STORE_SLOT_A        (0U)
#define TM_PARAM_STORE_SLOT_B        (1U)
#define TM_PARAM_STORE_SLOT_COUNT    (2U)
#define TM_PARAM_STORE_SEQUENCE_SIZE (sizeof(uint32_t))

/* Private Types -------------------------------------------------------------------------------- */

struct tm_param_store_slot_runtime {
    struct tp_slot      slot;
    struct tp_slot_info info;
    int                 status;
    uint32_t            sequence;
    bool                has_sequence;
    bool                valid;
};

/* Private Helpers ------------------------------------------------------------------------------ */

static int _tm_param_store_backend_read(void *user_ctx, uint32_t addr, uint8_t *buf, uint32_t size);
static int _tm_param_store_backend_write(void          *user_ctx,
                                         uint32_t       addr,
                                         const uint8_t *buf,
                                         uint32_t       size);
static int _tm_param_store_backend_erase(void *user_ctx, uint32_t addr, uint32_t size);
static const void *_tm_param_store_backend_map_readonly(void    *user_ctx,
                                                        uint32_t addr,
                                                        uint32_t size);
static int         _tm_param_store_open(struct tm_param_store *store);
static void        _tm_param_store_close(struct tm_param_store *store);
static int         _tm_param_store_u32_add(uint32_t lhs, uint32_t rhs, uint32_t *out);
static int         _tm_param_store_pair_validate(const struct tm_param_store           *store,
                                                 const struct tm_param_store_slot_pair *pair,
                                                 uint32_t                               record_len);
static void        _tm_param_store_record_prefix_read(const void                         *record,
                                                      struct tm_param_store_record_prefix *prefix);
static void        _tm_param_store_record_prefix_write(void                                  *record,
                                                       const struct tm_param_store_record_prefix *prefix);
static int         _tm_param_store_build_runtime_slots(const struct tm_param_store           *store,
                                                       const struct tm_param_store_slot_pair *pair,
                                                       struct tm_param_store_slot_runtime     slots[2]);
static int         _tm_param_store_read_sequence(struct tm_param_store *store,
                                                 const struct tp_slot  *slot,
                                                 uint32_t              *sequence);
static int         _tm_param_store_scan_slots(struct tm_param_store                 *store,
                                              const struct tm_param_store_slot_pair *pair,
                                              uint32_t                               record_len,
                                              struct tm_param_store_slot_runtime     slots[2]);
static int         _tm_param_store_status_select(const struct tm_param_store_slot_runtime slots[2],
                                                 int                                     *active_slot,
                                                 int                                     *write_slot,
                                                 uint32_t                                *current_sequence);
static int  _tm_param_store_status_pick_error(const struct tm_param_store_slot_runtime slots[2]);
static void _tm_param_store_diag_fill(const struct tm_param_store_slot_runtime slots[2],
                                      int                                      status,
                                      int                                      active_slot,
                                      int                                      write_slot,
                                      uint32_t                                 current_sequence,
                                      struct tm_param_store_pair_diag         *diag);

/* Private State -------------------------------------------------------------------------------- */

static const struct tp_flash_backend_ops _tm_param_store_backend_ops = {
    .read         = _tm_param_store_backend_read,
    .write        = _tm_param_store_backend_write,
    .erase        = _tm_param_store_backend_erase,
    .map_readonly = _tm_param_store_backend_map_readonly,
};

/* Public Functions ----------------------------------------------------------------------------- */

int tm_param_store_init(struct tm_param_store *store, struct tm_flash_device *flash)
{
    int result;

    if ((store == NULL) || (flash == NULL)) {
        return -EINVAL;
    }

    memset(store, 0, sizeof(*store));

    result = tm_device_get(&flash->parent);
    if (result != 0) {
        return result;
    }

    result = tm_flash_get_info(flash, &store->flash_info);
    if (result != 0) {
        tm_device_put(&flash->parent);
        return result;
    }

    result = tm_device_get_name(&flash->parent, store->flash_name, (uint8_t)sizeof(store->flash_name));
    if (result != 0) {
        tm_device_put(&flash->parent);
        return result;
    }

    store->flash       = flash;
    store->initialized = true;
    return 0;
}

void tm_param_store_deinit(struct tm_param_store *store)
{
    if ((store == NULL) || (store->flash == NULL)) {
        if (store != NULL) {
            memset(store, 0, sizeof(*store));
        }
        return;
    }

    tm_device_put(&store->flash->parent);
    memset(store, 0, sizeof(*store));
}

int tm_param_store_init_by_name(struct tm_param_store *store, const char *flash_name)
{
    struct tm_flash_device *flash;
    int                     result;

    if ((store == NULL) || (flash_name == NULL)) {
        return -EINVAL;
    }

    flash = tm_flash_device_find(flash_name);
    if (flash == NULL) {
        return -ENODEV;
    }

    result = tm_param_store_init(store, flash);
    tm_device_put(&flash->parent);
    return result;
}

bool tm_param_store_is_ready(const struct tm_param_store *store)
{
    return ((store != NULL) && store->initialized && (store->flash != NULL));
}

int tm_param_store_check_pair(struct tm_param_store                 *store,
                              const struct tm_param_store_slot_pair *pair,
                              uint32_t                               record_len,
                              struct tm_param_store_pair_diag       *diag)
{
    struct tm_param_store_slot_runtime slots[2] = {0};
    int                                active_slot;
    int                                write_slot;
    uint32_t                           current_sequence;
    int                                result;

    result = _tm_param_store_pair_validate(store, pair, record_len);
    if (result != 0) {
        if (diag != NULL) {
            memset(diag, 0, sizeof(*diag));
            diag->status = result;
        }
        return result;
    }

    result = _tm_param_store_open(store);
    if (result != 0) {
        if (diag != NULL) {
            memset(diag, 0, sizeof(*diag));
            diag->status = result;
        }
        return result;
    }

    result = _tm_param_store_scan_slots(store, pair, record_len, slots);
    if (result == 0) {
        result = _tm_param_store_status_select(slots, &active_slot, &write_slot, &current_sequence);
    } else {
        active_slot      = -1;
        write_slot       = -1;
        current_sequence = 0U;
    }

    _tm_param_store_close(store);

    if (diag != NULL) {
        _tm_param_store_diag_fill(slots, result, active_slot, write_slot, current_sequence, diag);
    }

    return result;
}

int tm_param_store_save_pair(struct tm_param_store                 *store,
                             const struct tm_param_store_slot_pair *pair,
                             void                                  *record,
                             uint32_t                               record_len,
                             struct tm_param_store_pair_diag       *diag)
{
    struct tm_param_store_slot_runtime  *active;
    struct tm_param_store_slot_runtime  *write_slot_runtime;
    struct tm_param_store_slot_runtime   slots[2] = {0};
    struct tm_param_store_record_prefix  prefix = {0};
    int                                  active_slot;
    int                                  write_slot;
    uint32_t                             current_sequence;
    uint32_t                             computed_crc;
    int                                  result;

    result = _tm_param_store_pair_validate(store, pair, record_len);
    if ((result != 0) || (record == NULL)) {
        if (diag != NULL) {
            memset(diag, 0, sizeof(*diag));
            diag->status = (result != 0) ? result : -EINVAL;
        }
        return (result != 0) ? result : -EINVAL;
    }

    result = _tm_param_store_open(store);
    if (result != 0) {
        if (diag != NULL) {
            memset(diag, 0, sizeof(*diag));
            diag->status = result;
        }
        return result;
    }

    result = _tm_param_store_scan_slots(store, pair, record_len, slots);
    if (result == 0) {
        result = _tm_param_store_status_select(slots, &active_slot, &write_slot, &current_sequence);
    } else {
        active_slot      = -1;
        write_slot       = (int)TM_PARAM_STORE_SLOT_A;
        current_sequence = 0U;
    }

    _tm_param_store_record_prefix_read(record, &prefix);
    prefix.sequence    = current_sequence;
    _tm_param_store_record_prefix_write(record, &prefix);
    active             = (active_slot >= 0) ? &slots[(uint32_t)active_slot] : NULL;
    write_slot_runtime = &slots[(uint32_t)write_slot];

    if ((active != NULL) && (active->status == 0) && (active->info.data_len == record_len)) {
        computed_crc = tp_calc_crc(&active->slot, record, record_len);
        if (computed_crc == active->info.stored_crc) {
            result = 0;
            goto exit_with_diag;
        }
    }

    prefix.sequence  = current_sequence + 1U;
    _tm_param_store_record_prefix_write(record, &prefix);
    result           = tp_save_backend(&write_slot_runtime->slot,
                             &(const struct tp_flash_backend){
                                           .ops      = &_tm_param_store_backend_ops,
                                           .user_ctx = store,
                             },
                             record,
                             record_len);

exit_with_diag:
    _tm_param_store_close(store);

    if (diag != NULL) {
        (void)tm_param_store_check_pair(store, pair, record_len, diag);
        if ((result != 0) && (diag->status == 0)) {
            diag->status = result;
        }
    }

    return result;
}

int tm_param_store_load_pair(struct tm_param_store                 *store,
                             const struct tm_param_store_slot_pair *pair,
                             void                                  *record,
                             uint32_t                               record_len,
                             struct tm_param_store_pair_diag       *diag)
{
    struct tm_param_store_slot_runtime slots[2] = {0};
    int                                active_slot;
    int                                write_slot;
    uint32_t                           current_sequence;
    int                                result;

    result = _tm_param_store_pair_validate(store, pair, record_len);
    if ((result != 0) || (record == NULL)) {
        if (diag != NULL) {
            memset(diag, 0, sizeof(*diag));
            diag->status = (result != 0) ? result : -EINVAL;
        }
        return (result != 0) ? result : -EINVAL;
    }

    result = _tm_param_store_open(store);
    if (result != 0) {
        if (diag != NULL) {
            memset(diag, 0, sizeof(*diag));
            diag->status = result;
        }
        return result;
    }

    result = _tm_param_store_scan_slots(store, pair, record_len, slots);
    if (result == 0) {
        result = _tm_param_store_status_select(slots, &active_slot, &write_slot, &current_sequence);
    } else {
        active_slot      = -1;
        write_slot       = -1;
        current_sequence = 0U;
    }

    if ((result == 0) && (active_slot >= 0)) {
        result = tp_load_backend(&slots[(uint32_t)active_slot].slot,
                                 &(const struct tp_flash_backend){
                                     .ops      = &_tm_param_store_backend_ops,
                                     .user_ctx = store,
                                 },
                                 record,
                                 record_len);
    }

    _tm_param_store_close(store);

    if (diag != NULL) {
        _tm_param_store_diag_fill(slots, result, active_slot, write_slot, current_sequence, diag);
    }

    return result;
}

int tm_param_store_erase_pair(struct tm_param_store                 *store,
                              const struct tm_param_store_slot_pair *pair,
                              struct tm_param_store_pair_diag       *diag)
{
    struct tm_param_store_slot_runtime slots[2] = {0};
    int                                result;

    result = _tm_param_store_pair_validate(store, pair, TM_PARAM_STORE_SEQUENCE_SIZE);
    if (result != 0) {
        if (diag != NULL) {
            memset(diag, 0, sizeof(*diag));
            diag->status = result;
        }
        return result;
    }

    result = _tm_param_store_open(store);
    if (result != 0) {
        if (diag != NULL) {
            memset(diag, 0, sizeof(*diag));
            diag->status = result;
        }
        return result;
    }

    result = _tm_param_store_build_runtime_slots(store, pair, slots);
    if (result == 0) {
        result = tp_erase_backend(&slots[TM_PARAM_STORE_SLOT_B].slot,
                                  &(const struct tp_flash_backend){
                                      .ops      = &_tm_param_store_backend_ops,
                                      .user_ctx = store,
                                  });
    }
    if (result == 0) {
        result = tp_erase_backend(&slots[TM_PARAM_STORE_SLOT_A].slot,
                                  &(const struct tp_flash_backend){
                                      .ops      = &_tm_param_store_backend_ops,
                                      .user_ctx = store,
                                  });
    }

    _tm_param_store_close(store);

    if (diag != NULL) {
        (void)tm_param_store_check_pair(store, pair, TM_PARAM_STORE_SEQUENCE_SIZE, diag);
        if ((result != 0) && (diag->status == 0)) {
            diag->status = result;
        }
    }

    return result;
}

const char *tm_param_store_strerror(int rc)
{
    switch (rc) {
    case 0:
        return "TMPS_OK";
    case -ENODEV:
        return "TMPS_ERR_NO_DEVICE";
    case -ENOSPC:
        return "TMPS_ERR_NO_SPACE";
    case -EINVAL:
        return "TMPS_ERR_INVALID_ARG";
    default:
        return tp_strerr(rc);
    }
}

/* Private Helpers ------------------------------------------------------------------------------ */

static int _tm_param_store_backend_read(void *user_ctx, uint32_t addr, uint8_t *buf, uint32_t size)
{
    struct tm_param_store *store = (struct tm_param_store *)user_ctx;
    uint32_t               offset;

    ssize_t read_size;

    if ((store == NULL) || ((buf == NULL) && (size > 0U))) {
        return -EINVAL;
    }

    if ((addr < store->flash_info.base) ||
        (size > (store->flash_info.size - (addr - store->flash_info.base)))) {
        return -EINVAL;
    }

    offset    = addr - store->flash_info.base;
    read_size = tm_flash_read(store->flash, offset, buf, size);
    return (read_size == (ssize_t)size) ? (int)size : -ETIMEDOUT;
}

static int _tm_param_store_backend_write(void          *user_ctx,
                                         uint32_t       addr,
                                         const uint8_t *buf,
                                         uint32_t       size)
{
    struct tm_param_store *store = (struct tm_param_store *)user_ctx;
    uint32_t               offset;

    if ((store == NULL) || ((buf == NULL) && (size > 0U))) {
        return -EINVAL;
    }

    ssize_t write_size;

    if ((addr < store->flash_info.base) ||
        (size > (store->flash_info.size - (addr - store->flash_info.base)))) {
        return -EINVAL;
    }

    offset     = addr - store->flash_info.base;
    write_size = tm_flash_write(store->flash, offset, buf, size);
    return (write_size == (ssize_t)size) ? (int)size : -ETIMEDOUT;
}

static int _tm_param_store_backend_erase(void *user_ctx, uint32_t addr, uint32_t size)
{
    struct tm_param_store *store = (struct tm_param_store *)user_ctx;
    uint32_t               offset;

    if (store == NULL) {
        return -EINVAL;
    }

    if ((addr < store->flash_info.base) ||
        (size > (store->flash_info.size - (addr - store->flash_info.base)))) {
        return -EINVAL;
    }

    offset = addr - store->flash_info.base;
    return tm_flash_erase(store->flash, offset, size);
}

static const void *_tm_param_store_backend_map_readonly(void    *user_ctx,
                                                        uint32_t addr,
                                                        uint32_t size)
{
    struct tm_param_store *store = (struct tm_param_store *)user_ctx;
    uint32_t               offset;

    if (store == NULL) {
        return NULL;
    }

    if ((addr < store->flash_info.base) ||
        (size > (store->flash_info.size - (addr - store->flash_info.base)))) {
        return NULL;
    }

    offset = addr - store->flash_info.base;
    return tm_flash_map_readonly(store->flash, offset, size);
}

static int _tm_param_store_open(struct tm_param_store *store)
{
    if (!tm_param_store_is_ready(store)) {
        return -ENODEV;
    }

    return tm_flash_open(store->flash);
}

static void _tm_param_store_close(struct tm_param_store *store)
{
    if (!tm_param_store_is_ready(store)) {
        return;
    }

    (void)tm_flash_close(store->flash);
}

static int _tm_param_store_u32_add(uint32_t lhs, uint32_t rhs, uint32_t *out)
{
    if (out == NULL) {
        return -EINVAL;
    }

    if (lhs > (UINT32_MAX - rhs)) {
        return -ERANGE;
    }

    *out = lhs + rhs;
    return 0;
}

static int _tm_param_store_pair_validate(const struct tm_param_store           *store,
                                         const struct tm_param_store_slot_pair *pair,
                                         uint32_t                               record_len)
{
    uint32_t slot_a_end;
    uint32_t slot_b_end;
    uint32_t slot_payload_capacity;

    if (!tm_param_store_is_ready(store) || (pair == NULL)) {
        return -EINVAL;
    }

    if ((store->flash_info.sector_size == 0U) || (store->flash_info.write_align == 0U)) {
        return -EINVAL;
    }

    if ((pair->slot_size <= TP_HEADER_SIZE) || (pair->slot_a_offset == pair->slot_b_offset) ||
        (pair->slot_size > store->flash_info.size)) {
        return -EINVAL;
    }

    if ((_tm_param_store_u32_add(pair->slot_a_offset, pair->slot_size, &slot_a_end) != 0) ||
        (_tm_param_store_u32_add(pair->slot_b_offset, pair->slot_size, &slot_b_end) != 0)) {
        return -EINVAL;
    }

    if ((slot_a_end > store->flash_info.size) || (slot_b_end > store->flash_info.size)) {
        return -EINVAL;
    }

    if (((pair->slot_a_offset % store->flash_info.sector_size) != 0U) ||
        ((pair->slot_b_offset % store->flash_info.sector_size) != 0U) ||
        ((pair->slot_size % store->flash_info.sector_size) != 0U)) {
        return -EINVAL;
    }

    if ((pair->slot_a_offset < pair->slot_b_offset) ? (slot_a_end > pair->slot_b_offset)
                                                    : (slot_b_end > pair->slot_a_offset)) {
        return -EINVAL;
    }

    slot_payload_capacity = pair->slot_size - TP_HEADER_SIZE;
    if (slot_payload_capacity < TM_PARAM_STORE_SEQUENCE_SIZE) {
        return -ENOSPC;
    }

    if (record_len == 0U) {
        return 0;
    }

    if ((record_len < TM_PARAM_STORE_SEQUENCE_SIZE) || (record_len > slot_payload_capacity) ||
        ((record_len % store->flash_info.write_align) != 0U)) {
        return -EINVAL;
    }

    return 0;
}

static void _tm_param_store_record_prefix_read(const void                         *record,
                                               struct tm_param_store_record_prefix *prefix)
{
    if ((record == NULL) || (prefix == NULL)) {
        return;
    }

    memcpy(prefix, record, sizeof(*prefix));
}

static void _tm_param_store_record_prefix_write(void                                  *record,
                                                const struct tm_param_store_record_prefix *prefix)
{
    if ((record == NULL) || (prefix == NULL)) {
        return;
    }

    memcpy(record, prefix, sizeof(*prefix));
}

static int _tm_param_store_build_runtime_slots(const struct tm_param_store           *store,
                                               const struct tm_param_store_slot_pair *pair,
                                               struct tm_param_store_slot_runtime     slots[2])
{
    uint32_t slot_a_addr;
    uint32_t slot_b_addr;

    if ((store == NULL) || (pair == NULL) || (slots == NULL)) {
        return -EINVAL;
    }

    memset(slots, 0, sizeof(struct tm_param_store_slot_runtime) * TM_PARAM_STORE_SLOT_COUNT);

    if ((_tm_param_store_u32_add(store->flash_info.base, pair->slot_a_offset, &slot_a_addr) != 0) ||
        (_tm_param_store_u32_add(store->flash_info.base, pair->slot_b_offset, &slot_b_addr) != 0)) {
        return -ERANGE;
    }

    slots[TM_PARAM_STORE_SLOT_A].slot = (const struct tp_slot){
        .addr    = slot_a_addr,
        .max_len = pair->slot_size - TP_HEADER_SIZE,
        .magic   = pair->magic,
        .version = pair->version,
    };
    slots[TM_PARAM_STORE_SLOT_B].slot = (const struct tp_slot){
        .addr    = slot_b_addr,
        .max_len = pair->slot_size - TP_HEADER_SIZE,
        .magic   = pair->magic,
        .version = pair->version,
    };
    slots[TM_PARAM_STORE_SLOT_A].info.addr = slots[TM_PARAM_STORE_SLOT_A].slot.addr;
    slots[TM_PARAM_STORE_SLOT_B].info.addr = slots[TM_PARAM_STORE_SLOT_B].slot.addr;
    return 0;
}

static int _tm_param_store_read_sequence(struct tm_param_store *store,
                                         const struct tp_slot  *slot,
                                         uint32_t              *sequence)
{
    struct tm_param_store_record_prefix        prefix = {0};
    const void                                *mapped_data;
    ssize_t                                    read_size;
    uint32_t                                   offset;

    if ((store == NULL) || (slot == NULL) || (sequence == NULL)) {
        return -EINVAL;
    }

    offset = (slot->addr + TP_HEADER_SIZE) - store->flash_info.base;
    mapped_data = tm_flash_map_readonly(store->flash, offset, sizeof(prefix));
    if (mapped_data != NULL) {
        memcpy(&prefix, mapped_data, sizeof(prefix));
        *sequence = prefix.sequence;
        return 0;
    }

    read_size = tm_flash_read(store->flash, offset, &prefix, sizeof(prefix));
    if (read_size != (ssize_t)sizeof(prefix)) {
        return -ETIMEDOUT;
    }

    *sequence = prefix.sequence;
    return 0;
}

static int _tm_param_store_scan_slots(struct tm_param_store                 *store,
                                      const struct tm_param_store_slot_pair *pair,
                                      uint32_t                               record_len,
                                      struct tm_param_store_slot_runtime     slots[2])
{
    struct tp_flash_backend backend = {
        .ops      = &_tm_param_store_backend_ops,
        .user_ctx = store,
    };
    int result;

    result = _tm_param_store_build_runtime_slots(store, pair, slots);
    if (result != 0) {
        return result;
    }

    for (uint32_t i = 0U; i < TM_PARAM_STORE_SLOT_COUNT; i++) {
        slots[i].status = tp_check_backend(&slots[i].slot, &backend, &slots[i].info);
        if ((slots[i].status == 0) && (slots[i].info.data_len != record_len)) {
            slots[i].status      = -EPROTO;
            slots[i].info.status = -EPROTO;
        }

        if (slots[i].status == 0) {
            slots[i].status =
                _tm_param_store_read_sequence(store, &slots[i].slot, &slots[i].sequence);
            if (slots[i].status == 0) {
                slots[i].has_sequence = true;
                slots[i].valid        = true;
            }
            slots[i].info.status = slots[i].status;
        }
    }

    return 0;
}

static int _tm_param_store_status_select(const struct tm_param_store_slot_runtime slots[2],
                                         int                                     *active_slot,
                                         int                                     *write_slot,
                                         uint32_t                                *current_sequence)
{
    bool a_valid;
    bool b_valid;

    if ((slots == NULL) || (active_slot == NULL) || (write_slot == NULL) ||
        (current_sequence == NULL)) {
        return -EINVAL;
    }

    a_valid = slots[TM_PARAM_STORE_SLOT_A].valid;
    b_valid = slots[TM_PARAM_STORE_SLOT_B].valid;

    if (a_valid && b_valid) {
        if (slots[TM_PARAM_STORE_SLOT_A].sequence >= slots[TM_PARAM_STORE_SLOT_B].sequence) {
            *active_slot = (int)TM_PARAM_STORE_SLOT_A;
            *write_slot  = (int)TM_PARAM_STORE_SLOT_B;
        } else {
            *active_slot = (int)TM_PARAM_STORE_SLOT_B;
            *write_slot  = (int)TM_PARAM_STORE_SLOT_A;
        }
        *current_sequence = slots[(uint32_t)*active_slot].sequence;
        return 0;
    }

    if (a_valid) {
        *active_slot      = (int)TM_PARAM_STORE_SLOT_A;
        *write_slot       = (int)TM_PARAM_STORE_SLOT_B;
        *current_sequence = slots[TM_PARAM_STORE_SLOT_A].sequence;
        return 0;
    }

    if (b_valid) {
        *active_slot      = (int)TM_PARAM_STORE_SLOT_B;
        *write_slot       = (int)TM_PARAM_STORE_SLOT_A;
        *current_sequence = slots[TM_PARAM_STORE_SLOT_B].sequence;
        return 0;
    }

    *active_slot      = -1;
    *write_slot       = (int)TM_PARAM_STORE_SLOT_A;
    *current_sequence = 0U;
    return _tm_param_store_status_pick_error(slots);
}

static int _tm_param_store_status_pick_error(const struct tm_param_store_slot_runtime slots[2])
{
    if ((slots[TM_PARAM_STORE_SLOT_A].status == TP_EMPTY) &&
        (slots[TM_PARAM_STORE_SLOT_B].status == TP_EMPTY)) {
        return TP_EMPTY;
    }

    if (slots[TM_PARAM_STORE_SLOT_A].status != TP_EMPTY) {
        return slots[TM_PARAM_STORE_SLOT_A].status;
    }

    return slots[TM_PARAM_STORE_SLOT_B].status;
}

static void _tm_param_store_diag_fill(const struct tm_param_store_slot_runtime slots[2],
                                      int                                      status,
                                      int                                      active_slot,
                                      int                                      write_slot,
                                      uint32_t                                 current_sequence,
                                      struct tm_param_store_pair_diag         *diag)
{
    if ((slots == NULL) || (diag == NULL)) {
        return;
    }

    memset(diag, 0, sizeof(*diag));
    diag->status           = status;
    diag->active_valid     = (active_slot >= 0);
    diag->active_slot      = (active_slot >= 0) ? (uint8_t)active_slot : 0xFFU;
    diag->write_slot       = (write_slot >= 0) ? (uint8_t)write_slot : 0xFFU;
    diag->current_sequence = current_sequence;

    for (uint32_t i = 0U; i < TM_PARAM_STORE_SLOT_COUNT; i++) {
        diag->slots[i].addr         = slots[i].slot.addr;
        diag->slots[i].sequence     = slots[i].sequence;
        diag->slots[i].has_sequence = slots[i].has_sequence;
        diag->slots[i].slot         = slots[i].info;
    }
}

/* ---------------------------------------- end of file ----------------------------------------- */
