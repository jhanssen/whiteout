/*
 * ts_shape_check — confirm tree-sitter-typescript not only parses each TS 5.x/6.0
 * syntax feature without ERROR/MISSING nodes, but also yields the expected node
 * type. A construct that silently misparses (e.g. `using x = ...` treated as a
 * variable named `using`) would produce no ERRORs but the wrong shape.
 *
 * Check: each snippet's s-expression must contain the named substring.
 * Exit code: 0 if all checks pass, 1 otherwise.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tree_sitter/api.h>

const TSLanguage *tree_sitter_typescript(void);

typedef struct {
    const char *name;
    const char *src;
    const char *expected_node;  /* substring expected in s-expression */
} ShapeCheck;

static const ShapeCheck kChecks[] = {
    /* Previously confirmed broken */
    { "using declaration",                "{ using r = { [Symbol.dispose]() {} }; }",                                                "(using)" },
    { "await using declaration",          "async function f() { await using r = { [Symbol.asyncDispose]: async () => {} }; }",      "(using)" },
    { "accessor keyword",                 "class C { accessor x = 1; }",                                                            "(accessor)" },

    /* Previously confirmed working */
    { "import attributes",                "import data from \"./d.json\" with { type: \"json\" };",                                 "(import_attribute" },
    { "satisfies",                        "const x = { a: 1 } satisfies { a: number };",                                            "(satisfies_expression" },
    { "stage-3 decorator on method",      "function dec(_v: any, _c: any) {} class C { @dec method() {} }",                         "(decorator" },
    { "infer extends",                    "type First<T> = T extends [infer F extends string, ...any[]] ? F : never;",              "(infer_type" },
    { "const type parameter",             "function f<const T>(x: T): T { return x; }",                                             "(type_parameter" },

    /* New: TS-syntax constructs whiteout MUST strip/recognize */
    { "type-only import",                 "import type { Foo } from \"x\";",                                                        "import_statement" },
    { "inline type import specifier",     "import { type Foo, Bar } from \"x\";",                                                   "(import_specifier" },
    { "export type",                      "export type { Foo } from \"x\";",                                                        "export_statement" },
    { "as const",                         "const x = [1, 2, 3] as const;",                                                          "(as_expression" },
    { "non-null assertion",               "const x = foo()!.bar;",                                                                  "(non_null_expression" },
    { "override modifier",                "class B { m() {} } class C extends B { override m() {} }",                               "(override_modifier" },
    { "readonly modifier",                "class C { readonly x = 1; }",                                                            "(readonly" },
    { "variance in",                      "interface I<in T> { f(x: T): void; }",                                                   "(type_parameter" },
    { "variance out",                     "interface I<out T> { get(): T; }",                                                       "(type_parameter" },
    { "template literal type",            "type Greeting<T extends string> = `hello ${T}`;",                                        "(template_literal_type" },
    { "mapped type key remap (as)",       "type Getters<T> = { [K in keyof T as `get${string & K}`]: () => T[K] };",                "(mapped_type_clause" },
    { "tuple element label",              "type Pair = [first: number, second: string];",                                           "(labeled_tuple_type_element" },
    { "named tuple optional",             "type T = [first?: number, ...rest: string[]];",                                          "(labeled_tuple_type_element" },
    { "abstract class",                   "abstract class A { abstract foo(): void; }",                                             "(abstract_class_declaration" },
    { "declare global",                   "declare global { interface Window { x: number; } }",                                     "(ambient_declaration" },
    { "module augmentation",              "declare module \"x\" { interface Foo { extra: string; } }",                              "(ambient_declaration" },
    { "private class field",              "class C { #x = 1; get() { return this.#x; } }",                                          "(private_property_identifier" },
    { "class static block",               "class C { static x = 1; static { console.log(C.x); } }",                                 "(class_static_block" },
    { "decorator on class itself",        "function dec(_c: any) {} @dec class C {}",                                               "(decorator" },
    { "decorator on accessor",            "function dec(_v: any, _c: any) {} class C { @dec accessor x = 1; }",                     "(decorator" },
    { "satisfies + as chain",             "const x = ({ a: 1 } satisfies { a: number }) as { a: number };",                         "(as_expression" },
    { "generic arrow function",           "const f = <T,>(x: T): T => x;",                                                          "(arrow_function" },
    { "intersection type",                "type X = A & B & C;",                                                                    "(intersection_type" },
    { "union type",                       "type X = A | B | C;",                                                                    "(union_type" },
    { "conditional type",                 "type X<T> = T extends string ? \"s\" : \"o\";",                                          "(conditional_type" },
    { "readonly tuple",                   "type T = readonly [number, string];",                                                    "(readonly_type" },
    { "type predicate",                   "function isStr(x: unknown): x is string { return typeof x === \"string\"; }",            "(type_predicate" },
    { "asserts predicate",                "function check(x: unknown): asserts x is string {}",                                     "(asserts" },
};

int main(void) {
    TSParser *parser = ts_parser_new();
    if (!ts_parser_set_language(parser, tree_sitter_typescript())) {
        fprintf(stderr, "failed to set tree-sitter-typescript language\n");
        ts_parser_delete(parser);
        return 2;
    }

    size_t n = sizeof(kChecks) / sizeof(kChecks[0]);
    size_t pass = 0;
    int any_fail = 0;

    for (size_t i = 0; i < n; ++i) {
        const ShapeCheck *c = &kChecks[i];
        TSTree *tree = ts_parser_parse_string(parser, NULL, c->src, (uint32_t)strlen(c->src));
        TSNode root = ts_tree_root_node(tree);
        char *sexp = ts_node_string(root);

        int ok = strstr(sexp, c->expected_node) != NULL;
        if (ok) {
            printf("  ok    %-30s  contains %s\n", c->name, c->expected_node);
            pass++;
        } else {
            printf("  FAIL  %-30s  missing %s\n", c->name, c->expected_node);
            printf("        src:  %s\n", c->src);
            printf("        tree: %s\n", sexp);
            any_fail = 1;
        }
        free(sexp);
        ts_tree_delete(tree);
    }

    ts_parser_delete(parser);
    printf("\n%zu/%zu shape checks passed\n", pass, n);
    return any_fail ? 1 : 0;
}
