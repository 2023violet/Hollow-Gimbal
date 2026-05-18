/**
 * @file    micro_json.h
 * @brief   Zero-copy JSON tokenizer with caller-owned token storage
 * @version v1.1.0
 * @date    2026.04.24
 *
 * Design overview
 * ---------------
 * `micro_json` is a single-pass, zero-copy JSON tokenizer inspired by JSMN.
 * It does not build an AST or copy strings; instead it records token slices
 * into caller-provided `struct json_token` storage.
 *
 * @warning Contains floating-point division. Extremely slow on Cortex-M0
 *          without FPU. Evaluate fixed-point alternatives for bare-metal
 *          targets.
 *
 * Usage example
 * -------------
 * struct json_token  tokens[16];
 * struct json_parser parser = {0};
 *
 * json_parser_init(&parser);
 * json_parse(&parser, rx_buf, rx_len, tokens, 16);
 *
 * Ownership / lifecycle / context
 * -------------------------------
 * - The input buffer remains caller-owned and is never modified.
 * - The token array and parser context remain caller-owned.
 * - Helper extractors operate directly on the original buffer slices.
 * - @note Re-entrant for distinct caller-owned parser contexts and token
 *   arrays. Not thread-safe for a shared parser instance. Not ISR-safe.
 *
 * Blocking / allocation
 * ---------------------
 * - No allocation, no recursion, and no RTOS dependency.
 * - The parser performs one linear scan over the provided JSON slice.
 *
 * Failure / recovery
 * ------------------
 * - Malformed JSON fails with `-EINVAL`.
 * - Insufficient token storage fails with `-ENOSPC`.
 * - Truncated container state fails with `-EBADMSG`.
 * - Nesting deeper than the internal non-recursive validator stack fails with
 *   `-EOVERFLOW`.
 */

#pragma once

/* Includes ------------------------------------------------------------------------------------- */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Public Types --------------------------------------------------------------------------------- */

/**
 * @brief  JSON token kinds produced by the tokenizer.
 */
enum json_type {
    /**
     * @brief  Uninitialized or not-yet-closed token slot.
     */
    JSON_TYPE_UNDEFINED = 0,

    /**
     * @brief  Object token. `size` counts key-value pairs.
     */
    JSON_TYPE_OBJECT,

    /**
     * @brief  Array token. `size` counts direct elements.
     */
    JSON_TYPE_ARRAY,

    /**
     * @brief  String token. `start` and `end` exclude quote characters.
     */
    JSON_TYPE_STRING,

    /**
     * @brief  Primitive token: number, boolean, or null.
     */
    JSON_TYPE_PRIMITIVE
};

/**
 * @brief  Slice descriptor into the original JSON input buffer.
 *
 * The slice covers `js[start .. end)`. For string tokens, the surrounding
 * quotes are not included.
 */
struct json_token {
    /**
     * @brief  Token kind.
     */
    enum json_type type;

    /**
     * @brief  Inclusive byte offset of the first token character.
     */
    int start;

    /**
     * @brief  Exclusive byte offset past the last token character.
     */
    int end;

    /**
     * @brief  Number of direct children.
     *
     * For objects, this is the key-value pair count. For arrays, it is the
     * element count. For object-key strings, the value is 1.
     */
    int size;

    /**
     * @brief  Index of the enclosing container token, or -1 for the root token.
     */
    int parent;
};

/**
 * @brief  Parser state for one tokenization pass.
 *
 * Call json_parser_init() before each parse operation.
 */
struct json_parser {
    /**
     * @brief  Current byte position in the input stream.
     */
    size_t pos;

    /**
     * @brief  Index of the next free token slot.
     */
    int toknext;

    /**
     * @brief  Index of the innermost open container token, or -1.
     */
    int toksuper;
};

/* Public Functions ----------------------------------------------------------------------------- */

/* Lifecycle ------------------------------------------------------------------------------------ */

/**
 * @brief  Initialize or reset a parser context.
 *
 * @param  parser  Parser context to initialize. Must not be NULL.
 */
void json_parser_init(struct json_parser *parser);

