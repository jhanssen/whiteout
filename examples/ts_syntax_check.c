/*
 * ts_syntax_check — parse a curated set of TypeScript snippets with the
 * vendored tree-sitter-typescript grammar and report any ERROR or MISSING
 * nodes. Used to assess whether the grammar covers syntax features added
 * in TS 5.x and 6.0.
 *
 * Exit code: 0 if every snippet parsed without ERROR/MISSING nodes,
 *            1 otherwise.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tree_sitter/api.h>

const TSLanguage *tree_sitter_typescript(void);

typedef struct {
    const char *name;        /* short human-readable label */
    const char *ts_version;  /* TS version where this syntax shipped */
    const char *src;         /* TypeScript source snippet */
} Snippet;

/*
 * Snippets are deliberately small. Each exercises one syntactic construct.
 * If a snippet provokes ERROR or MISSING nodes, the grammar does not
 * understand that construct and whiteout cannot rely on it.
 */
static const Snippet kSnippets[] = {
    /* Established baseline (should parse fine; sanity check) */
    { "type alias",                "4.0",  "type Foo = string;" },
    { "interface",                 "1.0",  "interface I { a: number; }" },
    { "type annotation",           "1.0",  "let x: number = 1;" },
    { "generic function",          "1.0",  "function f<T>(x: T): T { return x; }" },
    { "as expression",             "1.6",  "let x = 1 as number;" },

    /* TS 4.7 */
    { "infer extends",             "4.7",
      "type First<T> = T extends [infer F extends string, ...any[]] ? F : never;" },

    /* TS 4.9 */
    { "satisfies",                 "4.9",  "const x = { a: 1 } satisfies { a: number };" },

    /* TS 5.0 */
    { "const type parameter",      "5.0",  "function f<const T>(x: T): T { return x; }" },
    { "accessor keyword",          "5.0",  "class C { accessor x = 1; }" },
    { "stage-3 decorator",         "5.0",
      "function dec(_v: any, _c: any) {} class C { @dec method() {} }" },

    /* TS 5.2 — these are ECMAScript stage 3, syntactic */
    { "using declaration",         "5.2",
      "{ using r = { [Symbol.dispose]() {} }; }" },
    { "await using declaration",   "5.2",
      "async function f() { await using r = { [Symbol.asyncDispose]: async () => {} }; }" },

    /* ECMAScript import attributes (TS 5.3) */
    { "import attributes (with)",  "5.3",
      "import data from \"./d.json\" with { type: \"json\" };" },

    /* TS 6.0 — almost no new syntax. The subpath import is just ES import syntax. */
    { "subpath import",            "6.0",  "import foo from \"#/foo\";" },

    /* Runtime-bearing constructs whiteout will reject, but the grammar must
       still recognize them so we can detect and refuse. */
    { "enum",                      "1.0",  "enum E { A, B, C }" },
    { "const enum",                "1.4",  "const enum E { A = 1, B = 2 }" },
    { "namespace",                 "1.0",  "namespace N { export const x = 1; }" },
    { "parameter property",        "1.0",
      "class C { constructor(public x: number, private readonly y: string) {} }" },
    { "import equals",             "1.0",  "import x = require(\"y\");" },
};

static int snippet_has_errors(const TSNode root, int *first_err_row, int *first_err_col, const char **kind) {
    /* Walk subtree iteratively with a manual stack to avoid recursion overhead. */
    enum { STACK_MAX = 4096 };
    TSNode stack[STACK_MAX];
    int top = 0;
    stack[top++] = root;
    while (top > 0) {
        TSNode n = stack[--top];
        if (ts_node_is_error(n)) {
            TSPoint p = ts_node_start_point(n);
            *first_err_row = (int)p.row;
            *first_err_col = (int)p.column;
            *kind = "ERROR";
            return 1;
        }
        if (ts_node_is_missing(n)) {
            TSPoint p = ts_node_start_point(n);
            *first_err_row = (int)p.row;
            *first_err_col = (int)p.column;
            *kind = "MISSING";
            return 1;
        }
        uint32_t child_count = ts_node_child_count(n);
        for (uint32_t i = 0; i < child_count && top < STACK_MAX; ++i) {
            stack[top++] = ts_node_child(n, i);
        }
    }
    return 0;
}

int main(void) {
    TSParser *parser = ts_parser_new();
    if (!ts_parser_set_language(parser, tree_sitter_typescript())) {
        fprintf(stderr, "failed to set tree-sitter-typescript language\n");
        ts_parser_delete(parser);
        return 2;
    }

    size_t n = sizeof(kSnippets) / sizeof(kSnippets[0]);
    size_t pass = 0, fail = 0;
    int any_fail = 0;

    for (size_t i = 0; i < n; ++i) {
        const Snippet *s = &kSnippets[i];
        TSTree *tree = ts_parser_parse_string(parser, NULL, s->src, (uint32_t)strlen(s->src));
        TSNode root = ts_tree_root_node(tree);
        int row = 0, col = 0;
        const char *kind = NULL;
        int bad = snippet_has_errors(root, &row, &col, &kind);
        if (bad) {
            printf("  FAIL  [TS %-4s] %-30s  %s at %d:%d\n", s->ts_version, s->name, kind, row, col);
            char *sexp = ts_node_string(root);
            printf("        src: %s\n        tree: %s\n", s->src, sexp);
            free(sexp);
            fail++;
            any_fail = 1;
        } else {
            printf("  ok    [TS %-4s] %-30s\n", s->ts_version, s->name);
            pass++;
        }
        ts_tree_delete(tree);
    }

    ts_parser_delete(parser);
    printf("\n%zu/%zu snippets parsed without ERROR/MISSING nodes\n", pass, n);
    return any_fail ? 1 : 0;
}
