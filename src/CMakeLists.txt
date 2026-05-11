add_library(whiteout STATIC
    whiteout.cpp
)
target_include_directories(whiteout
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)
target_link_libraries(whiteout PUBLIC tree_sitter_typescript)
target_compile_options(whiteout PRIVATE -Wall -Wextra -Wpedantic)
