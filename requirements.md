# whiteout requirements

A small C/C++ library that converts TypeScript source to runnable JavaScript by
erasing type syntax, with no transformation of runtime semantics. Built on a
fork of `tree-sitter-typescript`. No Rust, Go, or Node toolchain in the
consumer build. Node is only needed when regenerating the grammar (see
"Grammar fork" below).

## Goal

Provide a single C-callable function that takes TypeScript source and returns
JavaScript source. Output preserves line and column positions of all
runtime-bearing code so stack traces, debuggers, and source-position-sensitive
tooling work without source maps.

Primary consumer: a C/C++ host that embeds QuickJS (or any other JS engine) and
wants to load `.ts` files directly.

## Current state of the repo

- `requirements.md` (this file).
- CMake build wired up: top-level + `3rdparty/` + `src/` + `examples/` + `tests/`.
- `3rdparty/` submodules:
  - `tree-sitter` points at upstream master.
  - `tree-sitter-typescript` points at `jhanssen/tree-sitter-typescript@master`,
    a fork of SegaraRai's fork of upstream. Grammar fixes shipped:
    `using`/`await using` declarations with type annotations (via JS-0.25
    base), `export type *` and `export type * as Name`, variance annotations,
    plus our own three fixes for `accessor` modifier combinations,
    `import(...).T<U>` in type position, and `keyof import(...)`.
- `include/whiteout/whiteout.h`: public C ABI.
- `src/whiteout.cpp`: library implementation. The behavior described in the
  rest of this document is what this code does.
- `src/CMakeLists.txt`: builds the `whiteout` static library, linking
  `tree_sitter_typescript` and the `tree-sitter` runtime.
- `tests/`:
  - `whiteout_tests.c`: in-process unit tests (exact-match strip, property-
    based length/newline/reparse, positional substring, reject, parse-error,
    UTF-8, ABI sanity) plus end-to-end fixtures under `tests/e2e/` that pipe
    whiteout output through real `node` and compare stdout.
  - `ts_blank_space_corpus.c`: runs ts-blank-space's 8 fixture cases plus
    3 carved `-works` subsets (`a-works`, `b-works`, `asi-works`) through
    whiteout with per-fixture classification (byte-equal, reject, parse-
    error with diagnostic hint, documented divergence).
- `examples/`:
  - `ts_syntax_check`: parses a curated TS 1.0 to 6.0 corpus of 19 snippets
    and reports ERROR/MISSING nodes.
  - `ts_shape_check`: checks named-node shapes for 36 features.
  - `ts_full_dump`: prints every child of every node, including anonymous
    tokens, with byte ranges. Diagnostic tool for grammar investigation.
- Tooling installed via Homebrew when working on the grammar: `tree-sitter`
  and `tree-sitter-cli` 0.26.8.

## Grammar fork: jhanssen/tree-sitter-typescript

The grammar is a fork because the upstream `tree-sitter/tree-sitter-typescript`
is effectively dormant. The most recent meaningful commit was January 2025,
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
does **not** work; it lacks the `jsx_member_expression` rule that
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

## Known gaps

These are unresolved correctness or commitment risks. They are surfaced here
rather than buried in later sections.

1. **Performance and binary-size numbers in this document are budgets, not
   commitments.** The 5 MB/s throughput target, the under-4x-input memory
   target, and the under-2 MB binary-size target have not been measured.
   They must be verified against a real corpus before any external promise
   is made.
2. **Grammar coverage is verified for a curated 36-feature corpus and the
   ts-blank-space corpus (8 original fixtures plus 3 carved `-works`
   subsets), not for real-world TS code at scale.** No DefinitelyTyped or
   application-codebase run has been performed.
