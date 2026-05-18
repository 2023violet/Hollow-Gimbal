/**
 * @file    tiny_param.c
 * @brief   Portable flash-slot parameter storage with version and CRC checks
 * @version v1.3.0
 * @date    2026.04.07
 *
 * Implementation role
 * -------------------
 * - pairs with `tiny_param.h`
 * - owns serialized header validation, CRC construction, and backend adapter
 *   selection between legacy ops and context-aware backends
 *
 * Key collaborators
 * -----------------
 * - `tiny_param.h`: slot format, hook contracts, and caller-facing status API
 * - `crc.h`: streaming CRC helper used for slot verification
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "tiny_param.h"
#include "crc.h"
#include <errno.h>
#include <stddef.h>
#include <string.h>

/* Private Types -------------------------------------------------------------------------------- */

/**
 * @brief  Serialized header prepended to every flash slot.
 *
 * Layout is frozen permanently because it bootstraps version detection for all
 * future firmware builds. Do not add, remove, or reorder fields.
 *
 * CRC coverage:
 *   Input region 1: bytes [0, 12) = magic + version + len
 *   Input region 2: payload body  = len bytes immediately after the header
 */
struct tp_header {
    uint32_t magic;
    uint32_t version;
    uint32_t len;
    uint32_t crc;
};

/* Compile-time guard: public TP_HEADER_SIZE constant must match actual struct. */
typedef char _tp_hdr_size_assert[(sizeof(struct tp_header) == TP_HEADER_SIZE) ? 1 : -1];

/* Private Constants ---------------------------------------------------------------------------- */

/** Value of an un-programmed flash word on AT32 and most ARM flash backends. */
#define _TP_BLANK_WORD            (0xFFFFFFFFU)

/** CRC stream chunk size for backends that do not expose readonly mapping. */
#define _TP_CRC_STREAM_CHUNK_SIZE (64U)

/* Private Function Prototypes ------------------------------------------------------------------ */

/*
 * Argument validation helpers are layered on purpose:
 * - legacy variants operate on `const struct tp_flash_ops`
 * - backend variants operate on `const struct tp_flash_backend`
 * - base helpers only check slot geometry and shared nullability rules
 *
 * This keeps the public API split explicit and avoids an extra adapter layer
 * in the hot validation path.
 */

static int  _tp_legacy_base_args_ok(const struct tp_slot *slot, const struct tp_flash_ops *ops);
static int  _tp_legacy_read_args_ok(const struct tp_slot *slot, const struct tp_flash_ops *ops);
static int  _tp_legacy_write_erase_args_ok(const struct tp_slot      *slot,
                                           const struct tp_flash_ops *ops);
static int  _tp_legacy_erase_args_ok(const struct tp_slot *slot, const struct tp_flash_ops *ops);
static int  _tp_backend_base_args_ok(const struct tp_slot          *slot,
                                     const struct tp_flash_backend *backend);
static int  _tp_backend_read_args_ok(const struct tp_slot          *slot,
                                     const struct tp_flash_backend *backend);
static int  _tp_backend_write_erase_args_ok(const struct tp_slot          *slot,
                                            const struct tp_flash_backend *backend);
static int  _tp_backend_erase_args_ok(const struct tp_slot          *slot,
                                      const struct tp_flash_backend *backend);
static bool _tp_payload_len_is_valid(const struct tp_slot *slot, uint32_t payload_len);
static bool _tp_slot_span_is_valid(const struct tp_slot *slot, uint32_t payload_len);
static void _tp_header_encode(uint8_t *dst, const struct tp_header *hdr);
static void _tp_header_prefix_encode(uint8_t *dst, uint32_t magic, uint32_t version, uint32_t len);
static uint32_t _tp_u32_decode_le(const uint8_t *src);

static int _tp_backend_read(const struct tp_flash_backend *backend,
                            uint32_t                       addr,
                            uint8_t                       *buf,
                            uint32_t                       size);
static int _tp_backend_write(const struct tp_flash_backend *backend,
                             uint32_t                       addr,
                             const uint8_t                 *buf,
                             uint32_t                       size);
static int _tp_backend_erase(const struct tp_flash_backend *backend, uint32_t addr, uint32_t size);

static const uint8_t *_tp_legacy_map_readonly(const struct tp_flash_ops *ops,
                                              uint32_t                   addr,
                                              uint32_t                   size);
static const uint8_t *_tp_backend_map_readonly(const struct tp_flash_backend *backend,
                                               uint32_t                       addr,
                                               uint32_t                       size);

