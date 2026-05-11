#include "whiteout/whiteout.h"

#include <tree_sitter/api.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern "C" const TSLanguage *tree_sitter_typescript(void);

namespace {

using std::string_view;

struct Range { uint32_t start; uint32_t end; };

class Transformer {
public:
    Transformer(const char *src, uint32_t len) : src_(src), len_(len) {}

    void walk(TSNode node);

    whiteout_status status() const { return status_; }
    uint32_t err_offset() const { return err_offset_; }
    const std::string &err_msg() const { return err_msg_; }
    std::vector<Range> take_blanks() { return std::move(blanks_); }
    std::vector<uint32_t> take_semicolons() { return std::move(semicolons_); }
    std::vector<std::pair<uint32_t, char>> take_char_writes() { return std::move(char_writes_); }

private:
    void parse_error(TSNode node, const char *kind) {
        if (status_ != WHITEOUT_OK) return;
        status_ = WHITEOUT_ERR_PARSE;
        err_offset_ = ts_node_start_byte(node);
        err_msg_ = std::string("parse error: ") + kind;
    }
    void reject(TSNode node, const char *msg) {
        if (status_ != WHITEOUT_OK) return;
        status_ = WHITEOUT_ERR_UNSUPPORTED;
        err_offset_ = ts_node_start_byte(node);
        err_msg_ = msg;
    }
    void blank_node(TSNode n) { add(ts_node_start_byte(n), ts_node_end_byte(n)); }
    void add(uint32_t s, uint32_t e) { if (e > s) blanks_.push_back({s, e}); }

    void mark_char(uint32_t pos, char c) {
        if (pos < len_) char_writes_.push_back({pos, c});
    }

    void mark_semi_in(uint32_t s, uint32_t e) {
        for (uint32_t i = s; i < e && i < len_; ++i) {
            if (src_[i] != '\n' && src_[i] != '\r') { semicolons_.push_back(i); return; }
        }
    }

