/*
 * ts_full_dump — print every child of every node, including anonymous tokens
 * (which ts_node_string omits). Used to verify whether keywords like
 * `readonly`, `accessor`, `using` are actually present in the parse tree.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tree_sitter/api.h>

const TSLanguage *tree_sitter_typescript(void);

static void dump(TSNode n, const char *src, int depth) {
    for (int i = 0; i < depth; ++i) fputs("  ", stdout);
    const char *type = ts_node_type(n);
    int named = ts_node_is_named(n);
    uint32_t s = ts_node_start_byte(n), e = ts_node_end_byte(n);
    uint32_t len = e - s;
    if (len > 40) len = 40;
    printf("%s%-30s [%u..%u] \"%.*s\"\n",
           named ? "" : "<anon> ", type, s, e, (int)len, src + s);
    uint32_t cc = ts_node_child_count(n);
    for (uint32_t i = 0; i < cc; ++i) {
        dump(ts_node_child(n, i), src, depth + 1);
    }
}

int main(int argc, char **argv) {
    const char *src = (argc > 1) ? argv[1] : "class C { readonly x = 1; }";
    TSParser *parser = ts_parser_new();
    ts_parser_set_language(parser, tree_sitter_typescript());
    TSTree *tree = ts_parser_parse_string(parser, NULL, src, (uint32_t)strlen(src));
    printf("SRC: %s\n\n", src);
    dump(ts_tree_root_node(tree), src, 0);
    ts_tree_delete(tree);
    ts_parser_delete(parser);
    return 0;
}
