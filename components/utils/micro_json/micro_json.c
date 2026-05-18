/**
 * @file    micro_json.c
 * @brief   Zero dynamic memory JSON parser implementation
 * @version v1.1.0
 * @date    2026.04.24
 *
 * Implementation role
 * -------------------
 * - pairs with `micro_json.h`
 * - owns the single-pass tokenizer, dry-run counting mode, and slice-based
 *   value extraction helpers
 *
 * Key collaborators
 * -----------------
 * - `micro_json.h`: caller-facing buffer, token, and error contract
 */

/* Includes ------------------------------------------------------------------------------------- */
#include "micro_json.h"
#include <errno.h>
#include <string.h>

/* Private Helpers ------------------------------------------------------------------------------ */

#define _JSON_VALIDATOR_MAX_DEPTH (64U)

/**
 * @brief  Allocate the next token slot.
 *
 * In dry-run mode (`tokens == NULL`) the parser still advances `toknext` so
 * callers can learn the required token count without materializing tokens.
 *
 * @param  parser      Parser context.
 * @param  tokens      Token array, or NULL for dry-run mode.
 * @param  num_tokens  Capacity of @p tokens.
 *
 * @return Token index on success, or `-ENOSPC` when the physical token array
 *         is full.
 */
static int _json_alloc_token(struct json_parser *parser,
                             struct json_token  *tokens,
                             size_t              num_tokens)
{
    int idx = parser->toknext;

    if (tokens == NULL) {
        parser->toknext++;
        return idx;
    }

    if ((size_t)parser->toknext >= num_tokens) {
        return -ENOSPC;
    }

    parser->toknext++;

    tokens[idx].type   = JSON_TYPE_UNDEFINED;
    tokens[idx].start  = -1;
    tokens[idx].end    = -1;
    tokens[idx].size   = 0;
    tokens[idx].parent = -1;

    return idx;
}

/**
 * @brief  Fill one token slot with the parsed slice metadata.
 *
 * Safe no-op when @p tokens is NULL or @p idx is invalid.
 */
static void _json_fill_token(struct json_token *tokens,
                             int                idx,
                             enum json_type     type,
                             int                start,
                             int                end)
{
    if ((tokens == NULL) || (idx < 0)) {
        return;
    }

    tokens[idx].type  = type;
    tokens[idx].start = start;
    tokens[idx].end   = end;
    tokens[idx].size  = 0;
}

/**
 * @brief  Parse one JSON string token.
 *
 * On entry `parser->pos` must point at the opening quote. On success it points
 * one byte past the closing quote.
 *
 * Escape handling validates `\\`, `\"`, `\/`, `\b`, `\f`, `\n`, `\r`,
 * `\t`, and `\uXXXX`. Unicode escapes are validated but intentionally not
 * decoded; the token still references the original source slice.
 */
static int _json_parse_string(struct json_parser *parser,
                              const char         *js,
                              size_t              js_len,
                              struct json_token  *tokens,
                              size_t              num_tokens)
{
    int    tok_idx;
    size_t start_pos;

    tok_idx = _json_alloc_token(parser, tokens, num_tokens);
    if (tok_idx < 0) {
        return -ENOSPC;
    }

    start_pos = parser->pos;
    parser->pos++;

    while (parser->pos < js_len) {
        char c = js[parser->pos];

        if (c == '"') {
            _json_fill_token(tokens,
                             tok_idx,
                             JSON_TYPE_STRING,
                             (int)(start_pos + 1U),
                             (int)parser->pos);
            if ((tokens != NULL) && (tok_idx >= 0)) {
                tokens[tok_idx].parent = parser->toksuper;
            }

            parser->pos++;
            return 0;
        }

        if (c == '\\') {
            parser->pos++;
            if (parser->pos >= js_len) {
                return -EBADMSG;
            }

            switch (js[parser->pos]) {
            case '"':
            case '\\':
            case '/':
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't':
                break;

            case 'u':
                for (int h = 0; h < 4; h++) {
                    bool is_hex;
                    char hc;

                    parser->pos++;
                    if (parser->pos >= js_len) {
                        return -EBADMSG;
                    }

                    hc = js[parser->pos];
                    is_hex = ((hc >= '0' && hc <= '9') || (hc >= 'a' && hc <= 'f') ||
                              (hc >= 'A' && hc <= 'F'));
                    if (!is_hex) {
                        return -EINVAL;
                    }
                }
                break;

            default:
                return -EINVAL;
            }
        } else {
            if ((unsigned char)c < 0x20U) {
                return -EINVAL;
            }
        }

        parser->pos++;
    }

    return -EBADMSG;
}

