/*
 * whiteout_tests — exercises the library through its C ABI.
 *
 * Three test categories:
 *   - exact: input -> byte-identical expected output
 *   - property: input -> output of equal length, same newline positions,
 *     and parses without ERROR/MISSING when fed back through tree-sitter
 *   - reject: input -> specific whiteout_status with byte offset
 */

#include "whiteout/whiteout.h"
#include <tree_sitter/api.h>

#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>

const TSLanguage *tree_sitter_typescript(void);

static int g_pass = 0;
static int g_fail = 0;
static whiteout_ctx *g_ctx = NULL;
static TSParser     *g_verify = NULL;

static void fail_fmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("  FAIL  ", stdout);
    vprintf(fmt, ap);
    va_end(ap);
    g_fail++;
}

static void pass_msg(const char *name) {
    printf("  ok    %s\n", name);
    g_pass++;
}

/* Read the entire contents of a file. Returns 0 on failure. */
static int read_file(const char *path, char **out, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return 0; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(buf); return 0; }
    buf[sz] = '\0';
    *out = buf;
    *out_len = (size_t)sz;
    return 1;
}

static int write_file(const char *path, const char *buf, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    size_t wrote = fwrite(buf, 1, len, f);
    fclose(f);
    return wrote == len;
}

static void rtrim(char *s, size_t *len) {
    while (*len > 0 && (s[*len - 1] == '\n' || s[*len - 1] == '\r' || s[*len - 1] == ' '
                        || s[*len - 1] == '\t')) {
        s[--*len] = '\0';
    }
}

/* Run a single end-to-end fixture: read .ts, transform, write .mjs, exec node,
   compare its stdout against .expected. Returns 1 on pass. */
static int run_e2e_fixture(const char *name) {
    char ts_path[1024], expected_path[1024], js_path[1024], cmd[2048];
    snprintf(ts_path, sizeof ts_path, "%s/%s.ts", WHITEOUT_E2E_DIR, name);
    snprintf(expected_path, sizeof expected_path, "%s/%s.expected", WHITEOUT_E2E_DIR, name);
    snprintf(js_path, sizeof js_path, "%s/%s.mjs", WHITEOUT_E2E_BUILD_DIR, name);

    char *ts_src = NULL; size_t ts_len = 0;
    if (!read_file(ts_path, &ts_src, &ts_len)) {
        fail_fmt("e2e %s: cannot read fixture %s: %s\n", name, ts_path, strerror(errno));
        return 0;
    }

    char *js_out = NULL; size_t js_len = 0;
    whiteout_error err = {0};
    whiteout_status st = whiteout_transform(g_ctx, ts_src, ts_len, &js_out, &js_len, &err);
    free(ts_src);
    if (st != WHITEOUT_OK) {
        fail_fmt("e2e %s: transform failed status=%d msg=%s offset=%zu\n",
                 name, (int)st, err.message ? err.message : "(null)", err.offset);
        return 0;
    }
    if (!write_file(js_path, js_out, js_len)) {
        fail_fmt("e2e %s: cannot write %s\n", name, js_path);
        whiteout_free(js_out);
        return 0;
    }
    whiteout_free(js_out);

    snprintf(cmd, sizeof cmd, "\"%s\" \"%s\" 2>&1", WHITEOUT_NODE_EXECUTABLE, js_path);
    FILE *p = popen(cmd, "r");
    if (!p) {
        fail_fmt("e2e %s: popen failed: %s\n", name, strerror(errno));
        return 0;
    }
    char actual[16384];
    size_t got = 0;
    size_t n;
    while (got + 1 < sizeof actual
           && (n = fread(actual + got, 1, sizeof actual - 1 - got, p)) > 0) {
        got += n;
    }
    actual[got] = '\0';
    int rc = pclose(p);

    char *expected = NULL; size_t expected_len = 0;
    if (!read_file(expected_path, &expected, &expected_len)) {
        fail_fmt("e2e %s: cannot read %s\n", name, expected_path);
        return 0;
    }
    rtrim(actual, &got);
    rtrim(expected, &expected_len);

    if (rc != 0) {
        fail_fmt("e2e %s: node exited rc=%d\n        stdout:\n%s\n", name, rc, actual);
        free(expected);
        return 0;
    }
    if (got != expected_len || memcmp(actual, expected, got) != 0) {
        fail_fmt("e2e %s: output mismatch\n        expected: %.*s\n        got:      %.*s\n",
                 name, (int)expected_len, expected, (int)got, actual);
        free(expected);
        return 0;
    }
    free(expected);

    char label[256];
    snprintf(label, sizeof label, "e2e %s", name);
    pass_msg(label);
    return 1;
}