static uint32_t _tp_build_crc(uint32_t       magic,
                              uint32_t       version,
                              uint32_t       len,
                              const uint8_t *payload);
/*
 * Keep legacy and backend CRC stream builders separate:
 * - each path can call its own read hook directly
 * - no extra thunk is needed to thread user_ctx through the backend variant
 * - error returns stay specific to the caller contract that failed
 */
static int      _tp_build_crc_stream_legacy(uint32_t                   magic,
                                            uint32_t                   version,
                                            uint32_t                   len,
                                            const struct tp_flash_ops *ops,
                                            uint32_t                   payload_addr,
                                            uint32_t                  *out_crc);
static int      _tp_build_crc_stream_backend(uint32_t                       magic,
                                             uint32_t                       version,
                                             uint32_t                       len,
                                             const struct tp_flash_backend *backend,
                                             uint32_t                       payload_addr,
                                             uint32_t                      *out_crc);

/* Public Functions ----------------------------------------------------------------------------- */

int tp_save(const struct tp_slot      *slot,
            const struct tp_flash_ops *ops,
            const void                *payload,
            uint32_t                   payload_len)
{
    struct tp_header hdr;

    if (_tp_legacy_write_erase_args_ok(slot, ops) != 0) {
        return -EINVAL;
    }
    if ((payload == NULL) || !_tp_payload_len_is_valid(slot, payload_len) ||
        !_tp_slot_span_is_valid(slot, payload_len)) {
        return -EINVAL;
    }

    hdr.magic   = slot->magic;
    hdr.version = slot->version;
    hdr.len     = payload_len;
    hdr.crc     = _tp_build_crc(hdr.magic, hdr.version, hdr.len, (const uint8_t *)payload);

    if (ops->erase(slot->addr, TP_HEADER_SIZE + payload_len) < 0) {
        return -ETIMEDOUT;
    }
    {
        uint8_t hdr_bytes[sizeof(hdr)];

        _tp_header_encode(hdr_bytes, &hdr);
        if (ops->write(slot->addr, hdr_bytes, sizeof(hdr_bytes)) < 0) {
            return -ETIMEDOUT;
        }
    }
    if (ops->write(slot->addr + sizeof(hdr), (const uint8_t *)payload, payload_len) < 0) {
        return -ETIMEDOUT;
    }

    return 0;
}

int tp_save_backend(const struct tp_slot          *slot,
                    const struct tp_flash_backend *backend,
                    const void                    *payload,
                    uint32_t                       payload_len)
{
    struct tp_header hdr;

    if (_tp_backend_write_erase_args_ok(slot, backend) != 0) {
        return -EINVAL;
    }
    if ((payload == NULL) || !_tp_payload_len_is_valid(slot, payload_len) ||
        !_tp_slot_span_is_valid(slot, payload_len)) {
        return -EINVAL;
    }

    hdr.magic   = slot->magic;
    hdr.version = slot->version;
    hdr.len     = payload_len;
    hdr.crc     = _tp_build_crc(hdr.magic, hdr.version, hdr.len, (const uint8_t *)payload);

    if (_tp_backend_erase(backend, slot->addr, TP_HEADER_SIZE + payload_len) < 0) {
        return -ETIMEDOUT;
    }
    {
        uint8_t hdr_bytes[sizeof(hdr)];

        _tp_header_encode(hdr_bytes, &hdr);
        if (_tp_backend_write(backend, slot->addr, hdr_bytes, sizeof(hdr_bytes)) < 0) {
            return -ETIMEDOUT;
        }
    }
    if (_tp_backend_write(backend,
                          slot->addr + sizeof(hdr),
                          (const uint8_t *)payload,
                          payload_len) < 0) {
        return -ETIMEDOUT;
    }

    return 0;
}