/**
 * @brief  Parse one primitive token: number, boolean, or null.
 */
static int _json_parse_primitive(struct json_parser *parser,
                                 const char         *js,
                                 size_t              js_len,
                                 struct json_token  *tokens,
                                 size_t              num_tokens)
{
    int    tok_idx;
    size_t start_pos;

    tok_idx = _json_alloc_token(parser, tokens, num_tokens);
    if (tok_idx < 0) {
        return -ENOSPC;
    }

    start_pos = parser->pos;

    while (parser->pos < js_len) {
        char c = js[parser->pos];

        switch (c) {
        case '\t':
        case '\r':
        case '\n':
        case ' ':
        case ',':
        case ']':
        case '}':
            goto done;

        default:
            break;
        }

        if (((unsigned char)c < 0x20U) || ((unsigned char)c == 0x7FU)) {
            return -EINVAL;
        }

        parser->pos++;
    }

done:
    _json_fill_token(tokens, tok_idx, JSON_TYPE_PRIMITIVE, (int)start_pos, (int)parser->pos);
    if ((tokens != NULL) && (tok_idx >= 0)) {
        tokens[tok_idx].parent = parser->toksuper;
    }

    return 0;
}

enum _json_validator_frame_kind {
    _JSON_FRAME_OBJECT = 0,
    _JSON_FRAME_ARRAY,
};

enum _json_validator_expect {
    _JSON_EXPECT_ROOT_VALUE = 0,
    _JSON_EXPECT_ROOT_DONE,
    _JSON_EXPECT_OBJECT_FIRST_KEY_OR_END,
    _JSON_EXPECT_OBJECT_KEY,
    _JSON_EXPECT_OBJECT_COLON,
    _JSON_EXPECT_OBJECT_VALUE,
    _JSON_EXPECT_OBJECT_COMMA_OR_END,
    _JSON_EXPECT_ARRAY_FIRST_VALUE_OR_END,
    _JSON_EXPECT_ARRAY_VALUE,
    _JSON_EXPECT_ARRAY_COMMA_OR_END,
};

struct _json_validator_frame {
    enum _json_validator_frame_kind kind;
};

static bool _json_is_space(char c)
{
    return (c == ' ') || (c == '\t') || (c == '\r') || (c == '\n');
}

static void _json_skip_ws(const char *js, size_t js_len, size_t *pos)
{
    while ((*pos < js_len) && _json_is_space(js[*pos])) {
        (*pos)++;
    }
}

static int _json_validate_string_syntax(const char *js, size_t js_len, size_t *pos)
{
    if ((*pos >= js_len) || (js[*pos] != '"')) {
        return -EINVAL;
    }

    (*pos)++;

    while (*pos < js_len) {
        char c = js[*pos];

        if (c == '"') {
            (*pos)++;
            return 0;
        }

        if (c == '\\') {
            (*pos)++;
            if (*pos >= js_len) {
                return -EBADMSG;
            }

            switch (js[*pos]) {
            case '"':
            case '\\':
            case '/':
            case 'b':
            case 'f':
            case 'n':
            case 'r':
            case 't':
                (*pos)++;
                break;

            case 'u': {
                int hex_index;

                for (hex_index = 0; hex_index < 4; ++hex_index) {
                    char hc;

                    (*pos)++;
                    if (*pos >= js_len) {
                        return -EBADMSG;
                    }

                    hc = js[*pos];
                    if (!((hc >= '0' && hc <= '9') || (hc >= 'a' && hc <= 'f') ||
                          (hc >= 'A' && hc <= 'F'))) {
                        return -EINVAL;
                    }
                }

                (*pos)++;
                break;
            }

            default:
                return -EINVAL;
            }

            continue;
        }

        if ((unsigned char)c < 0x20U) {
            return -EINVAL;
        }

        (*pos)++;
    }

    return -EBADMSG;
}

