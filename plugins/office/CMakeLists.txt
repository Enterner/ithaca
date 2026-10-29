cmake_minimum_required(VERSION 3.24)

# ── plugin/office —— 办公文档抽取插件（独立工程）─────────────────────────────
# 独立构建：cd plugin/office && cmake --preset debug && cmake --build --preset debug
# 产物：plugin/office/build-<preset>/<config>/office_plugin.dll

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vc_plugin_office LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/nostos_root.cmake")

if(NOT TARGET nostos::core)
    add_subdirectory("${NOSTOS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_nostos" EXCLUDE_FROM_ALL)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/plugin_module.cmake")

nostos_plugin_module(office_plugin plugin.cpp)
# kit 的 include 目录：分页出参的 JSON 编解码用 nlohmann/json（版本随内核 vendored）。
target_link_libraries(office_plugin PRIVATE nostos::kit)
target_include_directories(office_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../contract")
target_include_directories(office_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../shared")
