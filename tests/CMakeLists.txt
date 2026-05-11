add_executable(whiteout_tests whiteout_tests.c)
target_link_libraries(whiteout_tests PRIVATE whiteout tree_sitter_typescript)
target_compile_definitions(whiteout_tests PRIVATE
    WHITEOUT_E2E_DIR="${CMAKE_CURRENT_SOURCE_DIR}/e2e"
    WHITEOUT_E2E_BUILD_DIR="${CMAKE_CURRENT_BINARY_DIR}/e2e"
    WHITEOUT_NODE_EXECUTABLE="${NODE_EXECUTABLE}"
)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/e2e)

add_test(NAME whiteout_tests COMMAND whiteout_tests)