static int _json_validate_number_syntax(const char *js, size_t js_len, size_t *pos)
{
    size_t cursor = *pos;

    if (cursor >= js_len) {
        return -EINVAL;
    }

    if (js[cursor] == '-') {
        cursor++;
        if (cursor >= js_len) {
            return -EINVAL;
        }
    }

    if (js[cursor] == '0') {
        cursor++;
        if ((cursor < js_len) && (js[cursor] >= '0') && (js[cursor] <= '9')) {
            return -EINVAL;
        }
    } else if ((js[cursor] >= '1') && (js[cursor] <= '9')) {
        do {
            cursor++;
        } while ((cursor < js_len) && (js[cursor] >= '0') && (js[cursor] <= '9'));
    } else {
        return -EINVAL;
    }

    if ((cursor < js_len) && (js[cursor] == '.')) {
        cursor++;
        if ((cursor >= js_len) || (js[cursor] < '0') || (js[cursor] > '9')) {
            return -EINVAL;
        }
        while ((cursor < js_len) && (js[cursor] >= '0') && (js[cursor] <= '9')) {
            cursor++;
        }
    }

    if ((cursor < js_len) && ((js[cursor] == 'e') || (js[cursor] == 'E'))) {
        cursor++;
        if ((cursor < js_len) && ((js[cursor] == '+') || (js[cursor] == '-'))) {
            cursor++;
        }
        if ((cursor >= js_len) || (js[cursor] < '0') || (js[cursor] > '9')) {
            return -EINVAL;
        }
        while ((cursor < js_len) && (js[cursor] >= '0') && (js[cursor] <= '9')) {
            cursor++;
        }
    }

    *pos = cursor;
    return 0;
}

static int _json_validate_primitive_syntax(const char *js, size_t js_len, size_t *pos)
{
    size_t cursor = *pos;

    if (cursor >= js_len) {
        return -EINVAL;
    }

    if ((js_len - cursor) >= 4U && (strncmp(&js[cursor], "true", 4U) == 0)) {
        *pos = cursor + 4U;
        return 0;
    }
    if ((js_len - cursor) >= 5U && (strncmp(&js[cursor], "false", 5U) == 0)) {
        *pos = cursor + 5U;
        return 0;
    }
    if ((js_len - cursor) >= 4U && (strncmp(&js[cursor], "null", 4U) == 0)) {
        *pos = cursor + 4U;
        return 0;
    }

    return _json_validate_number_syntax(js, js_len, pos);
}

static enum _json_validator_expect _json_finish_value(
    const struct _json_validator_frame *stack,
    size_t                              depth)
{
    if (depth == 0U) {
        return _JSON_EXPECT_ROOT_DONE;
    }

    return (stack[depth - 1U].kind == _JSON_FRAME_OBJECT) ? _JSON_EXPECT_OBJECT_COMMA_OR_END
                                                           : _JSON_EXPECT_ARRAY_COMMA_OR_END;
}

