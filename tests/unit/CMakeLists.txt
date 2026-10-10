cmake_minimum_required(VERSION 3.24)

# ── tests/unit/ —— ithaca 单元测试（纯逻辑层：jail / edit / snapshots）────────
# 内核边界纪律：产品与插件的测试不进内核 CI（nostos_test），所以自备一份。
# 构建：cmake --preset debug && cmake --build --preset debug（build.mjs 会顺带运行）。

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ithaca_unit_tests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/nostos_root.cmake")

if(NOT TARGET nostos::kit)
    add_subdirectory("${NOSTOS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_nostos" EXCLUDE_FROM_ALL)
endif()

add_executable(ithaca_unit_tests
    main.cpp
    ithaca_test_framework.hpp
    test_jail.cpp
    test_edit.cpp
    test_diff.cpp
    test_context.cpp
    test_permission.cpp
    test_snapshots.cpp
    test_sse.cpp
    test_tools_bridge.cpp
    test_proc.cpp)
target_include_directories(ithaca_unit_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/workspace"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/llm_cloud"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/agent"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/runner")
target_link_libraries(ithaca_unit_tests PRIVATE nostos::core nostos::kit)

if(MSVC)
    target_compile_options(ithaca_unit_tests PRIVATE /W4 /utf-8 /EHsc /permissive-)
else()
    target_compile_options(ithaca_unit_tests PRIVATE -Wall -Wextra)
endif()
