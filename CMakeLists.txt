cmake_minimum_required(VERSION 3.21)
project(whiteout C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(WHITEOUT_BUILD_TESTS "Build whiteout tests" ${PROJECT_IS_TOP_LEVEL})
option(WHITEOUT_BUILD_EXAMPLES "Build whiteout examples" ${PROJECT_IS_TOP_LEVEL})

if(WHITEOUT_BUILD_TESTS)
    find_program(NODE_EXECUTABLE node REQUIRED)
    message(STATUS "Using node for end-to-end tests: ${NODE_EXECUTABLE}")
endif()

add_subdirectory(3rdparty)
add_subdirectory(src)

if(WHITEOUT_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(WHITEOUT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