int tp_load(const struct tp_slot      *slot,
            const struct tp_flash_ops *ops,
            void                      *payload,
            uint32_t                   payload_len)
{
    struct tp_header hdr;
    const uint8_t   *mapped_payload;
    uint32_t         computed;

    if (_tp_legacy_read_args_ok(slot, ops) != 0) {
        return -EINVAL;
    }
    if ((payload == NULL) || !_tp_payload_len_is_valid(slot, payload_len)) {
        return -EINVAL;
    }

    {
        uint8_t hdr_bytes[sizeof(hdr)];

        if (ops->read(slot->addr, hdr_bytes, sizeof(hdr_bytes)) < 0) {
            return -ETIMEDOUT;
        }

        hdr.magic   = _tp_u32_decode_le(&hdr_bytes[0]);
        hdr.version = _tp_u32_decode_le(&hdr_bytes[4]);
        hdr.len     = _tp_u32_decode_le(&hdr_bytes[8]);
        hdr.crc     = _tp_u32_decode_le(&hdr_bytes[12]);
    }

    if (hdr.magic == _TP_BLANK_WORD) {
        return TP_EMPTY;
    }
    if (hdr.magic != slot->magic) {
        return -EBADMSG;
    }

    /*
     * Version and length must both match. A length mismatch with the same
     * version indicates a silent layout change that did not receive a version
     * bump, so we still surface -EPROTO and let the caller run migration code.
     */
    if ((hdr.version != slot->version) || (hdr.len != payload_len)) {
        return -EPROTO;
    }

    if (!_tp_slot_span_is_valid(slot, payload_len)) {
        return -EPROTO;
    }

    mapped_payload = _tp_legacy_map_readonly(ops, slot->addr + sizeof(hdr), payload_len);
    if (mapped_payload != NULL) {
        memcpy(payload, mapped_payload, payload_len);
        computed = _tp_build_crc(hdr.magic, hdr.version, hdr.len, mapped_payload);
    } else {
        if (ops->read(slot->addr + sizeof(hdr), (uint8_t *)payload, payload_len) < 0) {
            return -ETIMEDOUT;
        }
        computed = _tp_build_crc(hdr.magic, hdr.version, hdr.len, (const uint8_t *)payload);
    }

    if (computed != hdr.crc) {
        memset(payload, 0, payload_len);
        return -EILSEQ;
    }

    return 0;
}

int tp_load_backend(const struct tp_slot          *slot,
                    const struct tp_flash_backend *backend,
                    void                          *payload,
                    uint32_t                       payload_len)
{
    struct tp_header hdr;
    const uint8_t   *mapped_payload;
    uint32_t         computed;

    if (_tp_backend_read_args_ok(slot, backend) != 0) {
        return -EINVAL;
    }
    if ((payload == NULL) || !_tp_payload_len_is_valid(slot, payload_len)) {
        return -EINVAL;
    }

    {
        uint8_t hdr_bytes[sizeof(hdr)];

        if (_tp_backend_read(backend, slot->addr, hdr_bytes, sizeof(hdr_bytes)) < 0) {
            return -ETIMEDOUT;
        }

        hdr.magic   = _tp_u32_decode_le(&hdr_bytes[0]);
        hdr.version = _tp_u32_decode_le(&hdr_bytes[4]);
        hdr.len     = _tp_u32_decode_le(&hdr_bytes[8]);
        hdr.crc     = _tp_u32_decode_le(&hdr_bytes[12]);
    }

    if (hdr.magic == _TP_BLANK_WORD) {
        return TP_EMPTY;
    }
    if (hdr.magic != slot->magic) {
        return -EBADMSG;
    }

    if ((hdr.version != slot->version) || (hdr.len != payload_len)) {
        return -EPROTO;
    }

    if (!_tp_slot_span_is_valid(slot, payload_len)) {
        return -EPROTO;
    }

    mapped_payload = _tp_backend_map_readonly(backend, slot->addr + sizeof(hdr), payload_len);
    if (mapped_payload != NULL) {
        memcpy(payload, mapped_payload, payload_len);
        computed = _tp_build_crc(hdr.magic, hdr.version, hdr.len, mapped_payload);
    } else {
        if (_tp_backend_read(backend, slot->addr + sizeof(hdr), (uint8_t *)payload, payload_len) <
            0) {
            return -ETIMEDOUT;
        }
        computed = _tp_build_crc(hdr.magic, hdr.version, hdr.len, (const uint8_t *)payload);
    }

    if (computed != hdr.crc) {
        memset(payload, 0, payload_len);
        return -EILSEQ;
    }

    return 0;
}

