cmake_minimum_required(VERSION 3.24)

# ── plugin/llm_mock —— 确定性假 provider（独立工程）───────────────────────────
# 独立构建：cd plugin/llm_mock && cmake --preset debug && cmake --build --preset debug
# 产物：plugin/llm_mock/build-<preset>/<config>/llm_mock_plugin.dll

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vc_plugin_llm_mock LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/nostos_root.cmake")

if(NOT TARGET nostos::core)
    add_subdirectory("${NOSTOS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_nostos" EXCLUDE_FROM_ALL)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/plugin_module.cmake")

# 多翻译单元：入口/配置（plugin）· 请求登记（request）· 回放与回流（replay）；
# 类声明在 include/llm_mock_plugin.hpp
nostos_plugin_module(llm_mock_plugin
    src/plugin.cpp
    src/request.cpp
    src/replay.cpp)
# kit：nlohmann/json（turn 剧本的编解码，M3 起）
target_link_libraries(llm_mock_plugin PRIVATE nostos::kit)
target_include_directories(llm_mock_plugin PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../contract")