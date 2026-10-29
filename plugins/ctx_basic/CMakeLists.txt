cmake_minimum_required(VERSION 3.24)

# ── plugin/ctx_basic —— 上下文管理策略插件（独立工程）─────────────────────────
# 独立构建：cd plugin/ctx_basic && cmake --preset debug && cmake --build --preset debug
# 产物：plugin/ctx_basic/build-<preset>/<config>/ctx_basic_plugin.dll

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vc_plugin_ctx_basic LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/nostos_root.cmake")

if(NOT TARGET nostos::core)
    add_subdirectory("${NOSTOS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_nostos" EXCLUDE_FROM_ALL)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/plugin_module.cmake")

nostos_plugin_module(ctx_basic_plugin plugin.cpp)
# kit：nlohmann/json（plan/ingest/recall 的 JSON 数据面）
target_link_libraries(ctx_basic_plugin PRIVATE nostos::kit)
target_include_directories(ctx_basic_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../contract")
target_include_directories(ctx_basic_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../shared")