static int output_has_parse_error(const char *out, size_t out_len) {
    TSTree *tree = ts_parser_parse_string(g_verify, NULL, out, (uint32_t)out_len);
    TSNode root = ts_tree_root_node(tree);
    enum { STACK_MAX = 2048 };
    TSNode stack[STACK_MAX];
    int top = 0;
    stack[top++] = root;
    int bad = 0;
    while (top > 0) {
        TSNode n = stack[--top];
        if (ts_node_is_error(n) || ts_node_is_missing(n)) { bad = 1; break; }
        uint32_t cc = ts_node_child_count(n);
        for (uint32_t i = 0; i < cc && top < STACK_MAX; ++i) {
            stack[top++] = ts_node_child(n, i);
        }
    }
    ts_tree_delete(tree);
    return bad;
}

/* exact-match strip test */
static void t_exact(const char *name, const char *in, const char *expected) {
    char *out = NULL; size_t out_len = 0;
    whiteout_error err = {0};
    whiteout_status st = whiteout_transform(g_ctx, in, strlen(in), &out, &out_len, &err);
    if (st != WHITEOUT_OK) {
        fail_fmt("%s: transform failed status=%d msg=%s offset=%zu\n",
                 name, (int)st, err.message ? err.message : "(null)", err.offset);
        return;
    }
    size_t exp_len = strlen(expected);
    if (out_len != exp_len) {
        fail_fmt("%s: length got=%zu expected=%zu\n", name, out_len, exp_len);
        printf("        in :  [%s]\n", in);
        printf("        exp:  [%s]\n", expected);
        printf("        got:  [%.*s]\n", (int)out_len, out);
        whiteout_free(out);
        return;
    }
    if (memcmp(out, expected, out_len) != 0) {
        fail_fmt("%s: content mismatch\n", name);
        printf("        in :  [%s]\n", in);
        printf("        exp:  [%s]\n", expected);
        printf("        got:  [%.*s]\n", (int)out_len, out);
        whiteout_free(out);
        return;
    }
    /* extra: output must itself parse cleanly */
    if (output_has_parse_error(out, out_len)) {
        fail_fmt("%s: output failed re-parse\n", name);
        whiteout_free(out);
        return;
    }
    whiteout_free(out);
    pass_msg(name);
}

/* property-based strip test: length, newline positions, and re-parse cleanliness */
static void t_property(const char *name, const char *in) {
    char *out = NULL; size_t out_len = 0;
    whiteout_error err = {0};
    whiteout_status st = whiteout_transform(g_ctx, in, strlen(in), &out, &out_len, &err);
    if (st != WHITEOUT_OK) {
        fail_fmt("%s: transform failed status=%d msg=%s offset=%zu\n",
                 name, (int)st, err.message ? err.message : "(null)", err.offset);
        return;
    }
    size_t in_len = strlen(in);
    if (out_len != in_len) {
        fail_fmt("%s: length not preserved (got %zu, in %zu)\n", name, out_len, in_len);
        whiteout_free(out);
        return;
    }
    for (size_t i = 0; i < in_len; ++i) {
        int in_nl  = (in[i]  == '\n') || (in[i]  == '\r');
        int out_nl = (out[i] == '\n') || (out[i] == '\r');
        if (in_nl != out_nl) {
            fail_fmt("%s: newline at byte %zu not preserved (in=0x%02x out=0x%02x)\n",
                     name, i, (unsigned)(unsigned char)in[i], (unsigned)(unsigned char)out[i]);
            whiteout_free(out);
            return;
        }
    }
    if (output_has_parse_error(out, out_len)) {
        fail_fmt("%s: output failed re-parse\n", name);
        printf("        in :  [%s]\n", in);
        printf("        got:  [%.*s]\n", (int)out_len, out);
        whiteout_free(out);
        return;
    }
    whiteout_free(out);
    pass_msg(name);
}

