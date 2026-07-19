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

add_executable(dso_dsp examples/dsp_pipeline.c)
target_link_libraries(dso_dsp PRIVATE dso m)
target_compile_options(dso_dsp PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dso_parallel examples/parallel_demo.c)
target_link_libraries(dso_parallel PRIVATE dso m)
target_compile_options(dso_parallel PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dso_benchmark examples/benchmark.c)
target_link_libraries(dso_benchmark PRIVATE dso m)
target_compile_options(dso_benchmark PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dso_ecs examples/ecs_demo.c)
target_link_libraries(dso_ecs PRIVATE dso m)
target_compile_options(dso_ecs PRIVATE -Wall -Wextra -Wpedantic)

add_executable(dso_tests tests/test_runtime.c)
target_link_libraries(dso_tests PRIVATE dso)

add_executable(dso-run src/dso_run.c)
target_compile_definitions(dso-run PRIVATE _GNU_SOURCE _POSIX_C_SOURCE=200809L)
target_compile_options(dso-run PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_test(NAME dso_tests COMMAND dso_tests)

# Game (requires SDL2)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SDL2 QUIET sdl2)
    if(SDL2_FOUND)
        add_executable(dso_asteroids examples/game_asteroids.c)
        target_include_directories(dso_asteroids PRIVATE ${SDL2_INCLUDE_DIRS})
        target_link_libraries(dso_asteroids PRIVATE dso m ${SDL2_LIBRARIES})
        target_compile_options(dso_asteroids PRIVATE ${SDL2_CFLAGS_OTHER})
    endif()
endif()
