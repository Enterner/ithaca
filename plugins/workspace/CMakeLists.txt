cmake_minimum_required(VERSION 3.24)

# ── plugin/workspace —— 文件事务插件（独立工程）───────────────────────────────
# 独立构建：cd plugin/workspace && cmake --preset debug && cmake --build --preset debug
# 产物：plugin/workspace/build-<preset>/<config>/workspace_plugin.dll

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vc_plugin_workspace LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/nostos_root.cmake")

if(NOT TARGET nostos::core)
    add_subdirectory("${NOSTOS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_nostos" EXCLUDE_FROM_ALL)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/plugin_module.cmake")

nostos_plugin_module(workspace_plugin plugin.cpp)
# kit 的 include 目录：ops/results 的 JSON 编解码用 nlohmann/json（版本随内核 vendored，
# 经 nostos::kit 的 BUILD_INTERFACE 拿到——外部消费仓不必自带一份）。
target_link_libraries(workspace_plugin PRIVATE nostos::kit)
target_include_directories(workspace_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../contract")