    uint32_t next_significant(uint32_t from) const {
        uint32_t i = from;
        while (i < len_) {
            char c = src_[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') { i++; continue; }
            if (c == '/' && i + 1 < len_) {
                char n = src_[i + 1];
                if (n == '/') {
                    while (i < len_ && src_[i] != '\n') i++;
                    continue;
                }
                if (n == '*') {
                    i += 2;
                    while (i + 1 < len_ && !(src_[i] == '*' && src_[i + 1] == '/')) i++;
                    i = (i + 1 < len_) ? i + 2 : len_;
                    continue;
                }
            }
            return i;
        }
        return len_;
    }

    // ts-blank-space inserts `;` for any top-level type-only blank unless
    // the previous significant content ends with `;` (or doesn't exist).
    // Scan forward from the file start tracking comment state to find the
    // last non-comment, non-whitespace character before `start_byte`.
    char last_significant_char_before(uint32_t start_byte) const {
        char last = 0;
        bool in_block = false, in_line = false;
        uint32_t limit = start_byte > len_ ? len_ : start_byte;
        for (uint32_t i = 0; i < limit; ++i) {
            char c = src_[i];
            if (in_line) { if (c == '\n') in_line = false; continue; }
            if (in_block) {
                if (c == '*' && i + 1 < limit && src_[i + 1] == '/') { i++; in_block = false; }
                continue;
            }
            if (c == '/' && i + 1 < limit) {
                char n = src_[i + 1];
                if (n == '/') { in_line = true; i++; continue; }
                if (n == '*') { in_block = true; i++; continue; }
            }
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') continue;
            last = c;
        }
        return last;
    }

    bool needs_leading_semi_before(uint32_t start_byte) const {
        char c = last_significant_char_before(start_byte);
        return c != 0 && c != ';';
    }

    // If `node`'s parent is a `class_body` and the next sibling under that
    // parent is an anonymous `;` token, return the end byte past the `;`.
    // Otherwise return `default_end`. Used to extend strip-full blanking to
    // absorb a class-member statement terminator that tree-sitter keeps as a
    // sibling (TypeScript's AST includes the terminator inside the member
    // node; matching that behavior makes whiteout's byte-output byte-equal
    // with ts-blank-space).
    uint32_t end_with_trailing_class_semi(TSNode node, uint32_t default_end) const {
        TSNode parent = ts_node_parent(node);
        if (ts_node_is_null(parent)) return default_end;
        if (tname(parent) != "class_body") return default_end;
        uint32_t pcc = ts_node_child_count(parent);
        int idx = -1;
        for (uint32_t i = 0; i < pcc; ++i) {
            if (ts_node_eq(ts_node_child(parent, i), node)) { idx = (int)i; break; }
        }
        if (idx < 0 || (uint32_t)(idx + 1) >= pcc) return default_end;
        TSNode next = ts_node_child(parent, (uint32_t)(idx + 1));
        if (!ts_node_is_named(next) && tname(next) == ";") {
            return ts_node_end_byte(next);
        }
        return default_end;
    }

    bool asi_hazard_at(uint32_t end_byte) const {
        // Only a hazard if there's a newline between `end_byte` and the next
        // significant char. Same-line `foo as T(args)` is a call in both
        // TypeScript and tree-sitter; blanking preserves call semantics so
        // no `;` is needed. Cross-line `foo as T\n(args)` is the case where
        // TS terminates the statement while tree-sitter would chain.
        bool saw_newline = false;
        for (uint32_t i = end_byte; i < len_; ++i) {
            char c = src_[i];
            if (c == '\n' || c == '\r') { saw_newline = true; continue; }
            if (c == ' ' || c == '\t' || c == '\f' || c == '\v') continue;
            if (c == '/' && i + 1 < len_) {
                char n = src_[i + 1];
                if (n == '/') {
                    while (i < len_ && src_[i] != '\n') i++;
                    if (i < len_) saw_newline = true;
                    continue;
                }
                if (n == '*') {
                    i += 2;
                    while (i + 1 < len_ && !(src_[i] == '*' && src_[i + 1] == '/')) {
                        if (src_[i] == '\n') saw_newline = true;
                        i++;
                    }
                    if (i + 1 < len_) i++;
                    continue;
                }
            }
            return saw_newline && (c == '(' || c == '[' || c == '`');
        }
        return false;
    }

    static string_view tname(TSNode n) { return string_view(ts_node_type(n)); }

    bool is_strip_full(string_view t) const {
        return t == "type_annotation"
            || t == "opting_type_annotation"
            || t == "omitting_type_annotation"
            || t == "type_predicate_annotation"
            || t == "asserts_annotation"
            || t == "type_parameters"
            || t == "type_arguments"
            || t == "abstract_method_signature"
            || t == "method_signature"
            || t == "function_signature"
            || t == "construct_signature"
            || t == "call_signature"
            || t == "index_signature"
            || t == "property_signature"
            || t == "implements_clause"
            || t == "extends_type_clause";
    }

    bool inside_constructor_params(TSNode param) const {
        TSNode formal = ts_node_parent(param);
        if (ts_node_is_null(formal)) return false;
        if (tname(formal) != "formal_parameters") return false;
        TSNode owner = ts_node_parent(formal);
        if (ts_node_is_null(owner)) return false;
        if (tname(owner) != "method_definition") return false;
        TSNode name = ts_node_child_by_field_name(owner, "name", 4);
        if (ts_node_is_null(name)) return false;
        uint32_t s = ts_node_start_byte(name);
        uint32_t e = ts_node_end_byte(name);
        return (e - s) == 11 && std::memcmp(src_ + s, "constructor", 11) == 0;
    }

    void walk_field(TSNode node);
    void walk_parameter(TSNode node);
    void walk_import_statement(TSNode node);
    void walk_export_statement(TSNode node);
    void walk_specifier(TSNode spec);
    void walk_class(TSNode node);
    void walk_method_definition(TSNode node);
    void walk_children_named(TSNode node);

    const char *src_;
    uint32_t len_;
    std::vector<Range> blanks_;
    std::vector<uint32_t> semicolons_;
    std::vector<std::pair<uint32_t, char>> char_writes_;
    whiteout_status status_ = WHITEOUT_OK;
    uint32_t err_offset_ = 0;
    std::string err_msg_;
};

void Transformer::walk_children_named(TSNode node) {
    uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc && status_ == WHITEOUT_OK; ++i) {
        TSNode c = ts_node_child(node, i);
        if (ts_node_is_named(c)) walk(c);
    }
}

void Transformer::walk(TSNode node) {
    if (status_ != WHITEOUT_OK) return;

    if (ts_node_is_error(node)) { parse_error(node, "unexpected token"); return; }
    if (ts_node_is_missing(node)) { parse_error(node, "missing token"); return; }

    string_view t = tname(node);

    if (t == "decorator")          { reject(node, "decorators are not supported in v1"); return; }
    if (t == "enum_declaration")   { reject(node, "enum is not supported (runtime-emitting)"); return; }
    if (t == "internal_module")    { reject(node, "namespace is not supported (runtime-emitting)"); return; }
    if (t == "module")             { reject(node, "module is not supported (runtime-emitting)"); return; }
    if (t == "import_alias")       { reject(node, "import = is not supported (runtime-emitting)"); return; }
    if (t == "type_assertion") {
        reject(node, "prefix `<T>x` type assertion is not supported; use `x as T` instead");
        return;
    }

    // Statement-level type-only declarations: blanking creates an empty span
    // where a statement used to be. Insert `;` if either:
    //   - the previous significant content does not already end with `;`
    //     (matches ts-blank-space's defensive ASI behavior), or
    //   - the next significant token would chain via JS ASI (`(`/`[`/`` ` ``).
    if (t == "interface_declaration"
        || t == "type_alias_declaration"
        || t == "ambient_declaration") {
        uint32_t s = ts_node_start_byte(node);
        uint32_t e = ts_node_end_byte(node);
        blank_node(node);
        if (needs_leading_semi_before(s)) mark_semi_in(s, e);
        return;
    }

    if (is_strip_full(t)) {
        uint32_t s = ts_node_start_byte(node);
        uint32_t e = ts_node_end_byte(node);
        // Only class-body members participate in the `blankStatement` rule:
        // extend the blank to absorb the trailing `;` sibling that
        // tree-sitter keeps outside the member node, and prepend `;` when
        // the previous statement-level content does not already end with
        // `;`. For non-class-body strip-full nodes (e.g. `type_parameters`
        // inside a function declaration), no semicolon shaping applies.
        TSNode parent = ts_node_parent(node);
        if (!ts_node_is_null(parent) && tname(parent) == "class_body") {
            e = end_with_trailing_class_semi(node, e);
            add(s, e);
            if (needs_leading_semi_before(s)) mark_semi_in(s, e);
        } else {
            blank_node(node);
        }
        return;
    }

    if (t == "arrow_function") {
        TSNode tp = ts_node_child_by_field_name(node, "type_parameters", 15);
        TSNode rt = ts_node_child_by_field_name(node, "return_type", 11);
        TSNode params = ts_node_child_by_field_name(node, "parameters", 10);

        // Locate the `=>` token early so we can use it both for the multi-line
        // detection and the post-swap validity check.
        TSNode arrow{};
        bool have_arrow = false;
        uint32_t cc_arrow = ts_node_child_count(node);
        for (uint32_t i = 0; i < cc_arrow; ++i) {
            TSNode c = ts_node_child(node, i);
            if (!ts_node_is_named(c) && tname(c) == "=>") {
                arrow = c;
                have_arrow = true;
                break;
            }
        }

        auto contains_newline = [&](uint32_t a, uint32_t b) {
            for (uint32_t i = a; i < b && i < len_; ++i) {
                if (src_[i] == '\n' || src_[i] == '\r') return true;
            }
            return false;
        };

        // The swap is needed whenever blanking the type material would leave
        // a newline between (preceding context) and `(`, or between `)` and
        // `=>`. That happens when:
        //   - type_parameters exists and a newline sits anywhere between its
        //     start and the parameter list's `(`, or
        //   - return_type exists and a newline sits between the parameter
        //     list's `)` and `=>`.
        bool tp_multi = false;
        bool rt_multi = false;
        if (!ts_node_is_null(tp) && !ts_node_is_null(params)) {
            tp_multi = contains_newline(ts_node_start_byte(tp),
                                        ts_node_start_byte(params) + 1);
        }
        if (!ts_node_is_null(rt) && !ts_node_is_null(params) && have_arrow) {
            rt_multi = contains_newline(ts_node_end_byte(params),
                                        ts_node_start_byte(arrow));
        }

        if (tp_multi || rt_multi) {
            // The `(arglist) => body` rule in JS forbids a LineTerminator
            // between `)` and `=>`. Naively blanking a multi-line type
            // would leave a newline there, which is a syntax error.
            //
            // Fix: swap the type's outer delimiters with the parameter list's
            // delimiters, byte-for-byte. The `<` of multi-line type parameters
            // becomes `(`, the matching `>` of a multi-line return type becomes
            // `)`, and the original `(`/`)` of the parameter list are blanked.
            // Every retained identifier keeps its original column; only the
            // parenthesis characters move.
            if (ts_node_is_null(params)) {
                reject(tp_multi ? tp : rt,
                       "multi-line type on arrow function without `()` parameter list cannot be rewritten safely");
                return;
            }

            if (!have_arrow) {
                reject(node, "internal: arrow function without `=>` token");
                return;
            }
            uint32_t params_start = ts_node_start_byte(params);
            uint32_t params_end = ts_node_end_byte(params);

            if (tp_multi) {
                // `<` is at tp_start; rewrite it to `(`. Blank the type
                // parameter list's body (the chars between `<` and `>`,
                // plus the closing `>` itself) and the original `(` of the
                // parameter list. Leave comments between `>` and `(` alone,
                // matching ts-blank-space.
                uint32_t tp_start = ts_node_start_byte(tp);
                uint32_t tp_end = ts_node_end_byte(tp);
                if (src_[tp_start] != '<') {
                    reject(tp, "internal: type_parameters does not start with `<`");
                    return;
                }
                mark_char(tp_start, '(');
                add(tp_start + 1, tp_end);
                add(params_start, params_start + 1);
            }

            if (rt_multi) {
                uint32_t rt_start = ts_node_start_byte(rt);
                uint32_t rt_end = ts_node_end_byte(rt);
                uint32_t arrow_row = ts_node_start_point(arrow).row;

                // Find the rightmost non-whitespace, non-comment byte inside
                // return_type that sits on the same line as `=>`. That byte
                // becomes `)` and the rest of return_type is blanked. If no
                // such byte exists (return_type ends before the `=>` line
                // with only whitespace/newlines between), we cannot keep `)`
                // adjacent to `=>` and must reject.
                uint32_t current_row = ts_node_start_point(rt).row;
                bool in_line_c = false;
                bool in_block_c = false;
                uint32_t close_byte = UINT32_MAX;
                for (uint32_t i = rt_start; i < rt_end; ++i) {
                    char c = src_[i];
                    if (in_line_c) {
                        if (c == '\n') { in_line_c = false; current_row++; }
                        continue;
                    }
                    if (in_block_c) {
                        if (c == '*' && i + 1 < rt_end && src_[i + 1] == '/') {
                            in_block_c = false; i++;
                        } else if (c == '\n') {
                            current_row++;
                        }
                        continue;
                    }
                    if (c == '\n') { current_row++; continue; }
                    if (c == ' ' || c == '\t' || c == '\r') continue;
                    if (c == '/' && i + 1 < rt_end) {
                        if (src_[i + 1] == '/') { in_line_c = true; i++; continue; }
                        if (src_[i + 1] == '*') { in_block_c = true; i++; continue; }
                    }
                    if (current_row == arrow_row) close_byte = i;
                }
                if (close_byte == UINT32_MAX) {
                    reject(rt, "cannot place `)` adjacent to `=>`; the multi-line return type has no significant character on the same line as `=>`. Move some part of the return type onto the same line as `=>`, or rewrite the return type on a single line.");
                    return;
                }
                // Blank the original `)`, any comments and whitespace
                // between it and `:`, the return-type body, and trailing
                // whitespace, leaving the swap byte as `)`. ts-blank-space
                // erases comments between `)` and the return-type annotation
                // (unlike the tp side where comments between `>` and `(`
                // survive).
                add(params_end - 1, close_byte);
                mark_char(close_byte, ')');
                add(close_byte + 1, rt_end);
            }

            // Walk the remaining children (the formal_parameters body to strip
            // any annotations there, the arrow body, etc.). Skip the type
            // parameters and return type, which we already handled.
            for (uint32_t i = 0; i < cc_arrow && status_ == WHITEOUT_OK; ++i) {
                TSNode c = ts_node_child(node, i);
                if (!ts_node_is_named(c)) continue;
                if (!ts_node_is_null(tp) && ts_node_eq(c, tp)) continue;
                if (!ts_node_is_null(rt) && ts_node_eq(c, rt)) continue;
                walk(c);
            }
            return;
        }

        walk_children_named(node);
        return;
    }

    if (t == "import_statement")        { walk_import_statement(node); return; }
    if (t == "export_statement")        { walk_export_statement(node); return; }
    if (t == "import_specifier" || t == "export_specifier") { walk_specifier(node); return; }
    if (t == "class_declaration" || t == "abstract_class_declaration") { walk_class(node); return; }
    if (t == "method_definition")       { walk_method_definition(node); return; }
    if (t == "public_field_definition") { walk_field(node); return; }
    if (t == "required_parameter" || t == "optional_parameter") { walk_parameter(node); return; }

    if (t == "variable_declarator") {
        // Strip `!` (definite-assignment) on `let`/`var`/`const` declarators
        // — it sits between the identifier and the type annotation as an
        // anonymous token and TS allows it on any variable, not just fields.
        uint32_t cc = ts_node_child_count(node);
        for (uint32_t i = 0; i < cc && status_ == WHITEOUT_OK; ++i) {
            TSNode c = ts_node_child(node, i);
            if (!ts_node_is_named(c) && tname(c) == "!") {
                blank_node(c);
            } else if (ts_node_is_named(c)) {
                walk(c);
            }
        }
        return;
    }

    if (t == "as_expression" || t == "satisfies_expression") {
        // Tree-sitter binds `foo as T\n(args)` as a single call expression
        // (per JS spec ASI), while TypeScript's own parser treats the
        // newline-then-`(`/`[`/`` ` `` as a statement boundary. To preserve
        // TS's runtime semantics in the blanked output, insert `;` in the
        // blanked `as T` range when the next significant character would
        // otherwise chain into the expression.
        if (ts_node_named_child_count(node) >= 1) {
            TSNode expr = ts_node_named_child(node, 0);
            walk(expr);
            uint32_t blank_start = ts_node_end_byte(expr);
            uint32_t blank_end = ts_node_end_byte(node);

            // Asymmetric case: `foo as T\n[N]` is parsed as `as_expression`
            // whose type is `lookup_type` or `array_type`, swallowing the
            // `[N]` runtime continuation. Detect a `[` token within the type
            // that sits on a different line than the type's start; cut the
            // blank at the end of the base type (preserving any trailing
            // comment before the newline) and force `;` insertion so the
            // runtime `[N]` remains as a separate statement.
            if (ts_node_named_child_count(node) >= 2) {
                TSNode type = ts_node_named_child(node, 1);
                string_view tt = tname(type);
                if ((tt == "lookup_type" || tt == "array_type")
                    && ts_node_named_child_count(type) >= 1) {
                    TSNode base = ts_node_named_child(type, 0);
                    uint32_t tcc = ts_node_child_count(type);
                    for (uint32_t i = 0; i < tcc; ++i) {
                        TSNode c = ts_node_child(type, i);
                        if (!ts_node_is_named(c) && tname(c) == "[") {
                            uint32_t br = ts_node_start_byte(c);
                            bool newline = false;
                            for (uint32_t j = ts_node_start_byte(type);
                                 j < br && j < len_; ++j) {
                                if (src_[j] == '\n' || src_[j] == '\r') {
                                    newline = true; break;
                                }
                            }
                            if (newline) {
                                blank_end = ts_node_end_byte(base);
                                add(blank_start, blank_end);
                                mark_semi_in(blank_start, blank_end);
                                return;
                            }
                            break;
                        }
                    }
                }
            }

            add(blank_start, blank_end);
            if (asi_hazard_at(blank_end)) mark_semi_in(blank_start, blank_end);
        }
        return;
    }
    if (t == "non_null_expression") {
        if (ts_node_named_child_count(node) >= 1) {
            TSNode expr = ts_node_named_child(node, 0);
            walk(expr);
            add(ts_node_end_byte(expr), ts_node_end_byte(node));
        }
        return;
    }

    walk_children_named(node);
}

void Transformer::walk_parameter(TSNode node) {
    uint32_t cc = ts_node_child_count(node);
    bool has_pp_modifier = false;
    bool is_this_param = false;
    for (uint32_t i = 0; i < cc; ++i) {
        TSNode c = ts_node_child(node, i);
        string_view ct = tname(c);
        if (ts_node_is_named(c)) {
            if (ct == "decorator") { reject(c, "decorators are not supported in v1"); return; }
            if (ct == "accessibility_modifier" || ct == "override_modifier") has_pp_modifier = true;
            if (ct == "this") is_this_param = true;
        } else {
            if (ct == "readonly") has_pp_modifier = true;
        }
    }
    if (has_pp_modifier && inside_constructor_params(node)) {
        reject(node, "parameter properties are not supported in v1");
        return;
    }
    if (is_this_param) {
        // `function f(this: T, ...)` — the `this` parameter is purely TS, with
        // no JS equivalent. Blank the whole parameter and a trailing comma if
        // present so the remaining parameter list stays well-formed.
        uint32_t s = ts_node_start_byte(node);
        uint32_t e = ts_node_end_byte(node);
        TSNode parent = ts_node_parent(node);
        if (!ts_node_is_null(parent)) {
            uint32_t pcc = ts_node_child_count(parent);
            int idx = -1;
            for (uint32_t i = 0; i < pcc; ++i) {
                if (ts_node_eq(ts_node_child(parent, i), node)) { idx = (int)i; break; }
            }
            if (idx >= 0 && (uint32_t)(idx + 1) < pcc) {
                TSNode next = ts_node_child(parent, (uint32_t)(idx + 1));
                if (!ts_node_is_named(next) && tname(next) == ",") {
                    e = ts_node_end_byte(next);
                }
            }
        }
        add(s, e);
        return;
    }
    for (uint32_t i = 0; i < cc && status_ == WHITEOUT_OK; ++i) {
        TSNode c = ts_node_child(node, i);
        string_view ct = tname(c);
        if (ts_node_is_named(c)) {
            if (ct == "accessibility_modifier" || ct == "override_modifier") {
                blank_node(c);
            } else {
                walk(c);
            }
        } else {
            if (ct == "readonly" || ct == "?") blank_node(c);
        }
    }
}

void Transformer::walk_field(TSNode node) {
    uint32_t cc = ts_node_child_count(node);

    // `declare` or `abstract` on a class field: tsc emits no field for these.
    // Blanking only the modifier would produce `class C { x; }` which under
    // useDefineForClassFields creates an own property the author did not want.
    for (uint32_t i = 0; i < cc; ++i) {
        TSNode c = ts_node_child(node, i);
        if (!ts_node_is_named(c)) {
            string_view ct = tname(c);
            if (ct == "declare" || ct == "abstract") {
                uint32_t s = ts_node_start_byte(node);
                uint32_t e = end_with_trailing_class_semi(node, ts_node_end_byte(node));
                add(s, e);
                if (needs_leading_semi_before(s)) mark_semi_in(s, e);
                return;
            }
        }
    }
    for (uint32_t i = 0; i < cc; ++i) {
        TSNode c = ts_node_child(node, i);
        if (ts_node_is_named(c) && tname(c) == "decorator") {
            reject(c, "decorators are not supported in v1");
            return;
        }
    }

    // If the leftmost child is a TS-only modifier we are about to blank and
    // the member's name is a `[computed]` form, mark `;` in the modifier's
    // bytes. Otherwise the previous member's expression value can chain into
    // `[` post-blanking (e.g. `f = 1` + `public ["x"]` → `f = 1["x"]`).
    if (cc > 0) {
        TSNode first = ts_node_child(node, 0);
        string_view ft = tname(first);
        bool first_is_ts_mod = ts_node_is_named(first)
            ? (ft == "accessibility_modifier" || ft == "override_modifier")
            : (ft == "readonly");
        if (first_is_ts_mod) {
            TSNode name = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name) && tname(name) == "computed_property_name") {
                mark_semi_in(ts_node_start_byte(first), ts_node_end_byte(first));
            }
        }
    }

    for (uint32_t i = 0; i < cc && status_ == WHITEOUT_OK; ++i) {
        TSNode c = ts_node_child(node, i);
        string_view ct = tname(c);
        if (ts_node_is_named(c)) {
            if (ct == "accessibility_modifier" || ct == "override_modifier") {
                blank_node(c);
            } else {
                walk(c);
            }
        } else {
            // `?` is the optional-field marker, `!` is the definite-assignment
            // assertion. Both are TS-only and must be blanked alongside the
            // type annotation. `readonly`/`abstract` are type-position modifiers.
            if (ct == "readonly" || ct == "abstract" || ct == "?" || ct == "!") blank_node(c);
        }
    }
}

