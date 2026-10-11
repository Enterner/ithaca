cmake_minimum_required(VERSION 3.24)

# ── plugin/runner —— 命令执行插件（独立工程）──────────────────────────────────
# 独立构建：cd plugin/runner && cmake --preset debug && cmake --build --preset debug
# 产物：plugin/runner/build-<preset>/<config>/runner_plugin.dll

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vc_plugin_runner LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/nostos_root.cmake")

if(NOT TARGET nostos::core)
    add_subdirectory("${NOSTOS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_nostos" EXCLUDE_FROM_ALL)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/plugin_module.cmake")

nostos_plugin_module(runner_plugin plugin.cpp)
target_link_libraries(runner_plugin PRIVATE nostos::kit)  # json
target_include_directories(runner_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../contract")
