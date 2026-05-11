add_library(tree_sitter STATIC
    tree-sitter/lib/src/lib.c
)
target_include_directories(tree_sitter
    PUBLIC tree-sitter/lib/include
    PRIVATE tree-sitter/lib/src
)
target_compile_options(tree_sitter PRIVATE -Wno-unused-parameter)

add_library(tree_sitter_typescript STATIC
    tree-sitter-typescript/typescript/src/parser.c
    tree-sitter-typescript/typescript/src/scanner.c
)
target_include_directories(tree_sitter_typescript
    PUBLIC tree-sitter-typescript/typescript/src
)
target_link_libraries(tree_sitter_typescript PUBLIC tree_sitter)
target_compile_options(tree_sitter_typescript PRIVATE -Wno-unused-parameter)