void Transformer::walk_method_definition(TSNode node) {
    uint32_t cc = ts_node_child_count(node);

    if (cc > 0) {
        TSNode first = ts_node_child(node, 0);
        string_view ft = tname(first);
        bool first_is_ts_mod = ts_node_is_named(first)
            ? (ft == "accessibility_modifier" || ft == "override_modifier")
            : (ft == "readonly" || ft == "abstract" || ft == "declare");
        if (first_is_ts_mod) {
            TSNode name = ts_node_child_by_field_name(node, "name", 4);
            if (!ts_node_is_null(name) && tname(name) == "computed_property_name") {
                mark_semi_in(ts_node_start_byte(first), ts_node_end_byte(first));
            }
        }
    }

    for (uint32_t i = 0; i < cc && status_ == WHITEOUT_OK; ++i) {
        TSNode c = ts_node_child(node, i);
        string_view ct = tname(c);
        if (ts_node_is_named(c)) {
            if (ct == "decorator") { reject(c, "decorators are not supported in v1"); return; }
            if (ct == "accessibility_modifier" || ct == "override_modifier") {
                blank_node(c);
            } else {
                walk(c);
            }
        } else {
            // `?` is the optional-method marker on a method_definition.
            if (ct == "readonly" || ct == "abstract" || ct == "declare" || ct == "?") blank_node(c);
        }
    }
}

