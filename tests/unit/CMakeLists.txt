cmake_minimum_required(VERSION 3.24)

# ── tests/unit/ —— ithaca 单元测试（产品/插件纯逻辑层）───────────────────────
# 内核边界纪律：产品与插件的测试不进内核仓，所以自备一份。
# 构建：cmake --preset debug && cmake --build --preset debug（build.mjs 会顺带运行）。

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ithaca_unit_tests LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/nostos_root.cmake")

if(NOT TARGET nostos::core)
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
    test_proc.cpp
    test_read_paged.cpp
    test_attachments.cpp)
target_include_directories(ithaca_unit_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../contract"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../shared"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/workspace"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/llm_cloud"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/agent"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/runner"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../plugins/webui")
target_link_libraries(ithaca_unit_tests PRIVATE nostos::core)

if(MSVC)
    # /utf-8 /EHsc /permissive- 经 nostos::core INTERFACE 传递，本目标勿重复添加
    target_compile_options(ithaca_unit_tests PRIVATE /W4)
else()
    target_compile_options(ithaca_unit_tests PRIVATE -Wall -Wextra)
endif()