/* property test with one additional check: a given substring must appear at a given offset
   in the output (to confirm a kept identifier didn't move). */
static void t_property_at(const char *name, const char *in,
                          const char *needle, size_t expected_offset) {
    char *out = NULL; size_t out_len = 0;
    whiteout_error err = {0};
    whiteout_status st = whiteout_transform(g_ctx, in, strlen(in), &out, &out_len, &err);
    if (st != WHITEOUT_OK) {
        fail_fmt("%s: transform failed status=%d msg=%s\n",
                 name, (int)st, err.message ? err.message : "(null)");
        return;
    }
    size_t nlen = strlen(needle);
    if (expected_offset + nlen > out_len ||
        memcmp(out + expected_offset, needle, nlen) != 0) {
        fail_fmt("%s: expected '%s' at offset %zu\n", name, needle, expected_offset);
        printf("        got: [%.*s]\n", (int)out_len, out);
        whiteout_free(out);
        return;
    }
    if (out_len != strlen(in)) {
        fail_fmt("%s: length not preserved\n", name);
        whiteout_free(out);
        return;
    }
    if (output_has_parse_error(out, out_len)) {
        fail_fmt("%s: output failed re-parse\n", name);
        whiteout_free(out);
        return;
    }
    whiteout_free(out);
    pass_msg(name);
}

/* reject test */
static void t_reject(const char *name, const char *in,
                     whiteout_status expect_st, size_t expect_off) {
    char *out = (char *)0x1; size_t out_len = 12345;
    whiteout_error err = {0};
    whiteout_status st = whiteout_transform(g_ctx, in, strlen(in), &out, &out_len, &err);
    if (st != expect_st) {
        fail_fmt("%s: status got=%d expected=%d\n", name, (int)st, (int)expect_st);
        return;
    }
    if (out != NULL) {
        fail_fmt("%s: out pointer not null on error\n", name);
        return;
    }
    if (out_len != 0) {
        fail_fmt("%s: out_len not zero on error\n", name);
        return;
    }
    if (err.status != expect_st) {
        fail_fmt("%s: err.status got=%d expected=%d\n", name, (int)err.status, (int)expect_st);
        return;
    }
    if (err.offset != expect_off) {
        fail_fmt("%s: err.offset got=%zu expected=%zu\n", name, err.offset, expect_off);
        return;
    }
    if (!err.message || err.message[0] == '\0') {
        fail_fmt("%s: err.message empty\n", name);
        return;
    }
    pass_msg(name);
}