void Transformer::walk_class(TSNode node) {
    uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc && status_ == WHITEOUT_OK; ++i) {
        TSNode c = ts_node_child(node, i);
        string_view ct = tname(c);
        if (ts_node_is_named(c)) {
            if (ct == "decorator") { reject(c, "decorators are not supported in v1"); return; }
            walk(c);
        } else {
            if (ct == "abstract" || ct == "declare") blank_node(c);
        }
    }
}

void Transformer::walk_import_statement(TSNode node) {
    uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc; ++i) {
        TSNode c = ts_node_child(node, i);
        if (!ts_node_is_named(c) && tname(c) == "type") {
            uint32_t s = ts_node_start_byte(node);
            uint32_t e = ts_node_end_byte(node);
            blank_node(node);
            if (needs_leading_semi_before(s)) mark_semi_in(s, e);
            return;
        }
    }
    for (uint32_t i = 0; i < cc; ++i) {
        TSNode c = ts_node_child(node, i);
        if (ts_node_is_named(c) && tname(c) == "import_require_clause") {
            reject(node, "import = is not supported (runtime-emitting)");
            return;
        }
    }
    walk_children_named(node);
}

void Transformer::walk_export_statement(TSNode node) {
    uint32_t cc = ts_node_child_count(node);
    for (uint32_t i = 0; i < cc; ++i) {
        TSNode c = ts_node_child(node, i);
        if (!ts_node_is_named(c) && tname(c) == "=") {
            reject(node, "export = is not supported (runtime-emitting)");
            return;
        }
    }
    for (uint32_t i = 0; i < cc; ++i) {
        TSNode c = ts_node_child(node, i);
        if (!ts_node_is_named(c) && tname(c) == "type") {
            uint32_t s = ts_node_start_byte(node);
            uint32_t e = ts_node_end_byte(node);
            blank_node(node);
            if (needs_leading_semi_before(s)) mark_semi_in(s, e);
            return;
        }
    }
    // `export interface I {}`, `export type T = ...`, `export declare ...`
    // — no runtime emit, blank the entire statement including `export`.
    for (uint32_t i = 0; i < cc; ++i) {
        TSNode c = ts_node_child(node, i);
        if (ts_node_is_named(c)) {
            string_view ct = tname(c);
            if (ct == "type_alias_declaration" || ct == "interface_declaration"
                || ct == "ambient_declaration") {
                uint32_t s = ts_node_start_byte(node);
                uint32_t e = ts_node_end_byte(node);
                blank_node(node);
                if (needs_leading_semi_before(s)) mark_semi_in(s, e);
                return;
            }
        }
    }
    walk_children_named(node);
}

