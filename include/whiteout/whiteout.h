#ifndef WHITEOUT_WHITEOUT_H
#define WHITEOUT_WHITEOUT_H

/* Sequential ABI version. Bumped on every release that changes anything
 * a consumer can observe through this header. Use `#if WHITEOUT_VERSION >= N`
 * to gate against a specific revision. */
#define WHITEOUT_VERSION 1

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct whiteout_ctx whiteout_ctx;

typedef enum {
    WHITEOUT_OK = 0,
    WHITEOUT_ERR_PARSE,        /* ERROR or MISSING node in parse tree */
    WHITEOUT_ERR_UNSUPPORTED,  /* rejected construct (enum, namespace, ...) */
    WHITEOUT_ERR_ALLOC,        /* allocation failure */
    WHITEOUT_ERR_UTF8,         /* input was not valid UTF-8 */
    WHITEOUT_ERR_INTERNAL
} whiteout_status;

typedef struct {
    whiteout_status status;
    const char *message;
    size_t offset;
} whiteout_error;

whiteout_ctx *whiteout_ctx_new(void);
void whiteout_ctx_free(whiteout_ctx *ctx);

whiteout_status whiteout_transform(
    whiteout_ctx *ctx,
    const char *src, size_t src_len,
    char **out, size_t *out_len,
    whiteout_error *err);

/* Transform `buf[0..len)` in place. The caller owns the buffer; do not pass
 * it to whiteout_free. Same byte-equal-length and position-preservation
 * guarantees as whiteout_transform. On any non-OK return, no bytes of the
 * buffer have been modified. */
whiteout_status whiteout_transform_inplace(
    whiteout_ctx *ctx,
    char *buf, size_t len,
    whiteout_error *err);

void whiteout_free(char *buf);

#ifdef __cplusplus
}
#endif

#endif
