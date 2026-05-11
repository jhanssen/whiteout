# whiteout

A C/C++ library that converts TypeScript source to runnable JavaScript by
erasing type syntax. Type-bearing bytes are replaced with whitespace so the
output is byte-equal-length to the input and every retained character keeps
its original `(line, column)`.

Built on a fork of `tree-sitter-typescript`. See [requirements.md](requirements.md)
for the full specification.

```
let x: number = 1;     →     let x         = 1;
function f<T>(x: T)    →     function f   (x   )
type Foo = string;     →     ;
```

whiteout does not type-check. Pair it with `tsc --noEmit` if you want errors.
JSX/TSX is not handled; `.tsx` input is unsupported.

## API

```c
#include <whiteout/whiteout.h>

whiteout_ctx *ctx = whiteout_ctx_new();
char *out = NULL;
size_t out_len = 0;
whiteout_error err = {0};
whiteout_status st = whiteout_transform(ctx, src, src_len, &out, &out_len, &err);
if (st == WHITEOUT_OK) {
    /* `out` is heap-allocated, `out_len` bytes, equal to `src_len`. */
    whiteout_free(out);
} else {
    /* err.message is a human-readable string owned by ctx. */
    /* err.offset is the byte offset of the offending construct. */
}
whiteout_ctx_free(ctx);
```

A single context is not thread-safe. Multiple contexts may be used
concurrently from different threads.

## Build

```sh
git clone --recurse-submodules <repo>
cd whiteout
cmake -S . -B build
cmake --build build
```

CMake options:

- `WHITEOUT_BUILD_TESTS`: default `ON` for top-level builds, `OFF` when
  consumed via `add_subdirectory`/`FetchContent`. Test builds require `node`
  on `PATH` for the end-to-end fixtures.
- `WHITEOUT_BUILD_EXAMPLES`: same default behavior.

Embedded use:

```cmake
add_subdirectory(whiteout)
target_link_libraries(your_target PRIVATE whiteout)
```

## Tests

Run under `ctest --test-dir build`:

- `whiteout_tests`: 77 unit tests plus 10 end-to-end fixtures that pipe
  whiteout output through real `node` and compare stdout.
- `ts_blank_space_corpus`: runs ts-blank-space's 8 fixture cases through
  whiteout with per-fixture classification.

## License

MIT. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
