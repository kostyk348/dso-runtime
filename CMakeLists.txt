cmake_minimum_required(VERSION 3.16)
project(dso_runtime C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

add_library(dso STATIC
    src/dso.c
)

target_include_directories(dso PUBLIC include)
target_compile_definitions(dso PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(dso PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dso_pipeline examples/pipeline.c)
target_link_libraries(dso_pipeline PRIVATE dso)

add_executable(dso_frame_loop examples/frame_loop.c)
target_link_libraries(dso_frame_loop PRIVATE dso)

add_executable(dso_tests tests/test_runtime.c)
target_link_libraries(dso_tests PRIVATE dso)

add_executable(dso-run src/dso_run.c)
target_compile_definitions(dso-run PRIVATE _GNU_SOURCE _POSIX_C_SOURCE=200809L)
target_compile_options(dso-run PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME dso_tests COMMAND dso_tests)