static int _json_validate_document_syntax(const char *js, size_t js_len)
{
    struct _json_validator_frame stack[_JSON_VALIDATOR_MAX_DEPTH];
    enum _json_validator_expect  expect = _JSON_EXPECT_ROOT_VALUE;
    size_t                       depth  = 0U;
    size_t                       pos    = 0U;

    while (true) {
        _json_skip_ws(js, js_len, &pos);

        if (pos >= js_len) {
            return (expect == _JSON_EXPECT_ROOT_DONE) ? 0 : -EBADMSG;
        }

        switch (expect) {
        case _JSON_EXPECT_ROOT_VALUE:
        case _JSON_EXPECT_OBJECT_VALUE:
        case _JSON_EXPECT_ARRAY_FIRST_VALUE_OR_END:
        case _JSON_EXPECT_ARRAY_VALUE:
            if ((expect == _JSON_EXPECT_ARRAY_FIRST_VALUE_OR_END) && (js[pos] == ']')) {
                pos++;
                if (depth == 0U || stack[depth - 1U].kind != _JSON_FRAME_ARRAY) {
                    return -EINVAL;
                }
                depth--;
                expect = _json_finish_value(stack, depth);
                break;
            }

            if (js[pos] == '{') {
                if (depth >= _JSON_VALIDATOR_MAX_DEPTH) {
                    return -EOVERFLOW;
                }
                stack[depth].kind = _JSON_FRAME_OBJECT;
                depth++;
                pos++;
                expect = _JSON_EXPECT_OBJECT_FIRST_KEY_OR_END;
                break;
            }

            if (js[pos] == '[') {
                if (depth >= _JSON_VALIDATOR_MAX_DEPTH) {
                    return -EOVERFLOW;
                }
                stack[depth].kind = _JSON_FRAME_ARRAY;
                depth++;
                pos++;
                expect = _JSON_EXPECT_ARRAY_FIRST_VALUE_OR_END;
                break;
            }

            if (js[pos] == '"') {
                int rc = _json_validate_string_syntax(js, js_len, &pos);

                if (rc < 0) {
                    return rc;
                }
                expect = _json_finish_value(stack, depth);
                break;
            }

            if ((js[pos] == '-') || (js[pos] == 't') || (js[pos] == 'f') || (js[pos] == 'n') ||
                ((js[pos] >= '0') && (js[pos] <= '9'))) {
                int rc = _json_validate_primitive_syntax(js, js_len, &pos);

                if (rc < 0) {
                    return rc;
                }
                expect = _json_finish_value(stack, depth);
                break;
            }

            return -EINVAL;

        case _JSON_EXPECT_OBJECT_FIRST_KEY_OR_END:
            if (js[pos] == '}') {
                pos++;
                if (depth == 0U || stack[depth - 1U].kind != _JSON_FRAME_OBJECT) {
                    return -EINVAL;
                }
                depth--;
                expect = _json_finish_value(stack, depth);
                break;
            }
            /* fall through */

        case _JSON_EXPECT_OBJECT_KEY:

            if (js[pos] != '"') {
                return -EINVAL;
            }

            if (_json_validate_string_syntax(js, js_len, &pos) < 0) {
                return -EINVAL;
            }
            expect = _JSON_EXPECT_OBJECT_COLON;
            break;

        case _JSON_EXPECT_OBJECT_COLON:
            if (js[pos] != ':') {
                return -EINVAL;
            }
            pos++;
            expect = _JSON_EXPECT_OBJECT_VALUE;
            break;

        case _JSON_EXPECT_OBJECT_COMMA_OR_END:
            if (js[pos] == ',') {
                pos++;
                expect = _JSON_EXPECT_OBJECT_KEY;
                break;
            }
            if (js[pos] == '}') {
                pos++;
                if (depth == 0U || stack[depth - 1U].kind != _JSON_FRAME_OBJECT) {
                    return -EINVAL;
                }
                depth--;
                expect = _json_finish_value(stack, depth);
                break;
            }
            return -EINVAL;

        case _JSON_EXPECT_ARRAY_COMMA_OR_END:
            if (js[pos] == ',') {
                pos++;
                expect = _JSON_EXPECT_ARRAY_VALUE;
                break;
            }
            if (js[pos] == ']') {
                pos++;
                if (depth == 0U || stack[depth - 1U].kind != _JSON_FRAME_ARRAY) {
                    return -EINVAL;
                }
                depth--;
                expect = _json_finish_value(stack, depth);
                break;
            }
            return -EINVAL;

        case _JSON_EXPECT_ROOT_DONE:
            return -EINVAL;
        }
    }
}

/* Public Core API ------------------------------------------------------------------------------ */

void json_parser_init(struct json_parser *parser)
{
    if (parser == NULL) {
        return;
    }

    parser->pos      = 0U;
    parser->toknext  = 0;
    parser->toksuper = -1;
}

