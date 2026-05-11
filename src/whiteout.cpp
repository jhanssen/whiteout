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
    Transformer(const char *src, uint32_t /*len*/) : src_(src) {}

    void walk(TSNode node);

    whiteout_status status() const { return status_; }
    uint32_t err_offset() const { return err_offset_; }
    const std::string &err_msg() const { return err_msg_; }
    std::vector<Range> take_blanks() { return std::move(blanks_); }

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

    static string_view tname(TSNode n) { return string_view(ts_node_type(n)); }

    bool is_strip_full(string_view t) const {
        return t == "type_annotation"
            || t == "opting_type_annotation"
            || t == "omitting_type_annotation"
            || t == "type_predicate_annotation"
            || t == "asserts_annotation"
            || t == "type_parameters"
            || t == "type_arguments"
            || t == "interface_declaration"
            || t == "type_alias_declaration"
            || t == "ambient_declaration"
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
    std::vector<Range> blanks_;
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

    if (is_strip_full(t)) { blank_node(node); return; }

    if (t == "import_statement")        { walk_import_statement(node); return; }
    if (t == "export_statement")        { walk_export_statement(node); return; }
    if (t == "import_specifier" || t == "export_specifier") { walk_specifier(node); return; }
    if (t == "class_declaration" || t == "abstract_class_declaration") { walk_class(node); return; }
    if (t == "method_definition")       { walk_method_definition(node); return; }
    if (t == "public_field_definition") { walk_field(node); return; }
    if (t == "required_parameter" || t == "optional_parameter") { walk_parameter(node); return; }

    if (t == "as_expression" || t == "satisfies_expression") {
        if (ts_node_named_child_count(node) >= 1) {
            TSNode expr = ts_node_named_child(node, 0);
            walk(expr);
            add(ts_node_end_byte(expr), ts_node_end_byte(node));
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
    if (t == "type_assertion") {
        // `<T>expr`: type_arguments first in source order, expression last.
        uint32_t ncc = ts_node_named_child_count(node);
        if (ncc >= 1) {
            TSNode expr = ts_node_named_child(node, ncc - 1);
            add(ts_node_start_byte(node), ts_node_start_byte(expr));
            walk(expr);
        } else {
            blank_node(node);
        }
        return;
    }

    walk_children_named(node);
}

void Transformer::walk_parameter(TSNode node) {
    uint32_t cc = ts_node_child_count(node);
    bool has_pp_modifier = false;
    for (uint32_t i = 0; i < cc; ++i) {
        TSNode c = ts_node_child(node, i);
        string_view ct = tname(c);
        if (ts_node_is_named(c)) {
            if (ct == "decorator") { reject(c, "decorators are not supported in v1"); return; }
            if (ct == "accessibility_modifier" || ct == "override_modifier") has_pp_modifier = true;
        } else {
            if (ct == "readonly") has_pp_modifier = true;
        }
    }
    if (has_pp_modifier && inside_constructor_params(node)) {
        reject(node, "parameter properties are not supported in v1");
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
    for (uint32_t i = 0; i < cc; ++i) {
        TSNode c = ts_node_child(node, i);
        if (!ts_node_is_named(c)) {
            string_view ct = tname(c);
            // `declare` or `abstract` on a class field: tsc emits no field for these.
            // Blanking only the modifier would produce `class C { x; }` which under
            // useDefineForClassFields creates an own property the author did not want.
            if (ct == "declare" || ct == "abstract") { blank_node(node); return; }
        }
    }
    for (uint32_t i = 0; i < cc; ++i) {
        TSNode c = ts_node_child(node, i);
        if (ts_node_is_named(c) && tname(c) == "decorator") {
            reject(c, "decorators are not supported in v1");
            return;
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
            if (ct == "readonly" || ct == "abstract" || ct == "declare") blank_node(c);
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
            blank_node(node);
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
            blank_node(node);
            return;
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

    TSNode parent = ts_node_parent(spec);
    uint32_t s = ts_node_start_byte(spec);
    uint32_t e = ts_node_end_byte(spec);
    if (!ts_node_is_null(parent)) {
        uint32_t pcc = ts_node_child_count(parent);
        int idx = -1;
        for (uint32_t i = 0; i < pcc; ++i) {
            if (ts_node_eq(ts_node_child(parent, i), spec)) { idx = static_cast<int>(i); break; }
        }
        if (idx >= 0) {
            bool took = false;
            if (static_cast<uint32_t>(idx + 1) < pcc) {
                TSNode tr = ts_node_child(parent, static_cast<uint32_t>(idx + 1));
                if (!ts_node_is_named(tr) && tname(tr) == ",") {
                    e = ts_node_end_byte(tr);
                    took = true;
                }
            }
            if (!took && idx > 0) {
                TSNode ld = ts_node_child(parent, static_cast<uint32_t>(idx - 1));
                if (!ts_node_is_named(ld) && tname(ld) == ",") {
                    s = ts_node_start_byte(ld);
                }
            }
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
        set_err(ctx, err, WHITEOUT_ERR_PARSE,
                std::string("parse error: ") + kind,
                ts_node_start_byte(bad_node));
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

    char *buf = static_cast<char *>(std::malloc(src_len));
    if (!buf) {
        set_err(ctx, err, WHITEOUT_ERR_ALLOC, "allocation failed", 0);
        ts_tree_delete(tree);
        return WHITEOUT_ERR_ALLOC;
    }
    std::memcpy(buf, src, src_len);

    auto blanks = xf.take_blanks();
    std::sort(blanks.begin(), blanks.end(),
              [](const Range &a, const Range &b) { return a.start < b.start; });
    for (const auto &r : blanks) {
        uint32_t s = r.start;
        uint32_t e = r.end > src_len ? static_cast<uint32_t>(src_len) : r.end;
        for (uint32_t i = s; i < e; ++i) {
            if (buf[i] != '\n' && buf[i] != '\r') buf[i] = ' ';
        }
    }

    *out = buf;
    *out_len = src_len;
    ts_tree_delete(tree);
    return WHITEOUT_OK;
}
