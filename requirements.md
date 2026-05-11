# whiteout — requirements

A small C/C++ library that converts TypeScript source to runnable JavaScript by
erasing type syntax, with no transformation of runtime semantics. Built on a
fork of `tree-sitter-typescript`. No Rust, Go, or Node toolchain in the
consumer build. (Node is needed only when regenerating the grammar — see
"Grammar fork" below.)

## Goal

Provide a single C-callable function that takes TypeScript source and returns
JavaScript source. Output preserves line and column positions of all
runtime-bearing code so stack traces, debuggers, and source-position-sensitive
tooling work without source maps.

Primary consumer: a C/C++ host that embeds QuickJS (or any other JS engine) and
wants to load `.ts` files directly.

## Current state of the repo

- `requirements.md` (this file).
- CMake build wired up: top-level + `3rdparty/` + `src/` + `examples/`.
- `3rdparty/` submodules:
  - `tree-sitter` → upstream master.
  - `tree-sitter-typescript` → `jhanssen/tree-sitter-typescript@master`, a fork
    of SegaraRai's fork of upstream. Grammar fixes shipped: `using`/`await
    using` declarations with type annotations (via JS-0.25 base), `export type
    *` and `export type * as Name`, variance annotations, plus our own three
    fixes for `accessor` modifier combinations, `import(...).T<U>` in type
    position, and `keyof import(...)`.
- `src/` — placeholder. **No whiteout library code has been written yet.** The
  C ABI in this document is a sketch; nothing implements it.
- `examples/`:
  - `ts_syntax_check` — parses a curated TS 1.0–6.0 corpus of 19 snippets and
    reports ERROR/MISSING nodes. All 19 pass against the current grammar.
  - `ts_shape_check` — checks named-node shapes for 36 features. 30 pass; the
    6 "fails" are an artifact of `ts_node_string` hiding anonymous keyword
    tokens — verified separately, not real bugs.
  - `ts_full_dump` — prints every child of every node, including anonymous
    tokens, with byte ranges. Diagnostic tool for grammar investigation.
- Tooling installed via Homebrew when working on the grammar: `tree-sitter`
  and `tree-sitter-cli` 0.26.8.

## Grammar fork: jhanssen/tree-sitter-typescript

The grammar is a fork because the upstream `tree-sitter/tree-sitter-typescript`
is effectively dormant — the most recent meaningful commit was January 2025,
and at the time of writing there are 18 open PRs (some from January 2025) sat
unmerged including fixes for several issues whiteout needs. We base on
SegaraRai's fork (which already merged the open work into one tree) and then
add our own fixes on top.

### Confirmed-fixed in `jhanssen/tree-sitter-typescript@master`

- Typed `using r: Disposable = expr` and `await using` (inherited from
  SegaraRai via tree-sitter-javascript 0.25 base).
- `export type * from "..."` and `export type * as Name from "..."`.
- Variance annotations `<in T>`, `<out T>`, `<in out T>`.
- `class C { static accessor x = 1 }`, `override accessor`, `abstract accessor`.
- `const x: import("foo").Bar<Baz>` as a type.
- `type X = keyof import("a").A`.

### Grammar regeneration workflow

The grammar requires regeneration after any change to `common/define-grammar.js`.

Working directory: `/Users/jhanssen/dev/tree-sitter-typescript` (a sibling
clone of the fork). This is **separate** from the submodule at
`whiteout/3rdparty/tree-sitter-typescript`, which is just a pinned checkout.

Sibling dependency: the fork's `package.json` declares
`"tree-sitter-javascript": "file:../tree-sitter-javascript"`. A working clone
of `SegaraRai/tree-sitter-javascript` must exist at
`/Users/jhanssen/dev/tree-sitter-javascript` for `npm install` and
`tree-sitter generate` to succeed. Upstream `tree-sitter/tree-sitter-javascript`
does **not** work — it lacks the `jsx_member_expression` rule that
SegaraRai's grammar references.

Steps to add a grammar fix:

```sh
cd /Users/jhanssen/dev/tree-sitter-typescript
# Edit common/define-grammar.js.

cd typescript && tree-sitter generate && cd ..
cd tsx        && tree-sitter generate && cd ..

# Optional: run the upstream corpus
tree-sitter test     # currently 121/121

git add common/define-grammar.js typescript/src tsx/src
git commit -m "fix: <what>"
git push origin master