int json_parse(struct json_parser *parser,
               const char         *js,
               size_t              js_len,
               struct json_token  *tokens,
               size_t              num_tokens)
{
    int ret;

    if ((parser == NULL) || (js == NULL)) {
        return -EINVAL;
    }
    ret = _json_validate_document_syntax(js, js_len);
    if (ret < 0) {
        return ret;
    }

    while (parser->pos < js_len) {
        char c = js[parser->pos];

        switch (c) {
        case '{':
        case '[': {
            int tok_idx = _json_alloc_token(parser, tokens, num_tokens);

            if (tok_idx < 0) {
                return -ENOSPC;
            }

            if ((parser->toksuper >= 0) && (tokens != NULL)) {
                tokens[tok_idx].parent = parser->toksuper;
                tokens[parser->toksuper].size++;
            }

            _json_fill_token(tokens,
                             tok_idx,
                             (c == '{') ? JSON_TYPE_OBJECT : JSON_TYPE_ARRAY,
                             (int)parser->pos,
                             -1);

            parser->toksuper = tok_idx;
            parser->pos++;
            break;
        }

        case '}':
        case ']': {
            enum json_type expected = (c == '}') ? JSON_TYPE_OBJECT : JSON_TYPE_ARRAY;
            int            i        = parser->toknext - 1;

            for (; i >= 0; i--) {
                if (tokens != NULL) {
                    if ((tokens[i].type == JSON_TYPE_OBJECT) || (tokens[i].type == JSON_TYPE_ARRAY)) {
                        if (tokens[i].end == -1) {
                            break;
                        }
                    }
                } else {
                    i = parser->toksuper;
                    break;
                }
            }

            if (i < 0) {
                return -EINVAL;
            }

            if (tokens != NULL) {
                if (tokens[i].type != expected) {
                    return -EINVAL;
                }

                tokens[i].end = (int)(parser->pos + 1U);
            }

            parser->toksuper = (tokens != NULL) ? tokens[i].parent : (i - 1);
            parser->pos++;
            break;
        }

        case '"':
            ret = _json_parse_string(parser, js, js_len, tokens, num_tokens);
            if (ret < 0) {
                return ret;
            }

            if ((parser->toksuper >= 0) && (tokens != NULL)) {
                tokens[parser->toksuper].size++;
            }

            if ((parser->toksuper >= 0) && (tokens != NULL)) {
                if (tokens[parser->toksuper].type == JSON_TYPE_OBJECT) {
                    parser->toksuper = parser->toknext - 1;
                }
            }
            break;

        case ':':
            parser->pos++;
            break;

        case ',':
            if ((parser->toksuper >= 0) && (tokens != NULL)) {
                int cur = parser->toksuper;

                if ((tokens[cur].type == JSON_TYPE_STRING) && (tokens[cur].end != -1)) {
                    parser->toksuper = tokens[cur].parent;
                }
            }
            parser->pos++;
            break;

        case '\t':
        case '\r':
        case '\n':
        case ' ':
            parser->pos++;
            break;

        default: {
            bool valid_start = ((c >= '0' && c <= '9') || (c == '-') || (c == 't') ||
                                (c == 'f') || (c == 'n'));

            if (!valid_start) {
                return -EINVAL;
            }

            ret = _json_parse_primitive(parser, js, js_len, tokens, num_tokens);
            if (ret < 0) {
                return ret;
            }

            if ((parser->toksuper >= 0) && (tokens != NULL)) {
                int cur;

                tokens[parser->toksuper].size++;
                cur = parser->toksuper;

                if ((tokens[cur].type == JSON_TYPE_STRING) && (tokens[cur].end != -1)) {
                    parser->toksuper = tokens[cur].parent;
                }
            }
            break;
        }
        }
    }

    if (tokens != NULL) {
        for (int i = parser->toknext - 1; i >= 0; i--) {
            if ((tokens[i].type == JSON_TYPE_OBJECT) || (tokens[i].type == JSON_TYPE_ARRAY)) {
                if (tokens[i].end == -1) {
                    return -EBADMSG;
                }
            }
        }
    }

    return parser->toknext;
}

/* Value Extraction Helpers --------------------------------------------------------------------- */

bool json_token_streq(const char *js, const struct json_token *tok, const char *str)
{
    int tok_len;

    if ((js == NULL) || (tok == NULL) || (str == NULL)) {
        return false;
    }
    if (tok->type != JSON_TYPE_STRING) {
        return false;
    }

    tok_len = json_token_len(tok);
    if (tok_len < 0) {
        return false;
    }

    return (strncmp(js + tok->start, str, (size_t)tok_len) == 0) && (str[tok_len] == '\0');
}