int tp_load_raw(const struct tp_slot      *slot,
                const struct tp_flash_ops *ops,
                void                      *payload,
                uint32_t                   buf_len)
{
    struct tp_header hdr;
    const uint8_t   *mapped_payload;
    uint32_t         read_len;
    uint32_t         computed = 0U;
    int              crc_rc;

    if (_tp_legacy_read_args_ok(slot, ops) != 0) {
        return -EINVAL;
    }
    if ((payload == NULL) || (buf_len == 0U)) {
        return -EINVAL;
    }

    {
        uint8_t hdr_bytes[sizeof(hdr)];

        if (ops->read(slot->addr, hdr_bytes, sizeof(hdr_bytes)) < 0) {
            return -ETIMEDOUT;
        }

        hdr.magic   = _tp_u32_decode_le(&hdr_bytes[0]);
        hdr.version = _tp_u32_decode_le(&hdr_bytes[4]);
        hdr.len     = _tp_u32_decode_le(&hdr_bytes[8]);
        hdr.crc     = _tp_u32_decode_le(&hdr_bytes[12]);
    }

    if (hdr.magic == _TP_BLANK_WORD) {
        return TP_EMPTY;
    }
    if (hdr.magic != slot->magic) {
        return -EBADMSG;
    }

    if (!_tp_payload_len_is_valid(slot, hdr.len)) {
        return -EPROTO;
    }

    read_len       = (hdr.len < buf_len) ? hdr.len : buf_len;
    mapped_payload = _tp_legacy_map_readonly(ops, slot->addr + sizeof(hdr), hdr.len);

    if (mapped_payload != NULL) {
        memcpy(payload, mapped_payload, read_len);
        computed = _tp_build_crc(hdr.magic, hdr.version, hdr.len, mapped_payload);
    } else {
        if (ops->read(slot->addr + sizeof(hdr), (uint8_t *)payload, read_len) < 0) {
            return -ETIMEDOUT;
        }

        /*
         * Rebuild the CRC using the stored header fields, not slot->version,
         * because migration reads must authenticate the old layout as-written.
         */
        crc_rc = _tp_build_crc_stream_legacy(hdr.magic,
                                             hdr.version,
                                             hdr.len,
                                             ops,
                                             slot->addr + sizeof(struct tp_header),
                                             &computed);
        if (crc_rc < 0) {
            memset(payload, 0, buf_len);
            return crc_rc;
        }
    }

    if (buf_len > hdr.len) {
        memset((uint8_t *)payload + hdr.len, 0, buf_len - hdr.len);
    }

    if (computed != hdr.crc) {
        memset(payload, 0, buf_len);
        return -EILSEQ;
    }

    return 0;
}

int tp_load_raw_backend(const struct tp_slot          *slot,
                        const struct tp_flash_backend *backend,
                        void                          *payload,
                        uint32_t                       buf_len)
{
    struct tp_header hdr;
    const uint8_t   *mapped_payload;
    uint32_t         read_len;
    uint32_t         computed = 0U;
    int              crc_rc;

    if (_tp_backend_read_args_ok(slot, backend) != 0) {
        return -EINVAL;
    }
    if ((payload == NULL) || (buf_len == 0U)) {
        return -EINVAL;
    }

    {
        uint8_t hdr_bytes[sizeof(hdr)];

        if (_tp_backend_read(backend, slot->addr, hdr_bytes, sizeof(hdr_bytes)) < 0) {
            return -ETIMEDOUT;
        }

        hdr.magic   = _tp_u32_decode_le(&hdr_bytes[0]);
        hdr.version = _tp_u32_decode_le(&hdr_bytes[4]);
        hdr.len     = _tp_u32_decode_le(&hdr_bytes[8]);
        hdr.crc     = _tp_u32_decode_le(&hdr_bytes[12]);
    }

    if (hdr.magic == _TP_BLANK_WORD) {
        return TP_EMPTY;
    }
    if (hdr.magic != slot->magic) {
        return -EBADMSG;
    }

    if (!_tp_payload_len_is_valid(slot, hdr.len)) {
        return -EPROTO;
    }

    read_len       = (hdr.len < buf_len) ? hdr.len : buf_len;
    mapped_payload = _tp_backend_map_readonly(backend, slot->addr + sizeof(hdr), hdr.len);

    if (mapped_payload != NULL) {
        memcpy(payload, mapped_payload, read_len);
        computed = _tp_build_crc(hdr.magic, hdr.version, hdr.len, mapped_payload);
    } else {
        if (_tp_backend_read(backend, slot->addr + sizeof(hdr), (uint8_t *)payload, read_len) < 0) {
            return -ETIMEDOUT;
        }

        crc_rc = _tp_build_crc_stream_backend(hdr.magic,
                                              hdr.version,
                                              hdr.len,
                                              backend,
                                              slot->addr + sizeof(struct tp_header),
                                              &computed);
        if (crc_rc < 0) {
            memset(payload, 0, buf_len);
            return crc_rc;
        }
    }

    if (buf_len > hdr.len) {
        memset((uint8_t *)payload + hdr.len, 0, buf_len - hdr.len);
    }

    if (computed != hdr.crc) {
        memset(payload, 0, buf_len);
        return -EILSEQ;
    }

    return 0;
}