# Then in whiteout:
cd /Users/jhanssen/dev/whiteout/3rdparty/tree-sitter-typescript
git fetch origin
git merge --ff-only origin/master
cd /Users/jhanssen/dev/whiteout
cmake --build build
./build/examples/ts_syntax_check    # verify no regressions
git add 3rdparty/tree-sitter-typescript
git commit -m "submodule: bump tree-sitter-typescript"
```

Bootstrap (one-time) requires `npm install --ignore-scripts` inside the fork
to populate `node_modules/tree-sitter-javascript`. The `--ignore-scripts` flag
is needed because the native `node-gyp` build for the JS binding fails on
Node 24; we don't need the native binding, only the JS module for
`require('tree-sitter-javascript/grammar')`.

The toolchain dependency (Node, npm, tree-sitter CLI, the sibling JS grammar
clone) is **only** needed for grammar development. The whiteout consumer
build sees only the pre-generated `parser.c` and uses plain CMake + C/C++.

## Known gaps (must be resolved or accepted before v1)

These are unresolved correctness or commitment risks. They are surfaced here
rather than buried in later sections.

1. **No library code exists yet.** The C ABI in this document is a sketch.
   Building the library is the next major work item.
2. **Performance and binary-size numbers in this document are budgets, not
   commitments.** The 5 MB/s throughput target, the under-4×-input memory
   target, and the under-2 MB binary-size target have not been measured.
   They must be verified against a real corpus before any external promise
   is made.
3. **Grammar coverage is verified for a curated 36-feature corpus, not for
   real-world TS code.** No DefinitelyTyped or application-codebase run has
   been performed. Expect to discover additional grammar bugs when that runs.
4. **Known partial-support cases in the current grammar:**
   - `import("a").Outer<X>.Inner<Y>` — chained generics on import paths still
     errors. Simpler `import("a").Foo<Bar>` works.
   - Flow leftovers in the grammar (`?T`, `*`, `import typeof`) accept
     non-TS syntax. Fidelity issue, not whiteout breakage — the bytes are
     still in type position and get blanked.
   - `tree-sitter-typescript` open issues #335, #320, #314, #317, #309 and
     others not investigated. May or may not affect whiteout.

## Non-goals

- No type checking. Ever. Type errors are reported by `tsc --noEmit` out of band.
- No transformation of TS constructs with runtime semantics (`enum`, `namespace`,
  parameter properties, `import =`, `export =`). These are rejected, not lowered.
- No JSX/TSX transformation. TSX may be parsed but is out of scope for v1.
- No source maps. Position fidelity is achieved by whitespace replacement, not
  map generation.
- No bundling, module resolution, or import rewriting.
- No CLI. Library only. A trivial CLI may exist for testing but is not shipped
  as a product.

## Functional requirements

### Input
- UTF-8 TypeScript source as a `(const char*, size_t)` pair.
- File extension is not consulted; the caller decides what to pass in.

### Output
- UTF-8 JavaScript source as a heap-allocated buffer the caller frees via a
  library-provided free function.
- Output byte length is **equal to input byte length** in all successful cases.
  Type syntax is replaced with ASCII spaces; newlines inside type regions are
  preserved. This guarantees line and column numbers in the output match the
  input.
- On error, output pointer is null and a structured error is returned (see
  Error model).

### Stripped constructs (replaced with whitespace)
The following are removed by overwriting their byte range with spaces (newlines
preserved):
- Type annotations on variables, parameters, return types, properties.
- Optional markers (`?`) on parameters and class fields. The `?` is TS-only
  syntax with no JS equivalent and must be blanked alongside the annotation.
- Old-style type assertions `<T>x`. The `<T>` prefix is blanked; `x` is kept.
- Type-only declarations: `interface`, `type` aliases.
- Generic type parameters and type arguments: `<T>`, `<T extends U>`, `foo<T>()`,
  `new Foo<T>()`.
- Type assertions: `x as Foo`, `x satisfies Foo`. The inner expression is kept.
- Non-null assertions: `x!`. The `!` is blanked; `x` is kept.
- `import type` / `export type` declarations and type-only specifiers within
  regular import/export clauses. When a `type` specifier appears inside a
  mixed clause (`import { A, type B, C } from "x"`), one adjacent comma is
  also blanked so the resulting named-import list is well-formed JS.
- Class member modifiers in type positions only: `readonly`, `override`,
  `abstract`, `public`/`private`/`protected` (when not part of a parameter
  property — see Rejected).
- `declare`-prefixed and `abstract`-prefixed class field declarations are
  blanked *in their entirety* (the whole field, not just the modifier).
  Blanking only the modifier would produce `class C { x; }` which under
  `useDefineForClassFields` defines an own property the TS author specifically
  said not to create — `'x' in new C()` flips from `false` (under `tsc`) to
  `true`. The same problem applies to `abstract` on a field: `tsc` emits no
  field at all. Blanking the whole field preserves the original semantics for
  both.
- Type-only `declare` statements at module scope (`declare const`,
  `declare function`, `declare module`, `declare global`, etc.).
- `const` type parameter modifier (`<const T>`).
- Variance modifiers `in`/`out` on type parameters.

### Rejected constructs (cause failure)
The following are not stripped and not lowered. The library returns a
structured error with byte offset:
- `enum` declarations (including `const enum`).
- `namespace` and `module` declarations with runtime emit.
- Parameter properties: any constructor parameter carrying one or more of
  `public`, `private`, `protected`, `readonly`, or `override`. Trigger rule
  is the strict/simpler form: presence of any modifier in that set causes
  rejection, even though `override`-alone is not actually a parameter
  property under TS semantics. Revisit `override`-alone when (and if) the
  post-v1 lowering API lands; it would then be blanked as a class-member
  modifier in type position rather than rejected.
- `import =` and `export =`.
- Decorators. v1 rejects **all** decorators (`@foo`-prefixed forms on
  classes, methods, accessors, properties, and parameters), regardless of
  whether they would parse as legacy TS or stage-3 ECMAScript decorators.
  Both have runtime semantics; blanking is incorrect for either. Pass-through
  of stage-3 decorators may be considered post-v1 once a syntactic or
  configuration-driven way to distinguish them from legacy is settled.

The rejected list matches Node's `--experimental-strip-types` behavior so
behavior is portable between Node and whiteout.

### Error model
A successful call returns the output buffer. A failure returns:
- An error code (enum: parse error, unsupported construct, allocation failure,
  invalid UTF-8, internal error).
- A human-readable message owned by the library (caller does not free).
- The byte offset of the first offending construct, where applicable.

The library does not abort, log, or write to stderr. All errors are returned to
the caller.

### Determinism
For identical input, output and error are byte-identical across runs and across
supported platforms.

## Non-functional requirements

### Dependencies
- `tree-sitter` runtime (C, vendored via submodule at `3rdparty/tree-sitter`).
- `tree-sitter-typescript` grammar (generated C, vendored via submodule at
  `3rdparty/tree-sitter-typescript` pointing at `jhanssen/tree-sitter-typescript`).
- C11 for the parser sources; **C++23** for the library's own code.
- No Rust, Go, Node, or Python in the **consumer** build.
- No network access during build.
- Node + tree-sitter CLI are required **only** to regenerate the grammar
  after editing `define-grammar.js`; the generated `parser.c` is checked
  into the submodule.

### Performance
- Throughput target: at least 5 MB/s of TS source on a single core on Apple
  Silicon, measured against a representative corpus (DefinitelyTyped headers,
  application TS).
- Memory: peak working set under 4× input size for files up to 1 MB.
- These targets are budgets to verify, not promises. Benchmarks are part of
  the test suite.

### Binary size
- Static library plus vendored tree-sitter + tree-sitter-typescript should
  link into a host binary at under 2 MB additional size, release build,
  stripped. To verify, not promised.

### Platforms
- v1: macOS arm64, Linux x86_64, Linux arm64.
- Windows x86_64 supported but not in the initial CI matrix.

### Thread safety
- The library exposes a context object. Multiple contexts may be used
  concurrently from different threads. A single context is not thread-safe.
- The parser is created and owned by the context; no global state.

## Public API (sketch, subject to revision)

```c
// whiteout.h
#ifdef __cplusplus
extern "C" {
#endif

typedef struct whiteout_ctx whiteout_ctx;

typedef enum {
    WHITEOUT_OK = 0,
    WHITEOUT_ERR_PARSE,
    WHITEOUT_ERR_UNSUPPORTED,
    WHITEOUT_ERR_ALLOC,
    WHITEOUT_ERR_UTF8,
    WHITEOUT_ERR_INTERNAL,
} whiteout_status;

typedef struct {
    whiteout_status status;
    const char *message;   // borrowed; valid until next call on this ctx
    size_t offset;         // byte offset into input, or 0 if not applicable
} whiteout_error;

whiteout_ctx *whiteout_ctx_new(void);
void whiteout_ctx_free(whiteout_ctx *);

// On success: *out and *out_len are set; caller must call whiteout_free(*out).
// On failure: *out is NULL, *out_len is 0, error fields are populated.
whiteout_status whiteout_transform(
    whiteout_ctx *ctx,
    const char *src, size_t src_len,
    char **out, size_t *out_len,
    whiteout_error *err);

void whiteout_free(char *buf);

#ifdef __cplusplus
}
#endif
```

A C++ RAII wrapper (`whiteout::Context`, `whiteout::Result`) may be provided as
a header-only convenience but the C ABI is the supported interface.

## Build / packaging

- CMake project. `find_package(whiteout)` works after install.
- Static library by default. Shared library opt-in.
- pkg-config file installed.
- Vendored `tree-sitter` and `tree-sitter-typescript` sources live under
  `3rdparty/` as git submodules. Updating them is a manual, reviewed step.

## Testing

Built so far (under `examples/`, all use the vendored grammar directly without
needing the library to exist):

- `ts_syntax_check` — curated TS 1.0–6.0 corpus, 19 snippets. Reports any
  ERROR/MISSING nodes. Currently 19/19 clean.
- `ts_shape_check` — 36 snippets; verifies expected named-node-type substrings
  in s-expression dumps. Currently 30/36; the 6 "fails" are `ts_node_string`
  hiding anonymous keyword tokens, verified separately (`readonly`, `accessor`,
  `using` *are* in the tree, they just don't appear in the named-only s-expr).
- `ts_full_dump` — diagnostic tool, prints every child of every node with byte
  ranges. Used to investigate ambiguous cases.

Planned (not yet built):

- Unit tests against a curated corpus of TS snippets, one per supported and
  rejected construct.
- Snapshot tests: input → expected output, byte-equal.
- Line/column preservation tests: parse output with a JS parser, verify that
  every retained identifier in the output is at the same `(line, column)` as
  in the input.
- Differential tests: where feasible, compare output against
  `node --experimental-strip-types` on the same input and assert byte-equality
  (modulo Node-specific differences documented in a fixture-skip list).
- Real-corpus run: parse DefinitelyTyped and a real application codebase
  through the grammar, collect ERROR/MISSING locations, dedupe. This will
  surface grammar gaps that 36 hand-picked snippets cannot.
- Benchmark suite kept under `bench/`, run on demand.

## Decisions locked for v1

- **Decorators**: reject all. See Rejected constructs.
- **Error recovery**: hard failure. Any ERROR or MISSING node in the parse
  tree causes `whiteout_transform` to return `WHITEOUT_ERR_PARSE` with the
  byte offset of the first such node. No partial output is produced.
- **JSX/TSX**: not in v1. The library exposes a TS-only entry point backed
  by `tree_sitter_typescript`. `.tsx` input is not supported; a TSX entry
  point may be added post-v1.
- **Parameter properties**: rejected by the strict modifier-set rule. See
  Rejected constructs.

## Open questions

These are deferred and do not block v1 implementation, but should be settled
before or shortly after v1 release:

1. **Grammar maintenance posture**: long-term, do we (a) keep cherry-picking
   from SegaraRai and upstream PRs, (b) become the de-facto active fork and
   accept contributions, or (c) plan a migration to whatever Microsoft's TS
   7.0 native compiler ships (no concrete plan yet for an embeddable parser).
   The choice shapes effort beyond v1.

## Out of scope / explicitly not committed

- Watch mode, file I/O, glob handling.
- IDE integration, language server.
- Sourcemap generation.
- Plugin architecture, custom transforms.
- Any feature that requires a type system.

## Possible post-v1 directions (not committed)

- **Opt-in lowering of runtime-bearing TS constructs** (parameter properties,
  `enum`, `namespace`, and similar) via a separate entry point that emits a
  source map. This would lower whatever the CST allows us to lower
  mechanically, and reject the rest. It explicitly does **not** preserve the
  v1 byte-equal-length / position-without-sourcemap guarantee — callers opt
  into source maps in exchange for accepting more input. The current API and
  its invariants stay unchanged; this would be an additional API, not a
  replacement. No commitment to ship.
