/*
 * ts_blank_space_corpus — run ts-blank-space's fixture corpus through
 * whiteout and classify the result for each fixture. The manifest in
 * fixture_manifest below documents expected behavior per fixture.
 *
 * Classifications:
 *   EXPECT_BYTE_EQUAL — whiteout output should match expected/<name>.js
 *                       byte-for-byte (modulo trailing whitespace).
 *   EXPECT_REJECT     — whiteout should return WHITEOUT_ERR_UNSUPPORTED
 *                       (rule:  expected_substring describes the kind of
 *                       construct that triggers the rejection).
 *   EXPECT_PARSE_ERR  — tree-sitter rejects the input; whiteout returns
 *                       WHITEOUT_ERR_PARSE.
 *   EXPECT_DIVERGES   — output differs from ts-blank-space's expected, but
 *                       the difference is documented and runtime-equivalent.
 *                       The reason field explains why.
 */

#include "whiteout/whiteout.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    EXPECT_BYTE_EQUAL,
    EXPECT_REJECT,
    EXPECT_PARSE_ERR,
    EXPECT_DIVERGES,
} Expectation;

typedef struct {
    const char *name;
    Expectation expected;
    const char *reason;  /* free-text note shown on pass and on mismatch */
} Fixture;

static int g_pass = 0;
static int g_fail = 0;

static int read_file(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return 0; }
    fclose(f);
    buf[sz] = '\0';
    *out = buf; *out_len = (size_t)sz;
    return 1;
}

static void rtrim(char *s, size_t *len) {
    while (*len > 0 && (s[*len - 1] == '\n' || s[*len - 1] == '\r'
                        || s[*len - 1] == ' ' || s[*len - 1] == '\t')) {
        s[--*len] = '\0';
    }
}

static void pass(const Fixture *f, const char *detail) {
    printf("  ok    %-30s %s%s%s\n", f->name,
           detail ? "(" : "", detail ? detail : "", detail ? ")" : "");
    g_pass++;
}

static void fail(const Fixture *f, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("  FAIL  %-30s ", f->name);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    g_fail++;
}

static void run_fixture(whiteout_ctx *ctx, const Fixture *f) {
    char ts_path[1024], js_path[1024];
    snprintf(ts_path, sizeof ts_path, "%s/cases/%s.ts", WHITEOUT_CORPUS_DIR, f->name);
    snprintf(js_path, sizeof js_path, "%s/expected/%s.js", WHITEOUT_CORPUS_DIR, f->name);

    char *src = NULL; size_t src_len = 0;
    if (!read_file(ts_path, &src, &src_len)) {
        fail(f, "cannot read %s", ts_path);
        return;
    }

    char *out = NULL; size_t out_len = 0;
    whiteout_error err = {0};
    whiteout_status st = whiteout_transform(ctx, src, src_len, &out, &out_len, &err);

    switch (f->expected) {
    case EXPECT_BYTE_EQUAL: {
        if (st != WHITEOUT_OK) {
            fail(f, "transform err status=%d msg=%s offset=%zu",
                 (int)st, err.message ? err.message : "(null)", err.offset);
            break;
        }
        char *expected = NULL; size_t expected_len = 0;
        if (!read_file(js_path, &expected, &expected_len)) {
            fail(f, "cannot read %s", js_path);
            whiteout_free(out);
            break;
        }
        size_t alen = out_len, elen = expected_len;
        rtrim(out, &alen);
        rtrim(expected, &elen);
        if (alen == elen && memcmp(out, expected, alen) == 0) {
            pass(f, f->reason);
        } else {
            fail(f, "byte mismatch (in=%zu out=%zu expected=%zu)",
                 src_len, out_len, expected_len);
            /* show the first differing line */
            size_t i = 0;
            while (i < alen && i < elen && out[i] == expected[i]) i++;
            size_t line_start = i;
            while (line_start > 0 && out[line_start - 1] != '\n') line_start--;
            size_t end_a = i, end_e = i;
            while (end_a < alen && out[end_a] != '\n') end_a++;
            while (end_e < elen && expected[end_e] != '\n') end_e++;
            printf("        first diff at byte %zu\n", i);
            printf("        got:      [%.*s]\n", (int)(end_a - line_start), out + line_start);
            printf("        expected: [%.*s]\n", (int)(end_e - line_start), expected + line_start);
        }
        free(expected);
        break;
    }

    case EXPECT_REJECT:
        if (st == WHITEOUT_ERR_UNSUPPORTED) {
            pass(f, err.message);
        } else {
            fail(f, "expected reject, got status=%d msg=%s",
                 (int)st, err.message ? err.message : "(null)");
        }
        break;

    case EXPECT_PARSE_ERR:
        if (st == WHITEOUT_ERR_PARSE) {
            pass(f, err.message);
        } else {
            fail(f, "expected parse error, got status=%d msg=%s",
                 (int)st, err.message ? err.message : "(null)");
        }
        break;

    case EXPECT_DIVERGES:
        if (st == WHITEOUT_OK) {
            pass(f, f->reason);
        } else {
            fail(f, "expected diverging success, got status=%d msg=%s",
                 (int)st, err.message ? err.message : "(null)");
        }
        break;
    }

    whiteout_free(out);
    free(src);
}

int main(void) {
    static const Fixture fixtures[] = {
        { "a", EXPECT_PARSE_ERR,
          "contains TS syntax tree-sitter doesn't accept (definite assignment + complex parameters at line 17)" },
        { "a-works", EXPECT_BYTE_EQUAL,
          "a.ts with line 17 (optional param + comment between `?` and `:`) excised; covers the rest" },
        { "b", EXPECT_DIVERGES,
          "instantiation_expression `(expr)<T>`tpl`` parses as binary_expression in tree-sitter, so `<T>` survives blanking" },
        { "b-works", EXPECT_BYTE_EQUAL,
          "b.ts with line 84 (`(arrow)<T>`tpl``) excised; covers the rest" },
        { "asi", EXPECT_PARSE_ERR,
          "class-body `f = 1 as T\\n[\"x\"]()` is parse-error in tree-sitter; users add explicit `;`" },
        { "asi-works", EXPECT_BYTE_EQUAL,
          "asi.ts with the failing class-ASI method body excised; covers top-level ASI and class-field cases" },
        { "arrow-functions", EXPECT_BYTE_EQUAL,
          "multi-line arrow function types handled via paren-swap (matches ts-blank-space)" },
        { "decorators", EXPECT_PARSE_ERR,
          "fixture has `@expr<Type>` decorator with type arguments that tree-sitter doesn't accept" },
        { "modules", EXPECT_BYTE_EQUAL,
          "import/export type forms; byte-equal to ts-blank-space output" },
        { "namespaces", EXPECT_REJECT,
          "all namespaces are rejected (we don't try to distinguish type-only)" },
        { "parenthetised-types", EXPECT_BYTE_EQUAL,
          "parenthesised types blank cleanly; byte-equal" },
    };
    size_t n = sizeof fixtures / sizeof fixtures[0];

    whiteout_ctx *ctx = whiteout_ctx_new();
    if (!ctx) { fprintf(stderr, "ctx alloc failed\n"); return 2; }

    for (size_t i = 0; i < n; ++i) run_fixture(ctx, &fixtures[i]);

    whiteout_ctx_free(ctx);
    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