int tp_check(const struct tp_slot *slot, const struct tp_flash_ops *ops, struct tp_slot_info *info)
{
    struct tp_header hdr;
    const uint8_t   *mapped_payload;
    uint32_t         computed = 0U;
    int              crc_rc;

    if (info != NULL) {
        info->addr       = (slot != NULL) ? slot->addr : 0U;
        info->magic      = _TP_BLANK_WORD;
        info->version    = 0U;
        info->data_len   = 0U;
        info->stored_crc = 0U;
        info->status     = -ETIMEDOUT;
    }

    if (_tp_legacy_read_args_ok(slot, ops) != 0) {
        if (info != NULL) {
            info->status = -EINVAL;
        }
        return -EINVAL;
    }

    {
        uint8_t hdr_bytes[sizeof(hdr)];

        if (ops->read(slot->addr, hdr_bytes, sizeof(hdr_bytes)) < 0) {
            return -ETIMEDOUT;
        }

        hdr.magic   = _tp_u32_decode_le(&hdr_bytes[0]);
        hdr.version = _tp_u32_decode_le(&hdr_bytes[4]);
        hdr.len     = _tp_u32_decode_le(&hdr_bytes[8]);
        hdr.crc     = _tp_u32_decode_le(&hdr_bytes[12]);
    }

    if (info != NULL) {
        info->magic      = hdr.magic;
        info->version    = hdr.version;
        info->data_len   = hdr.len;
        info->stored_crc = hdr.crc;
    }

    if (hdr.magic == _TP_BLANK_WORD) {
        if (info != NULL) {
            info->status = TP_EMPTY;
        }
        return TP_EMPTY;
    }
    if (hdr.magic != slot->magic) {
        if (info != NULL) {
            info->status = -EBADMSG;
        }
        return -EBADMSG;
    }
    if ((hdr.version != slot->version) || !_tp_payload_len_is_valid(slot, hdr.len)) {
        if (info != NULL) {
            info->status = -EPROTO;
        }
        return -EPROTO;
    }

    mapped_payload = _tp_legacy_map_readonly(ops, slot->addr + sizeof(hdr), hdr.len);
    if (mapped_payload != NULL) {
        computed = _tp_build_crc(hdr.magic, hdr.version, hdr.len, mapped_payload);
    } else {
        crc_rc = _tp_build_crc_stream_legacy(hdr.magic,
                                             hdr.version,
                                             hdr.len,
                                             ops,
                                             slot->addr + sizeof(struct tp_header),
                                             &computed);
        if (crc_rc < 0) {
            if (info != NULL) {
                info->status = crc_rc;
            }
            return crc_rc;
        }
    }

    if (computed != hdr.crc) {
        if (info != NULL) {
            info->status = -EILSEQ;
        }
        return -EILSEQ;
    }

    if (info != NULL) {
        info->status = 0;
    }
    return 0;
}

int tp_check_backend(const struct tp_slot          *slot,
                     const struct tp_flash_backend *backend,
                     struct tp_slot_info           *info)
{
    struct tp_header hdr;
    const uint8_t   *mapped_payload;
    uint32_t         computed = 0U;
    int              crc_rc;

    if (info != NULL) {
        info->addr       = (slot != NULL) ? slot->addr : 0U;
        info->magic      = _TP_BLANK_WORD;
        info->version    = 0U;
        info->data_len   = 0U;
        info->stored_crc = 0U;
        info->status     = -ETIMEDOUT;
    }

    if (_tp_backend_read_args_ok(slot, backend) != 0) {
        if (info != NULL) {
            info->status = -EINVAL;
        }
        return -EINVAL;
    }

    {
        uint8_t hdr_bytes[sizeof(hdr)];

        if (_tp_backend_read(backend, slot->addr, hdr_bytes, sizeof(hdr_bytes)) < 0) {
            return -ETIMEDOUT;
        }

        hdr.magic   = _tp_u32_decode_le(&hdr_bytes[0]);
        hdr.version = _tp_u32_decode_le(&hdr_bytes[4]);
        hdr.len     = _tp_u32_decode_le(&hdr_bytes[8]);
        hdr.crc     = _tp_u32_decode_le(&hdr_bytes[12]);
    }

    if (info != NULL) {
        info->magic      = hdr.magic;
        info->version    = hdr.version;
        info->data_len   = hdr.len;
        info->stored_crc = hdr.crc;
    }