void Transformer::walk_specifier(TSNode spec) {
    // import_specifier / export_specifier. If a leading `type` keyword is
    // present, blank the specifier plus one adjacent comma so the surrounding
    // named-import/export clause remains valid JS.
    uint32_t cc = ts_node_child_count(spec);
    bool has_type = false;
    for (uint32_t i = 0; i < cc; ++i) {
        TSNode c = ts_node_child(spec, i);
        if (!ts_node_is_named(c) && tname(c) == "type") { has_type = true; break; }
    }
    if (!has_type) return;

    // Scan the source for the adjacent comma, skipping whitespace and
    // comments — those can sit between the specifier and the `,` in the
    // input (e.g. `import { type X/**/, Y }`).
    uint32_t s = ts_node_start_byte(spec);
    uint32_t e = ts_node_end_byte(spec);

    bool took_trailing = false;
    uint32_t i = e;
    while (i < len_) {
        char c = src_[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') { i++; continue; }
        if (c == '/' && i + 1 < len_) {
            char n = src_[i + 1];
            if (n == '/') { while (i < len_ && src_[i] != '\n') i++; continue; }
            if (n == '*') {
                i += 2;
                while (i + 1 < len_ && !(src_[i] == '*' && src_[i + 1] == '/')) i++;
                if (i + 1 < len_) i += 2; else i = len_;
                continue;
            }
        }
        if (c == ',') { e = i + 1; took_trailing = true; }
        break;
    }
    if (!took_trailing) {
        // No trailing comma — sweep backwards for a leading comma (skipping
        // whitespace/comments in the same way).
        uint32_t j = s;
        while (j > 0) {
            char c = src_[j - 1];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') { j--; continue; }
            if (j >= 2 && src_[j - 2] == '*' && src_[j - 1] == '/') {
                j -= 2;
                while (j > 0 && !(src_[j - 1] == '*' && j >= 2 && src_[j - 2] == '/')) j--;
                if (j >= 2) j -= 2;
                continue;
            }
            if (c == ',') s = j - 1;
            break;
        }
    }
    add(s, e);
}

size_t validate_utf8(const char *src, size_t len) {
    const auto *p = reinterpret_cast<const unsigned char *>(src);
    size_t i = 0;
    while (i < len) {
        unsigned char b = p[i];
        if (b < 0x80) { i++; continue; }
        size_t need;
        unsigned int cp;
        if ((b & 0xE0) == 0xC0) {
            if (b < 0xC2) return i;
            need = 1; cp = b & 0x1F;
        } else if ((b & 0xF0) == 0xE0) {
            need = 2; cp = b & 0x0F;
        } else if ((b & 0xF8) == 0xF0) {
            if (b > 0xF4) return i;
            need = 3; cp = b & 0x07;
        } else {
            return i;
        }
        if (i + need >= len) return i;
        for (size_t k = 1; k <= need; ++k) {
            unsigned char c = p[i + k];
            if ((c & 0xC0) != 0x80) return i;
            cp = (cp << 6) | (c & 0x3F);
        }
        if (need == 1 && cp < 0x80) return i;
        if (need == 2 && cp < 0x800) return i;
        if (need == 3 && (cp < 0x10000 || cp > 0x10FFFF)) return i;
        if (cp >= 0xD800 && cp <= 0xDFFF) return i;
        i += need + 1;
    }
    return len;
}

} // namespace