int main(void) {
    g_ctx = whiteout_ctx_new();
    if (!g_ctx) { fprintf(stderr, "ctx alloc failed\n"); return 2; }
    g_verify = ts_parser_new();
    if (!ts_parser_set_language(g_verify, tree_sitter_typescript())) {
        fprintf(stderr, "verify parser setup failed\n");
        return 2;
    }

    /* === exact-match strip === */
    t_exact("type annotation on let",
            "let x: number = 1;",
            "let x         = 1;");

    t_exact("generic function decl",
            "function f<T>(x: T): T { return x; }",
            "function f   (x   )    { return x; }");

    t_exact("as expression",
            "let x = 1 as number;",
            "let x = 1          ;");

    t_exact("chained as",
            "let x = 1 as unknown as number;",
            "let x = 1                     ;");

    t_exact("non-null suffix",
            "let x = y!;",
            "let x = y ;");

    t_reject("prefix type assertion rejected",
             "let x = <T>y;",
             WHITEOUT_ERR_UNSUPPORTED, 8);
    t_reject("prefix type assertion in arrow body rejected",
             "const f = () => <T>{ a: 1 };",
             WHITEOUT_ERR_UNSUPPORTED, 16);

    t_exact("satisfies",
            "const x = { a: 1 } satisfies { a: number };",
            "const x = { a: 1 }                        ;");

    t_exact("mixed named import",
            "import { A, type B, C } from \"x\";",
            "import { A,         C } from \"x\";");

    t_exact("mixed named import, type first",
            "import { type A, B, C } from \"x\";",
            "import {         B, C } from \"x\";");

    t_exact("mixed named import, type last",
            "import { A, B, type C } from \"x\";",
            "import { A, B         } from \"x\";");

    t_exact("class field with type and init",
            "class C { x: number = 1; }",
            "class C { x         = 1; }");

    t_exact("class field optional marker",
            "class C { x?: number = 1; }",
            "class C { x          = 1; }");

    t_exact("optional parameter marker",
            "function f(x?: number) { return x; }",
            "function f(x         ) { return x; }");

    t_exact("non-null in expression",
            "let x = a!.b!.c;",
            "let x = a .b .c;");

    /* === property-based strip === */
    t_property("interface decl",
               "interface I { a: number; b(): void; }");

    t_property("type alias union",
               "type Foo = string | number | null;");

    t_property("import type whole",
               "import type { A, B } from \"x\";");

    t_property("export type whole",
               "export type { A, B } from \"x\";");

    t_property("export type star",
               "export type * from \"x\";");

    t_property("ambient const",
               "declare const VERSION: string;");

    t_property("ambient function",
               "declare function hello(name: string): void;");

    t_property("ambient class",
               "declare class Foo { x: number; }");

    t_property("ambient module",
               "declare module \"foo\" { export const x: number; }");

    t_property("ambient global",
               "declare global { interface Window { x: number; } }");

    t_property("class with modifiers",
               "class C { public readonly x = 1; private y = 2; protected z = 3; }");

    t_property("class field declare blanked whole",
               "class C { declare x: number; method() { return 1; } }");

    t_property("abstract class with abstract method and field",
               "abstract class C { abstract foo(): void; abstract x: number; }");

    t_property("generic class",
               "class Box<T extends object> { value: T; constructor(v: T) { this.value = v; } }");

    t_property("class extends and implements",
               "class C extends B implements I, J { x: number = 1; }");

    t_property("override method",
               "class D extends B { override greet(): string { return \"hi\"; } }");

    t_property("type predicate return",
               "function isNum(x: unknown): x is number { return typeof x === \"number\"; }");

    t_property("asserts return",
               "function assert(x: unknown): asserts x { if (!x) throw new Error(\"\"); }");

    t_property("using declaration preserved",
               "{ using r = { [Symbol.dispose]() {} }; }");

    t_property("await using preserved",
               "async function f() { await using r = { [Symbol.asyncDispose]: async () => {} }; }");

    t_property("accessor field preserved",
               "class C { accessor x = 1; }");

    t_property("multi-line interface",
               "interface I {\n  a: number;\n  b: string;\n}\n");

    t_property("multi-line class with methods",
               "class P {\n"
               "  x: number = 1;\n"
               "  greet(name: string): string {\n"
               "    return `hi ${name}`;\n"
               "  }\n"
               "}\n");

    t_property("string contains type-like punctuation",
               "let s: string = \"a: number = 1; type T = string;\";");

    t_property("template literal preserved",
               "let s = `a: ${1+2} type T`;");

    t_property("nested types in annotations",
               "let m: Map<string, Array<number>> = new Map();");

    t_property("conditional type",
               "type X<T> = T extends string ? number : boolean;");

    t_property("infer in conditional",
               "type First<T> = T extends [infer F, ...any[]] ? F : never;");

    t_property("mapped type",
               "type Partial2<T> = { [K in keyof T]?: T[K] };");

    t_property("readonly modifier in type",
               "type R = readonly number[];");

    t_property("variance modifiers",
               "interface I<in T, out U> { f(x: T): U; }");

    t_property("const type parameter",
               "function f<const T>(x: T): T { return x; }");

    t_property("type import inside mixed default",
               "import D, { type T, x } from \"m\";");

    t_property("export named with type",
               "const a = 1; const b = 2; export { a, type T, b };");

    t_property("complex method modifiers",
               "class C { public static async foo<T>(x: T): Promise<T> { return x; } }");

    t_property("pure JS untouched",
               "function add(a, b) { return a + b; }\n"
               "const arr = [1, 2, 3].map(x => x * 2);\n");

    t_property("empty input",
               "");

    t_property("comments preserved",
               "// a: number\n/* b: string */\nlet x = 1;");

    /* === property + positional substring === */
    /* "let x: number = 1;" — the kept '1' is at offset 16. */
    t_property_at("kept literal position",
                  "let x: number = 1;", "1", 16);
    /* generic function: kept return statement starts at offset 25. */
    t_property_at("kept return position",
                  "function f<T>(x: T): T { return x; }", "return x;", 25);
    /* mixed import: 'C' is at offset 19 in the input. */
    t_property_at("kept identifier in mixed import",
                  "import { A, type B, C } from \"x\";", "C", 20);

    /* === reject === */
    t_reject("enum", "enum E { A, B }", WHITEOUT_ERR_UNSUPPORTED, 0);
    t_reject("const enum", "const enum E { A = 1 }", WHITEOUT_ERR_UNSUPPORTED, 0);
    t_reject("namespace", "namespace N { export const x = 1; }",
             WHITEOUT_ERR_UNSUPPORTED, 0);
    t_reject("module form", "module N { export const x = 1; }",
             WHITEOUT_ERR_UNSUPPORTED, 0);
    t_reject("import equals require",
             "import x = require(\"y\");",
             WHITEOUT_ERR_UNSUPPORTED, 0);
    t_reject("import equals identifier",
             "import x = NS.Foo;",
             WHITEOUT_ERR_UNSUPPORTED, 0);
    t_reject("export equals",
             "export = X;",
             WHITEOUT_ERR_UNSUPPORTED, 0);
    t_reject("parameter property public",
             "class C { constructor(public x: number) {} }",
             WHITEOUT_ERR_UNSUPPORTED, 22);
    t_reject("parameter property private readonly",
             "class C { constructor(private readonly x: number) {} }",
             WHITEOUT_ERR_UNSUPPORTED, 22);
    t_reject("parameter property readonly alone",
             "class C { constructor(readonly x: number) {} }",
             WHITEOUT_ERR_UNSUPPORTED, 22);
    t_reject("parameter property override alone",
             "class C { constructor(override x: number) {} }",
             WHITEOUT_ERR_UNSUPPORTED, 22);
    t_reject("decorator on class",
             "function d(_v, _c) {} @d class C {}",
             WHITEOUT_ERR_UNSUPPORTED, 22);
    t_reject("decorator on method",
             "function d(_v, _c) {} class C { @d m() {} }",
             WHITEOUT_ERR_UNSUPPORTED, 32);
    t_reject("decorator on field",
             "function d(_v, _c) {} class C { @d x = 1; }",
             WHITEOUT_ERR_UNSUPPORTED, 32);
    t_reject("decorator on parameter",
             "function d(_v, _c) {} class C { m(@d x) {} }",
             WHITEOUT_ERR_UNSUPPORTED, 34);

    /* enum nested inside function body must also be detected */
    t_reject("enum nested in function",
             "function f() { enum E { A } return E; }",
             WHITEOUT_ERR_UNSUPPORTED, 15);

    /* === parse error === */
    {
        const char *in = "let x = ;";
        char *out = (char *)0x1; size_t out_len = 1;
        whiteout_error err = {0};
        whiteout_status st = whiteout_transform(g_ctx, in, strlen(in), &out, &out_len, &err);
        if (st == WHITEOUT_ERR_PARSE && out == NULL && err.status == WHITEOUT_ERR_PARSE
            && err.message && err.message[0] != '\0') {
            pass_msg("parse error: bare =");
        } else {
            fail_fmt("parse error: bare =: got st=%d out=%p msg=%s\n",
                     (int)st, (void *)out, err.message ? err.message : "(null)");
        }
    }
    {
        const char *in = "class C {";
        char *out = NULL; size_t out_len = 0;
        whiteout_error err = {0};
        whiteout_status st = whiteout_transform(g_ctx, in, strlen(in), &out, &out_len, &err);
        if (st == WHITEOUT_ERR_PARSE) pass_msg("parse error: unterminated class");
        else fail_fmt("parse error: unterminated class: got st=%d\n", (int)st);
    }

    /* === UTF-8 error === */
    {
        const char in[] = { 'l','e','t',' ','x',' ','=',' ','"',(char)0xff,'"',';' };
        char *out = NULL; size_t out_len = 0;
        whiteout_error err = {0};
        whiteout_status st = whiteout_transform(g_ctx, in, sizeof in, &out, &out_len, &err);
        if (st == WHITEOUT_ERR_UTF8 && err.offset == 9) {
            pass_msg("invalid UTF-8 in string literal");
        } else {
            fail_fmt("UTF-8 detect: got st=%d offset=%zu\n", (int)st, err.offset);
        }
    }

    /* === sanity / ABI === */

    /* whiteout_free(NULL) must be a no-op */
    whiteout_free(NULL);
    pass_msg("whiteout_free(NULL) is safe");

    /* ctx reuse across many calls */
    {
        int ok = 1;
        for (int i = 0; i < 50 && ok; ++i) {
            const char *in = "let x: number = 1;";
            const char *expected = "let x         = 1;";
            char *out = NULL; size_t out_len = 0;
            whiteout_error err = {0};
            whiteout_status st = whiteout_transform(g_ctx, in, strlen(in), &out, &out_len, &err);
            if (st != WHITEOUT_OK || out_len != strlen(expected)
                || memcmp(out, expected, out_len) != 0) {
                ok = 0;
            }
            whiteout_free(out);
        }
        if (ok) pass_msg("ctx reuse 50x");
        else fail_fmt("ctx reuse failed\n");
    }

    /* error message overwritten on next call: success after failure must clear */
    {
        char *out = NULL; size_t out_len = 0;
        whiteout_error err = {0};
        whiteout_status st = whiteout_transform(g_ctx, "enum E {}", 9, &out, &out_len, &err);
        if (st != WHITEOUT_ERR_UNSUPPORTED) fail_fmt("seq: expected reject first\n");
        else {
            err.status = WHITEOUT_OK; err.message = ""; err.offset = 0;
            const char *clean = "let x = 1;";
            st = whiteout_transform(g_ctx, clean, strlen(clean), &out, &out_len, &err);
            if (st == WHITEOUT_OK && out_len == strlen(clean)
                && memcmp(out, clean, out_len) == 0) {
                pass_msg("ctx error -> success cycle");
            } else {
                fail_fmt("seq: cycle failed st=%d out_len=%zu\n", (int)st, out_len);
            }
            whiteout_free(out);
        }
    }

    /* === end-to-end: run whiteout output through real node === */
    {
        const char *fixtures[] = {
            "simple_const",
            "function_with_types",
            "generic_identity",
            "as_satisfies",
            "interface_and_class",
            "optional_param",
            "definite_assignment",
            "template_literal",
            "nested_generics",
            "type_alias",
        };
        size_t nf = sizeof fixtures / sizeof fixtures[0];
        for (size_t i = 0; i < nf; ++i) run_e2e_fixture(fixtures[i]);
    }

    ts_parser_delete(g_verify);
    whiteout_ctx_free(g_ctx);

    printf("\n%d pass, %d fail\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