    if (hdr.magic == _TP_BLANK_WORD) {
        if (info != NULL) {
            info->status = TP_EMPTY;
        }
        return TP_EMPTY;
    }
    if (hdr.magic != slot->magic) {
        if (info != NULL) {
            info->status = -EBADMSG;
        }
        return -EBADMSG;
    }
    if ((hdr.version != slot->version) || !_tp_payload_len_is_valid(slot, hdr.len)) {
        if (info != NULL) {
            info->status = -EPROTO;
        }
        return -EPROTO;
    }

    mapped_payload = _tp_backend_map_readonly(backend, slot->addr + sizeof(hdr), hdr.len);
    if (mapped_payload != NULL) {
        computed = _tp_build_crc(hdr.magic, hdr.version, hdr.len, mapped_payload);
    } else {
        crc_rc = _tp_build_crc_stream_backend(hdr.magic,
                                              hdr.version,
                                              hdr.len,
                                              backend,
                                              slot->addr + sizeof(struct tp_header),
                                              &computed);
        if (crc_rc < 0) {
            if (info != NULL) {
                info->status = crc_rc;
            }
            return crc_rc;
        }
    }

    if (computed != hdr.crc) {
        if (info != NULL) {
            info->status = -EILSEQ;
        }
        return -EILSEQ;
    }

    if (info != NULL) {
        info->status = 0;
    }
    return 0;
}

int tp_erase(const struct tp_slot *slot, const struct tp_flash_ops *ops)
{
    if (_tp_legacy_erase_args_ok(slot, ops) != 0) {
        return -EINVAL;
    }

    if (!_tp_slot_span_is_valid(slot, slot->max_len)) {
        return -EINVAL;
    }

    if (ops->erase(slot->addr, TP_HEADER_SIZE + slot->max_len) < 0) {
        return -ETIMEDOUT;
    }
    return 0;
}

int tp_erase_backend(const struct tp_slot *slot, const struct tp_flash_backend *backend)
{
    if (_tp_backend_erase_args_ok(slot, backend) != 0) {
        return -EINVAL;
    }

    if (!_tp_slot_span_is_valid(slot, slot->max_len)) {
        return -EINVAL;
    }

    if (_tp_backend_erase(backend, slot->addr, TP_HEADER_SIZE + slot->max_len) < 0) {
        return -ETIMEDOUT;
    }
    return 0;
}

uint32_t tp_calc_crc(const struct tp_slot *slot, const void *payload, uint32_t payload_len)
{
    if ((slot == NULL) || (payload == NULL) || (payload_len == 0U) ||
        (payload_len > slot->max_len)) {
        return 0U;
    }

    return _tp_build_crc(slot->magic, slot->version, payload_len, (const uint8_t *)payload);
}

const char *tp_strerr(int rc)
{
    switch (rc) {
    case 0:
        return "TP_OK";
    case TP_EMPTY:
        return "TP_EMPTY";
    case -ETIMEDOUT:
        return "TP_ERR_HW";
    case -EILSEQ:
        return "TP_ERR_CRC";
    case -EBADMSG:
        return "TP_ERR_MAGIC";
    case -EPROTO:
        return "TP_ERR_PROTO";
    case -EINVAL:
        return "TP_ERR_INVALID_ARG";
    default:
        return "TP_ERR_UNKNOWN";
    }
}

/* Private Functions ---------------------------------------------------------------------------- */

static int _tp_legacy_base_args_ok(const struct tp_slot *slot, const struct tp_flash_ops *ops)
{
    if ((slot == NULL) || (ops == NULL)) {
        return -1;
    }

    if (!_tp_slot_span_is_valid(slot, 0U)) {
        return -1;
    }

    return 0;
}

static int _tp_legacy_read_args_ok(const struct tp_slot *slot, const struct tp_flash_ops *ops)
{
    if (_tp_legacy_base_args_ok(slot, ops) != 0) {
        return -1;
    }

    return (ops->read != NULL) ? 0 : -1;
}

static int _tp_legacy_write_erase_args_ok(const struct tp_slot      *slot,
                                          const struct tp_flash_ops *ops)
{
    if (_tp_legacy_base_args_ok(slot, ops) != 0) {
        return -1;
    }

    return ((ops->write != NULL) && (ops->erase != NULL)) ? 0 : -1;
}

static int _tp_legacy_erase_args_ok(const struct tp_slot *slot, const struct tp_flash_ops *ops)
{
    if (_tp_legacy_base_args_ok(slot, ops) != 0) {
        return -1;
    }

    return (ops->erase != NULL) ? 0 : -1;
}