struct whiteout_ctx {
    TSParser *parser;
    std::string message;
};

extern "C" whiteout_ctx *whiteout_ctx_new(void) {
    auto *ctx = new (std::nothrow) whiteout_ctx{};
    if (!ctx) return nullptr;
    ctx->parser = ts_parser_new();
    if (!ctx->parser) { delete ctx; return nullptr; }
    if (!ts_parser_set_language(ctx->parser, tree_sitter_typescript())) {
        ts_parser_delete(ctx->parser);
        delete ctx;
        return nullptr;
    }
    return ctx;
}

extern "C" void whiteout_ctx_free(whiteout_ctx *ctx) {
    if (!ctx) return;
    if (ctx->parser) ts_parser_delete(ctx->parser);
    delete ctx;
}

extern "C" void whiteout_free(char *buf) {
    std::free(buf);
}

namespace {

void set_err(whiteout_ctx *ctx, whiteout_error *err,
             whiteout_status st, std::string msg, size_t off) {
    if (ctx) ctx->message = std::move(msg);
    if (err) {
        err->status = st;
        err->message = ctx ? ctx->message.c_str() : "";
        err->offset = off;
    }
}

bool find_first_parse_error(TSNode root, TSNode *bad, const char **kind) {
    enum { STACK_MAX = 4096 };
    TSNode stack[STACK_MAX];
    int top = 0;
    stack[top++] = root;
    while (top > 0) {
        TSNode n = stack[--top];
        if (ts_node_is_error(n))   { *bad = n; *kind = "unexpected token"; return true; }
        if (ts_node_is_missing(n)) { *bad = n; *kind = "missing token";    return true; }
        uint32_t cc = ts_node_child_count(n);
        for (uint32_t i = 0; i < cc && top < STACK_MAX; ++i) {
            stack[top++] = ts_node_child(n, i);
        }
    }
    return false;
}

void line_col_for_offset(const char *src, size_t len, size_t offset,
                         size_t *line, size_t *col) {
    size_t l = 1, c = 1;
    size_t bound = offset < len ? offset : len;
    for (size_t i = 0; i < bound; ++i) {
        if (src[i] == '\n') { l++; c = 1; }
        else { c++; }
    }
    *line = l; *col = c;
}

// Look at the bytes preceding `offset` (back to the previous newline, or up
// to a small window) for an unparenthesised `as` or `satisfies` keyword on
// the same logical line. Used to tailor parse-error messages.
bool preceded_by_assertion_keyword(const char *src, size_t len, size_t offset) {
    if (offset > len) offset = len;
    size_t start = offset;
    // Walk back to the previous newline, but cap the lookback so we don't
    // scan an entire file.
    size_t cap = (offset > 128) ? offset - 128 : 0;
    while (start > cap && src[start - 1] != '\n') start--;
    std::string_view window(src + start, offset - start);
    auto contains_word = [&](std::string_view word) {
        size_t pos = 0;
        while ((pos = window.find(word, pos)) != std::string_view::npos) {
            bool left_ok  = (pos == 0) || !(isalnum((unsigned char)window[pos - 1])
                                            || window[pos - 1] == '_' || window[pos - 1] == '$');
            size_t end = pos + word.size();
            bool right_ok = (end == window.size())
                            || !(isalnum((unsigned char)window[end])
                                 || window[end] == '_' || window[end] == '$');
            if (left_ok && right_ok) return true;
            pos = end;
        }
        return false;
    };
    return contains_word("as") || contains_word("satisfies");
}

std::string format_parse_error(const char *src, size_t len, size_t offset,
                               const char *kind) {
    size_t line = 0, col = 0;
    line_col_for_offset(src, len, offset, &line, &col);
    std::string msg = "parse error at line " + std::to_string(line)
                    + ":" + std::to_string(col) + ": " + kind;
    if (preceded_by_assertion_keyword(src, len, offset)) {
        msg += "; if the preceding `as` or `satisfies` expression is meant to "
               "end here, add an explicit `;` before the newline "
               "(tree-sitter's grammar binds the next `(`/`[` as a continuation, "
               "unlike TypeScript's own parser)";
    }
    return msg;
}

// Core pipeline: validate UTF-8, parse, walk, then mutate `dst` in place.
// `dst` may alias `src`; all reads of `src` finish before any write to `dst`.
// On any non-OK return, no bytes of `dst` have been written.
whiteout_status do_transform(
    whiteout_ctx *ctx,
    const char *src, size_t src_len,
    char *dst,
    whiteout_error *err)
{
    size_t bad = validate_utf8(src, src_len);
    if (bad != src_len) {
        set_err(ctx, err, WHITEOUT_ERR_UTF8, "invalid UTF-8", bad);
        return WHITEOUT_ERR_UTF8;
    }

    TSTree *tree = ts_parser_parse_string(ctx->parser, nullptr, src,
                                          static_cast<uint32_t>(src_len));
    if (!tree) {
        set_err(ctx, err, WHITEOUT_ERR_INTERNAL, "tree-sitter parse returned null", 0);
        return WHITEOUT_ERR_INTERNAL;
    }

    TSNode root = ts_tree_root_node(tree);

    TSNode bad_node{};
    const char *kind = "";
    if (find_first_parse_error(root, &bad_node, &kind)) {
        size_t off = ts_node_start_byte(bad_node);
        set_err(ctx, err, WHITEOUT_ERR_PARSE,
                format_parse_error(src, src_len, off, kind), off);
        ts_tree_delete(tree);
        return WHITEOUT_ERR_PARSE;
    }

    Transformer xf(src, static_cast<uint32_t>(src_len));
    xf.walk(root);

    if (xf.status() != WHITEOUT_OK) {
        whiteout_status st = xf.status();
        size_t off = xf.err_offset();
        std::string m = xf.err_msg();
        set_err(ctx, err, st, std::move(m), off);
        ts_tree_delete(tree);
        return st;
    }

    // Past here we mutate dst. The loops below only read dst; src is no longer
    // touched, so dst aliasing src is safe.
    if (dst != src) std::memcpy(dst, src, src_len);

    auto blanks = xf.take_blanks();
    std::sort(blanks.begin(), blanks.end(),
              [](const Range &a, const Range &b) { return a.start < b.start; });
    for (const auto &r : blanks) {
        uint32_t s = r.start;
        uint32_t e = r.end > src_len ? static_cast<uint32_t>(src_len) : r.end;
        for (uint32_t i = s; i < e; ++i) {
            if (dst[i] != '\n' && dst[i] != '\r') dst[i] = ' ';
        }
    }
    for (uint32_t pos : xf.take_semicolons()) {
        if (pos < src_len && dst[pos] != '\n' && dst[pos] != '\r') dst[pos] = ';';
    }
    for (auto [pos, c] : xf.take_char_writes()) {
        if (pos < src_len && dst[pos] != '\n' && dst[pos] != '\r') dst[pos] = c;
    }

    ts_tree_delete(tree);
    return WHITEOUT_OK;
}

} // namespace