float json_get_float(const char *js, const struct json_token *tok)
{
    const char *p;
    const char *end;
    float       sign = 1.0f;
    float       integer_part = 0.0f;
    float       frac_part = 0.0f;
    float       frac_divisor = 1.0f;
    int32_t     exp_val = 0;
    bool        exp_neg = false;
    float       result;

    if ((js == NULL) || (tok == NULL)) {
        return 0.0f;
    }
    if (tok->type != JSON_TYPE_PRIMITIVE) {
        return 0.0f;
    }

    p   = js + tok->start;
    end = js + tok->end;

    if (p >= end) {
        return 0.0f;
    }

    /* Step 1: sign. */
    if (*p == '-') {
        sign = -1.0f;
        p++;
    } else if (*p == '+') {
        p++;
    }

    /* Step 2: integer part. */
    while ((p < end) && (*p >= '0') && (*p <= '9')) {
        integer_part = integer_part * 10.0f + (float)(*p - '0');
        p++;
    }

    /* Step 3: fractional part. */
    if ((p < end) && (*p == '.')) {
        p++;
        while ((p < end) && (*p >= '0') && (*p <= '9')) {
            frac_part = frac_part * 10.0f + (float)(*p - '0');
            frac_divisor *= 10.0f;
            p++;
        }
    }

    /* Step 4: exponent. */
    if ((p < end) && ((*p == 'e') || (*p == 'E'))) {
        p++;
        if ((p < end) && (*p == '-')) {
            exp_neg = true;
            p++;
        } else if ((p < end) && (*p == '+')) {
            p++;
        }

        while ((p < end) && (*p >= '0') && (*p <= '9')) {
            /*
             * Cap exponent accumulation before the multiply so absurdly long
             * exponents cannot overflow the signed integer accumulator.
             */
            if (exp_val < 1000) {
                exp_val = exp_val * 10 + (int32_t)(*p - '0');
            }
            p++;
        }
    }

    /* Step 5: assemble. */
    result = sign * (integer_part + frac_part / frac_divisor);

    /*
     * Apply the exponent via repeated multiply or divide by 10. Clamping keeps
     * the loop bounded while preserving float32 behavior for practical inputs.
     */
    if (exp_val > 38) {
        exp_val = 38;
    }
    if (!exp_neg) {
        for (int32_t i = 0; i < exp_val; i++) {
            result *= 10.0f;
        }
    } else {
        for (int32_t i = 0; i < exp_val; i++) {
            result /= 10.0f;
        }
    }

    return result;
}

int32_t json_get_int(const char *js, const struct json_token *tok)
{
    const char         *p;
    const char         *end;
    bool                is_neg = false;
    static const int32_t LIMIT = INT32_MAX / 10;
    static const int32_t LIMIT_LAST = INT32_MAX % 10;
    int32_t             value = 0;

    if ((js == NULL) || (tok == NULL)) {
        return 0;
    }
    if (tok->type != JSON_TYPE_PRIMITIVE) {
        return 0;
    }

    p   = js + tok->start;
    end = js + tok->end;

    if (p >= end) {
        return 0;
    }

    if (*p == '-') {
        is_neg = true;
        p++;
    } else if (*p == '+') {
        p++;
    }

    /*
     * Guard the multiplication so signed overflow never occurs while parsing.
     * On overflow, saturate to the nearest representable endpoint.
     */
    while ((p < end) && (*p >= '0') && (*p <= '9')) {
        int32_t digit = (int32_t)(*p - '0');

        if ((value > LIMIT) || ((value == LIMIT) && (digit > LIMIT_LAST))) {
            return is_neg ? INT32_MIN : INT32_MAX;
        }
        value = value * 10 + digit;
        p++;
    }

    return is_neg ? -value : value;
}

bool json_get_bool(const char *js, const struct json_token *tok, bool *out)
{
    int         len;
    const char *p;

    if ((js == NULL) || (tok == NULL) || (out == NULL)) {
        return false;
    }
    if (tok->type != JSON_TYPE_PRIMITIVE) {
        return false;
    }

    len = json_token_len(tok);
    p   = js + tok->start;

    if ((len == 4) && (p[0] == 't') && (p[1] == 'r') && (p[2] == 'u') && (p[3] == 'e')) {
        *out = true;
        return true;
    }
    if ((len == 5) && (p[0] == 'f') && (p[1] == 'a') && (p[2] == 'l') && (p[3] == 's') &&
        (p[4] == 'e')) {
        *out = false;
        return true;
    }

    return false;
}

int json_token_copy(const char *js, const struct json_token *tok, char *buf, size_t buf_len)
{
    int tok_len;

    if ((js == NULL) || (tok == NULL) || (buf == NULL) || (buf_len == 0U)) {
        return -EINVAL;
    }
    if (tok->type != JSON_TYPE_STRING) {
        return -EINVAL;
    }

    tok_len = json_token_len(tok);
    if (tok_len < 0) {
        return -EINVAL;
    }

    if ((size_t)tok_len >= buf_len) {
        return -ENOSPC;
    }

    memcpy(buf, js + tok->start, (size_t)tok_len);
    buf[tok_len] = '\0';

    return tok_len;
}

/* ---------------------------------------- end of file ----------------------------------------- */