static int _tp_backend_base_args_ok(const struct tp_slot          *slot,
                                    const struct tp_flash_backend *backend)
{
    if ((slot == NULL) || (backend == NULL) || (backend->ops == NULL)) {
        return -1;
    }

    if (!_tp_slot_span_is_valid(slot, 0U)) {
        return -1;
    }

    return 0;
}

static int _tp_backend_read_args_ok(const struct tp_slot          *slot,
                                    const struct tp_flash_backend *backend)
{
    if (_tp_backend_base_args_ok(slot, backend) != 0) {
        return -1;
    }

    return (backend->ops->read != NULL) ? 0 : -1;
}

static int _tp_backend_write_erase_args_ok(const struct tp_slot          *slot,
                                           const struct tp_flash_backend *backend)
{
    if (_tp_backend_base_args_ok(slot, backend) != 0) {
        return -1;
    }

    return ((backend->ops->write != NULL) && (backend->ops->erase != NULL)) ? 0 : -1;
}

static int _tp_backend_erase_args_ok(const struct tp_slot          *slot,
                                     const struct tp_flash_backend *backend)
{
    if (_tp_backend_base_args_ok(slot, backend) != 0) {
        return -1;
    }

    return (backend->ops->erase != NULL) ? 0 : -1;
}

static bool _tp_payload_len_is_valid(const struct tp_slot *slot, uint32_t payload_len)
{
    if (slot == NULL) {
        return false;
    }

    return (payload_len != 0U) && (payload_len <= slot->max_len);
}

static bool _tp_slot_span_is_valid(const struct tp_slot *slot, uint32_t payload_len)
{
    uint32_t end_addr;

    if ((slot == NULL) || (payload_len > slot->max_len)) {
        return false;
    }

    if (slot->addr > (UINT32_MAX - TP_HEADER_SIZE)) {
        return false;
    }

    end_addr = slot->addr + TP_HEADER_SIZE;
    if (end_addr > (UINT32_MAX - payload_len)) {
        return false;
    }

    return true;
}

static int _tp_backend_read(const struct tp_flash_backend *backend,
                            uint32_t                       addr,
                            uint8_t                       *buf,
                            uint32_t                       size)
{
    if ((backend == NULL) || (backend->ops == NULL) || (backend->ops->read == NULL)) {
        return -EINVAL;
    }

    return backend->ops->read(backend->user_ctx, addr, buf, size);
}

static int _tp_backend_write(const struct tp_flash_backend *backend,
                             uint32_t                       addr,
                             const uint8_t                 *buf,
                             uint32_t                       size)
{
    if ((backend == NULL) || (backend->ops == NULL) || (backend->ops->write == NULL)) {
        return -EINVAL;
    }

    return backend->ops->write(backend->user_ctx, addr, buf, size);
}

static int _tp_backend_erase(const struct tp_flash_backend *backend, uint32_t addr, uint32_t size)
{
    if ((backend == NULL) || (backend->ops == NULL) || (backend->ops->erase == NULL)) {
        return -EINVAL;
    }

    return backend->ops->erase(backend->user_ctx, addr, size);
}

static const uint8_t *_tp_legacy_map_readonly(const struct tp_flash_ops *ops,
                                              uint32_t                   addr,
                                              uint32_t                   size)
{
    if ((ops == NULL) || (ops->map_readonly == NULL)) {
        return NULL;
    }

    return (const uint8_t *)ops->map_readonly(addr, size);
}

static const uint8_t *_tp_backend_map_readonly(const struct tp_flash_backend *backend,
                                               uint32_t                       addr,
                                               uint32_t                       size)
{
    if ((backend == NULL) || (backend->ops == NULL) || (backend->ops->map_readonly == NULL)) {
        return NULL;
    }

    return (const uint8_t *)backend->ops->map_readonly(backend->user_ctx, addr, size);
}

/**
 * @brief  Compute the stored CRC-32 state over the header prefix and payload.
 *
 * CRC input is streamed as two regions:
 *   Region 1: [magic(4)] + [version(4)] + [len(4)]
 *   Region 2: payload body
 *
 * This function accepts explicit field values so both save and migration paths
 * can share the same CRC builder without aliasing issues.
 */
static uint32_t _tp_build_crc(uint32_t       magic,
                              uint32_t       version,
                              uint32_t       len,
                              const uint8_t *payload)
{
    uint8_t  prefix[12];
    uint32_t crc;

    _tp_header_prefix_encode(prefix, magic, version, len);

    crc = crc32_ieee(CRC32_IEEE_INIT, prefix, sizeof(prefix));
    crc = crc32_ieee(crc, payload, (size_t)len);

    return crc;
}