extern "C" whiteout_status whiteout_transform(
    whiteout_ctx *ctx,
    const char *src, size_t src_len,
    char **out, size_t *out_len,
    whiteout_error *err)
{
    if (out) *out = nullptr;
    if (out_len) *out_len = 0;
    if (err) { err->status = WHITEOUT_OK; err->message = ""; err->offset = 0; }
    if (!ctx || !src || !out || !out_len) {
        set_err(ctx, err, WHITEOUT_ERR_INTERNAL, "null argument", 0);
        return WHITEOUT_ERR_INTERNAL;
    }
    if (src_len > UINT32_MAX) {
        set_err(ctx, err, WHITEOUT_ERR_INTERNAL, "input too large", 0);
        return WHITEOUT_ERR_INTERNAL;
    }

    char *buf = static_cast<char *>(std::malloc(src_len ? src_len : 1));
    if (!buf) {
        set_err(ctx, err, WHITEOUT_ERR_ALLOC, "allocation failed", 0);
        return WHITEOUT_ERR_ALLOC;
    }

    whiteout_status st = do_transform(ctx, src, src_len, buf, err);
    if (st != WHITEOUT_OK) {
        std::free(buf);
        return st;
    }

    *out = buf;
    *out_len = src_len;
    return WHITEOUT_OK;
}

extern "C" whiteout_status whiteout_transform_inplace(
    whiteout_ctx *ctx,
    char *buf, size_t len,
    whiteout_error *err)
{
    if (err) { err->status = WHITEOUT_OK; err->message = ""; err->offset = 0; }
    if (!ctx || (!buf && len > 0)) {
        set_err(ctx, err, WHITEOUT_ERR_INTERNAL, "null argument", 0);
        return WHITEOUT_ERR_INTERNAL;
    }
    if (len > UINT32_MAX) {
        set_err(ctx, err, WHITEOUT_ERR_INTERNAL, "input too large", 0);
        return WHITEOUT_ERR_INTERNAL;
    }
    return do_transform(ctx, buf, len, buf, err);
}
