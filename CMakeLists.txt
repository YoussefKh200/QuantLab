cmake_minimum_required(VERSION 3.20)
project(QuantLab VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler flags ─────────────────────────────────────────────────────────
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(
        -O3 -march=native
        -Wall -Wextra -Wpedantic
        -Wno-unused-parameter
        -ffast-math -funroll-loops
        -DNDEBUG
    )
elseif(MSVC)
    add_compile_options(/O2 /W4 /arch:AVX2 /std:c++20)
endif()

# ── Main executable ────────────────────────────────────────────────────────
add_executable(quantlab main.cpp)
target_include_directories(quantlab PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# ── Threads ───────────────────────────────────────────────────────────────
find_package(Threads REQUIRED)
target_link_libraries(quantlab Threads::Threads)

# ── Optional: Parquet via Arrow ────────────────────────────────────────────
option(USE_ARROW "Enable Apache Arrow/Parquet support" OFF)
if(USE_ARROW)
    find_package(Arrow REQUIRED)
    find_package(Parquet REQUIRED)
    target_link_libraries(quantlab Arrow::arrow_shared Parquet::parquet_shared)
    target_compile_definitions(quantlab PRIVATE QL_USE_ARROW)
endif()

# ── Optional: SQLite ───────────────────────────────────────────────────────
option(USE_SQLITE "Enable SQLite persistence" OFF)
if(USE_SQLITE)
    find_package(SQLite3 REQUIRED)
    target_link_libraries(quantlab SQLite::SQLite3)
    target_compile_definitions(quantlab PRIVATE QL_USE_SQLITE)
endif()

# ── Optional: pybind11 bridge ──────────────────────────────────────────────
option(BUILD_PYTHON "Build Python bindings" OFF)
if(BUILD_PYTHON)
    find_package(pybind11 REQUIRED)
    pybind11_add_module(quantlab_py src/python_bridge/PyBridge.cpp)
    target_include_directories(quantlab_py PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(quantlab_py PRIVATE Threads::Threads)
endif()

# ── Tests ─────────────────────────────────────────────────────────────────
option(BUILD_TESTS "Build unit tests" OFF)
if(BUILD_TESTS)
    enable_testing()
    find_package(GTest QUIET)
    if(GTest_FOUND)
        add_subdirectory(tests)
    else()
        message(STATUS "GTest not found; skipping tests")
    endif()
endif()

# ── Install ────────────────────────────────────────────────────────────────
install(TARGETS quantlab DESTINATION bin)
install(DIRECTORY src/ DESTINATION include/quantlab
        FILES_MATCHING PATTERN "*.h")
install(FILES docs/ARCHITECTURE.md DESTINATION share/quantlab)

message(STATUS "QuantLab v${PROJECT_VERSION}")
message(STATUS "  Compiler:    ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  C++ Std:     ${CMAKE_CXX_STANDARD}")
message(STATUS "  Build Type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "  Arrow:       ${USE_ARROW}")
message(STATUS "  SQLite:      ${USE_SQLITE}")
message(STATUS "  Python:      ${BUILD_PYTHON}")