static int _tp_build_crc_stream_legacy(uint32_t                   magic,
                                       uint32_t                   version,
                                       uint32_t                   len,
                                       const struct tp_flash_ops *ops,
                                       uint32_t                   payload_addr,
                                       uint32_t                  *out_crc)
{
    uint8_t  prefix[12];
    uint8_t  chunk[_TP_CRC_STREAM_CHUNK_SIZE];
    uint32_t crc;
    uint32_t remaining;
    uint32_t addr;

    if ((ops == NULL) || (ops->read == NULL) || (out_crc == NULL)) {
        return -EINVAL;
    }

    _tp_header_prefix_encode(prefix, magic, version, len);

    crc       = crc32_ieee(CRC32_IEEE_INIT, prefix, sizeof(prefix));
    remaining = len;
    addr      = payload_addr;

    while (remaining > 0U) {
        uint32_t chunk_len =
            (remaining > (uint32_t)sizeof(chunk)) ? (uint32_t)sizeof(chunk) : remaining;

        if (ops->read(addr, chunk, chunk_len) < 0) {
            return -ETIMEDOUT;
        }

        crc = crc32_ieee(crc, chunk, (size_t)chunk_len);
        addr += chunk_len;
        remaining -= chunk_len;
    }

    *out_crc = crc;
    return 0;
}

static int _tp_build_crc_stream_backend(uint32_t                       magic,
                                        uint32_t                       version,
                                        uint32_t                       len,
                                        const struct tp_flash_backend *backend,
                                        uint32_t                       payload_addr,
                                        uint32_t                      *out_crc)
{
    uint8_t  prefix[12];
    uint8_t  chunk[_TP_CRC_STREAM_CHUNK_SIZE];
    uint32_t crc;
    uint32_t remaining;
    uint32_t addr;

    if ((backend == NULL) || (backend->ops == NULL) || (backend->ops->read == NULL) ||
        (out_crc == NULL)) {
        return -EINVAL;
    }

    _tp_header_prefix_encode(prefix, magic, version, len);

    crc       = crc32_ieee(CRC32_IEEE_INIT, prefix, sizeof(prefix));
    remaining = len;
    addr      = payload_addr;

    while (remaining > 0U) {
        uint32_t chunk_len =
            (remaining > (uint32_t)sizeof(chunk)) ? (uint32_t)sizeof(chunk) : remaining;

        if (_tp_backend_read(backend, addr, chunk, chunk_len) < 0) {
            return -ETIMEDOUT;
        }

        crc = crc32_ieee(crc, chunk, (size_t)chunk_len);
        addr += chunk_len;
        remaining -= chunk_len;
    }

    *out_crc = crc;
    return 0;
}

static void _tp_header_prefix_encode(uint8_t *dst, uint32_t magic, uint32_t version, uint32_t len)
{
    dst[0]  = (uint8_t)(magic & 0xFFU);
    dst[1]  = (uint8_t)((magic >> 8U) & 0xFFU);
    dst[2]  = (uint8_t)((magic >> 16U) & 0xFFU);
    dst[3]  = (uint8_t)((magic >> 24U) & 0xFFU);
    dst[4]  = (uint8_t)(version & 0xFFU);
    dst[5]  = (uint8_t)((version >> 8U) & 0xFFU);
    dst[6]  = (uint8_t)((version >> 16U) & 0xFFU);
    dst[7]  = (uint8_t)((version >> 24U) & 0xFFU);
    dst[8]  = (uint8_t)(len & 0xFFU);
    dst[9]  = (uint8_t)((len >> 8U) & 0xFFU);
    dst[10] = (uint8_t)((len >> 16U) & 0xFFU);
    dst[11] = (uint8_t)((len >> 24U) & 0xFFU);
}

static void _tp_header_encode(uint8_t *dst, const struct tp_header *hdr)
{
    _tp_header_prefix_encode(dst, hdr->magic, hdr->version, hdr->len);
    dst[12] = (uint8_t)(hdr->crc & 0xFFU);
    dst[13] = (uint8_t)((hdr->crc >> 8U) & 0xFFU);
    dst[14] = (uint8_t)((hdr->crc >> 16U) & 0xFFU);
    dst[15] = (uint8_t)((hdr->crc >> 24U) & 0xFFU);
}

static uint32_t _tp_u32_decode_le(const uint8_t *src)
{
    return ((uint32_t)src[0]) | ((uint32_t)src[1] << 8U) | ((uint32_t)src[2] << 16U) |
           ((uint32_t)src[3] << 24U);
}

/* ---------------------------------------- end of file ----------------------------------------- */
