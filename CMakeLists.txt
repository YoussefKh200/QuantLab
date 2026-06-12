cmake_minimum_required(VERSION 3.20)
project(QuantFusion VERSION 1.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-O3 -march=native -Wall -Wextra -Wpedantic
        -Wno-unused-parameter -ffast-math -funroll-loops -DNDEBUG)
elseif(MSVC)
    add_compile_options(/O2 /W4 /arch:AVX2)
endif()

# ── Main executable (single translation unit; all modules are header-only) ──
add_executable(quantfusion main.cpp)
target_include_directories(quantfusion PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Threads REQUIRED)
target_link_libraries(quantfusion Threads::Threads)

# ── Optional: SQLite persistence backend ────────────────────────────────────
option(USE_SQLITE "Enable SQLite persistence backend (storage/StorageEngine.h)" OFF)
if(USE_SQLITE)
    find_package(SQLite3 REQUIRED)
    target_link_libraries(quantfusion SQLite::SQLite3)
    target_compile_definitions(quantfusion PRIVATE QL_USE_SQLITE)
endif()

# ── Optional: Pybind11 Python bridge ────────────────────────────────────────
option(BUILD_PYTHON "Build Python bindings (python_bridge/PyBridge.cpp)" OFF)
if(BUILD_PYTHON)
    find_package(pybind11 REQUIRED)
    pybind11_add_module(quantfusion_py src/python_bridge/PyBridge.cpp)
    target_include_directories(quantfusion_py PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(quantfusion_py PRIVATE Threads::Threads)
    if(USE_SQLITE)
        target_link_libraries(quantfusion_py PRIVATE SQLite::SQLite3)
        target_compile_definitions(quantfusion_py PRIVATE QL_USE_SQLITE)
    endif()
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

message(STATUS "QuantFusion v${PROJECT_VERSION} -- C++${CMAKE_CXX_STANDARD}")
message(STATUS "  USE_SQLITE:    ${USE_SQLITE}")
message(STATUS "  BUILD_PYTHON:  ${BUILD_PYTHON}")
message(STATUS "  BUILD_TESTS:   ${BUILD_TESTS}")