/* Parse ---------------------------------------------------------------------------------------- */

/**
 * @brief  Tokenize a JSON text buffer.
 *
 * Performs one linear scan over `js[0 .. js_len)`. The input buffer is never
 * modified.
 *
 * @param  parser      Initialized parser context. Must not be NULL.
 * @param  js          Input JSON text. Need not be NUL-terminated. Must not be
 *                     NULL.
 * @param  js_len      Number of bytes available in @p js.
 * @param  tokens      Caller-allocated token array. May be NULL only when
 *                     @p num_tokens is 0 for a count-only dry run.
 * @param  num_tokens  Capacity of @p tokens.
 *
 * @return Positive token count on success, or negative `-errno` on failure.
 *
 * @note   On error, the partial token array must be treated as undefined.
 * @note   Passing `tokens = NULL` and `num_tokens = 0` performs a dry run that
 *         returns the token count required by a real parse.
 */
int json_parse(struct json_parser *parser,
               const char         *js,
               size_t              js_len,
               struct json_token  *tokens,
               size_t              num_tokens);

/* Value Extraction ----------------------------------------------------------------------------- */

/**
 * @brief  Test whether a string token equals a NUL-terminated C string.
 *
 * @param  js   Original JSON buffer passed to json_parse().
 * @param  tok  Token to compare. Must represent a string token.
 * @param  str  NUL-terminated C string. Must not be NULL.
 *
 * @return `true` when the token content equals @p str; otherwise `false`.
 */
bool json_token_streq(const char *js, const struct json_token *tok, const char *str);

/**
 * @brief  Extract a floating-point value from a primitive token.
 *
 * Supports optional sign, fractional part, and decimal exponent.
 *
 * @param  js   Original JSON buffer passed to json_parse().
 * @param  tok  Primitive token to extract.
 *
 * @return Parsed float value, or `0.0f` on NULL, empty input, or
 *         incompatible token type.
 */
float json_get_float(const char *js, const struct json_token *tok);

/**
 * @brief  Extract a signed 32-bit integer from a primitive token.
 *
 * Overflow saturates to `INT32_MAX` or `INT32_MIN`.
 *
 * @param  js   Original JSON buffer passed to json_parse().
 * @param  tok  Primitive token to extract.
 *
 * @return Parsed integer value, or `0` on NULL, empty input, or incompatible
 *         token type.
 */
int32_t json_get_int(const char *js, const struct json_token *tok);

/**
 * @brief  Extract a boolean value from a primitive token.
 *
 * Accepts only the JSON literals `true` and `false`.
 *
 * @param  js   Original JSON buffer passed to json_parse().
 * @param  tok  Primitive token to extract.
 * @param  out  Destination for the parsed boolean. Must not be NULL.
 *
 * @return `true` when parsing succeeded and @p out was written. Returns
 *         `false` on invalid input, incompatible token type, or non-boolean
 *         primitive text.
 */
bool json_get_bool(const char *js, const struct json_token *tok, bool *out);

/**
 * @brief  Copy a string token into a caller-supplied buffer.
 *
 * @param  js       Original JSON buffer.
 * @param  tok      String token to copy.
 * @param  buf      Destination buffer. Must not be NULL.
 * @param  buf_len  Capacity of @p buf in bytes, including the terminating NUL.
 *
 * @return Non-negative byte count excluding the terminating NUL on success, or
 *         `-EINVAL` when the token is not a string or the arguments are
 *         invalid, or
 *         `-ENOSPC` when the token does not fit in @p buf.
 */
int json_token_copy(const char *js, const struct json_token *tok, char *buf, size_t buf_len);

/* Inline Helpers ------------------------------------------------------------------------------- */

/**
 * @brief  Return the byte length of a token slice.
 *
 * @param  tok  Parsed token. Must not be NULL.
 *
 * @return `tok->end - tok->start`.
 */
static inline int json_token_len(const struct json_token *tok)
{
    return tok->end - tok->start;
}

#ifdef __cplusplus
}
#endif

/* ---------------------------------------- end of file ----------------------------------------- */