3. **Known partial-support cases in the current grammar:**
   - `import("a").Outer<X>.Inner<Y>`: chained generics on import paths still
     errors. Simpler `import("a").Foo<Bar>` works.
   - Flow leftovers in the grammar (`?T`, `*`, `import typeof`) accept
     non-TS syntax. Fidelity issue, not whiteout breakage; the bytes are
     still in type position and get blanked.
   - `(expr)<T>` instantiation expressions followed by a tagged template:
     tree-sitter parses `<T>` as binary `<` / `>` operators rather than
     `type_arguments`, so `<T>` is not blanked. Documented divergence vs
     ts-blank-space; covered by the `b` fixture (full file is
     `EXPECT_DIVERGES`, `b-works` excises line 84 to exercise the rest
     byte-equal).
   - Class-body constructs that combine `as` or `satisfies` with an
     immediately-following `(`/`[`/`` ` `` on a new line whose right-hand
     side does not fit as a `lookup_type` (e.g. a parenthesised parameter
     list or call) produce a parse error (e.g.
     `class C { f = 1 as N\n["x"]() {} }`). The error message includes a
     hint suggesting an explicit `;` to disambiguate. The `asi` fixture
     full file is `EXPECT_PARSE_ERR`; `asi-works` excises the class-ASI
     method body to exercise the rest byte-equal.
   - Optional parameter declarations with a comment between `?` and
     `:type`, e.g. `(a? /*c*/: T)`, produce a parse error in tree-sitter.
     The `a` fixture full file is `EXPECT_PARSE_ERR`; `a-works` excises
     the offending line to exercise the rest byte-equal.
   - `tree-sitter-typescript` open issues #335, #320, #314, #317, #309 and
     others not investigated. May or may not affect whiteout.

## Non-goals

- No type checking. Ever. Type errors are reported by `tsc --noEmit` out of band.
- No transformation of TS constructs with runtime semantics (`enum`, `namespace`,
  parameter properties, `import =`, `export =`). These are rejected, not lowered.
- No JSX/TSX transformation. TSX is out of scope.
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
- Optional markers (`?`) on parameters, class fields, and methods. The `?` is
  TS-only syntax with no JS equivalent and must be blanked alongside the
  annotation.
- Definite-assignment assertion `!` on class fields (`x!: T`) and on `let`/
  `var`/`const` declarators (`let h!: T`). The `!` token is blanked.
- `this` parameter declarations (`function f(this: T, x)`). The entire `this`
  parameter and any trailing comma are blanked so the remaining parameter
  list is well-formed JS.
- Type-only declarations: `interface`, `type` aliases.
- Generic type parameters and type arguments: `<T>`, `<T extends U>`,
  `foo<T>()`, `new Foo<T>()`.
- Type assertions: `x as Foo`, `x satisfies Foo`. The inner expression is
  kept.
- Non-null assertions: `x!`. The `!` is blanked; `x` is kept.
- `import type` / `export type` declarations and type-only specifiers within
  regular import/export clauses. When a `type` specifier appears inside a
  mixed clause (`import { A, type B, C } from "x"`), one adjacent comma is
  also blanked so the resulting named-import list is well-formed JS. Comments
  between the specifier and the comma are tolerated.
- `export <type-only-form>` where the body is a `type` alias, `interface`,
  or ambient declaration: the entire statement including the `export`
  keyword is blanked, since no runtime emit is produced.
- Class member modifiers in type positions only: `readonly`, `override`,
  `abstract`, `public`/`private`/`protected` (when not part of a parameter
  property; see Rejected).
- `declare`-prefixed and `abstract`-prefixed class field declarations are
  blanked *in their entirety* (the whole field, not just the modifier).
  Blanking only the modifier would produce `class C { x; }` which under
  `useDefineForClassFields` defines an own property the TS author specifically
  said not to create: `'x' in new C()` flips from `false` (under `tsc`) to
  `true`. The same problem applies to `abstract` on a field: `tsc` emits no
  field at all. Blanking the whole field preserves the original semantics for
  both.
- Type-only `declare` statements at module scope (`declare const`,
  `declare function`, `declare module`, `declare global`, etc.).
- `const` type parameter modifier (`<const T>`).
- Variance modifiers `in`/`out` on type parameters.

### ASI safety inside blanked ranges
Whitespace replacement can change parse semantics where the blanked region
otherwise separated two tokens that JavaScript would chain via automatic
semicolon insertion. In these specific cases whiteout writes a `;` at the
first non-newline byte of the blanked range to preserve TypeScript's
interpretation. The byte-equal-length and position-preservation guarantees
are unaffected.

- After `as` / `satisfies` blanks when the next significant character across
  a newline is `(`, `[`, or `` ` ``. TypeScript treats this as a statement
  boundary; without the `;` the emitted JS would form a call/index/tagged
  template that the TS author did not write.
- Inside an `as` / `satisfies` expression whose type is a `lookup_type` or
  `array_type` and the type's `[` token sits on a different line than the
  base type's start (e.g. `foo as T\n[0]`). Tree-sitter parses the
  bracketed continuation as indexed-type access, swallowing it into the
  type region; whiteout cuts the blank at the end of the base type,
  inserts `;`, and leaves the runtime `[N]` intact.
- After statement-level type blanks (`interface_declaration`,
  `type_alias_declaration`, `ambient_declaration`, and `export`-wrapped
  forms of these) when the previous significant character is not already
  `;`. Comment runs are skipped on both sides of the lookup. Matches
  ts-blank-space's `blankStatement` rule.
- After class-body strip-full members (`abstract`/`declare` fields,
  abstract methods, index signatures, etc.) when the previous significant
  character is not already `;`. The trailing `;` sibling that tree-sitter
  keeps outside the member node is absorbed into the blank.
- After class-member modifier blanks (`public ["x"] = 1`, etc.) when the
  member's name is a `[computed]` form, so the previous field's value does
  not chain into `[`.

### Rejected constructs (cause failure)
The following are not stripped and not lowered. The library returns a
structured error with byte offset:
- `enum` declarations (including `const enum`).
- `namespace` and `module` declarations with runtime emit.
- Parameter properties: any constructor parameter carrying one or more of
  `public`, `private`, `protected`, `readonly`, or `override`. Trigger rule
  is the strict/simpler form: presence of any modifier in that set causes
  rejection, even though `override`-alone is not actually a parameter
  property under TS semantics. Revisit `override`-alone if the optional
  lowering API described later in this document ships; it would then be
  blanked as a class-member modifier in type position rather than rejected.
- `import =` and `export =`.
- Decorators. Whiteout rejects **all** decorators (`@foo`-prefixed forms on
  classes, methods, accessors, properties, and parameters), regardless of
  whether they would parse as legacy TS or stage-3 ECMAScript decorators.
  Both have runtime semantics; blanking is incorrect for either. Pass-through
  of stage-3 decorators may be considered later once a syntactic or
  configuration-driven way to distinguish them from legacy is settled.
- Prefix-style type assertions `<T>x`. Whitespace replacement is not safe
  for these in several positions (`return <T>\nfoo`, `() => <T>{}`), so all
  such assertions are rejected. Users should migrate to `x as T`, which is
  byte-equal-safe.
- Arrow functions whose `type_parameters` or `return_type` span multiple
  lines, e.g. `(a: T): Array<\nU\n> => [...]`. Blanking would place a line
  terminator between `)` and `=>`, which is a JS syntax error.

The rejected list otherwise matches Node's `--experimental-strip-types`
behavior so behavior is portable between Node and whiteout.

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
- Supported: macOS arm64, Linux x86_64, Linux arm64.
- Windows x86_64 builds but isn't in the CI matrix.

### Thread safety
- The library exposes a context object. Multiple contexts may be used
  concurrently from different threads. A single context is not thread-safe.
- The parser is created and owned by the context; no global state.

## Public API

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

Built (under `tests/`):

- `whiteout_tests`: in-process unit tests covering exact-match strip cases,
  property-based checks (output length equal to input, newline positions
  preserved, output parses cleanly through tree-sitter), positional checks
  (retained identifiers at expected offsets), every rejected construct,
  parse errors, invalid UTF-8, and ABI sanity (ctx reuse, free of NULL,
  error-then-success cycle).
- `whiteout_tests` end-to-end fixtures (`tests/e2e/*.ts` + `.expected`): the
  test program writes whiteout's output to a `.mjs` file and runs `node` on
  it, comparing stdout against the fixture's expected output. Covers types,
  generics, `as`/`satisfies`, interface plus class, optional params,
  definite assignment, template literals, nested generics, type aliases.
- `ts_blank_space_corpus`: runs ts-blank-space's 8 original fixture cases
  through whiteout, plus 3 carved `-works` subsets (`a-works`, `b-works`,
  `asi-works`) that excise the lines the originals trip on so the rest
  can be exercised byte-equal. Each fixture has a per-fixture
  classification (byte-equal vs reject vs parse-error with hint vs
  documented divergence). The original fixtures and their expected
  outputs are copied verbatim from ts-blank-space under
  `tests/ts-blank-space-corpus/` (Apache-2.0); the `-works` files are
  derived from them.

Built (under `examples/`, all use the vendored grammar directly without
needing the library to exist):

- `ts_syntax_check`: curated TS 1.0 to 6.0 corpus, 19 snippets. Reports any
  ERROR/MISSING nodes. Currently 19/19 clean.
- `ts_shape_check`: 36 snippets; verifies expected named-node-type
  substrings in s-expression dumps.
- `ts_full_dump`: diagnostic tool, prints every child of every node with
  byte ranges. Used to investigate ambiguous cases.

Not yet built:

- Real-corpus run: parse DefinitelyTyped and a real application codebase
  through the grammar, collect ERROR/MISSING locations, dedupe. This will
  surface grammar gaps that the hand-picked snippets cannot.
- Benchmark suite kept under `bench/`, run on demand.

## Decisions locked

- **Decorators**: reject all. See Rejected constructs.
- **Error recovery**: hard failure. Any ERROR or MISSING node in the parse
  tree causes `whiteout_transform` to return `WHITEOUT_ERR_PARSE` with the
  byte offset of the first such node, plus a line:column-prefixed message
  that includes a disambiguation hint when an `as`/`satisfies` keyword
  appears nearby. No partial output is produced.
- **JSX/TSX**: not supported. The library exposes a TS-only entry point
  backed by `tree_sitter_typescript`. `.tsx` input is not handled.
- **Parameter properties**: rejected by the strict modifier-set rule. See
  Rejected constructs.
- **Prefix `<T>x` type assertions**: rejected. The `x as T` form is the only
  supported assertion syntax.
- **Multi-line arrow function types**: arrow functions whose
  `type_parameters` or `return_type` spans multiple lines are rejected.
- **ASI insertion strategy**: where blanking would form a parse-changing JS
  ASI hazard, whiteout writes a single `;` into the blanked range to
  preserve TypeScript's interpretation. See "ASI safety inside blanked
  ranges" above.

## Open questions

These are deferred and do not block correctness of the current behavior,
but should be settled eventually:

1. **Grammar maintenance posture**: long-term, do we (a) keep cherry-picking
   from SegaraRai and upstream PRs, (b) become the de-facto active fork and
   accept contributions, or (c) plan a migration to whatever Microsoft's TS
   7.0 native compiler ships (no concrete plan yet for an embeddable parser).

## Out of scope / explicitly not committed

- Watch mode, file I/O, glob handling.
- IDE integration, language server.
- Sourcemap generation.
- Plugin architecture, custom transforms.
- Any feature that requires a type system.

## Possible future directions (not committed)

- **Opt-in lowering of runtime-bearing TS constructs** (parameter properties,
  `enum`, `namespace`, and similar) via a separate entry point that emits a
  source map. This would lower whatever the CST allows us to lower
  mechanically, and reject the rest. It explicitly does **not** preserve the
  byte-equal-length / position-without-sourcemap guarantee that the main
  transform provides; callers opt into source maps in exchange for accepting
  more input. The current API and its invariants stay unchanged; this would
  be an additional API, not a replacement. No commitment to ship.
