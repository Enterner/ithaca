cmake_minimum_required(VERSION 3.24)

# ── plugin/llm_cloud —— OpenAI 兼容流式 provider（独立工程）───────────────────
# 独立构建：cd plugin/llm_cloud && cmake --preset debug && cmake --build --preset debug
# 产物：plugin/llm_cloud/build-<preset>/<config>/llm_cloud_plugin.dll

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vc_plugin_llm_cloud LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/nostos_root.cmake")

if(NOT TARGET nostos::core)
    add_subdirectory("${NOSTOS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_nostos" EXCLUDE_FROM_ALL)
endif()
include("${NOSTOS_ROOT}/cmake/plugin_module.cmake")

nostos_plugin_module(llm_cloud_plugin plugin.cpp)
# kit：nlohmann/json（请求体/响应块编解码）；winhttp：系统 TLS 客户端（Tier 2，零第三方）
target_link_libraries(llm_cloud_plugin PRIVATE nostos::kit)
if(WIN32)
    target_link_libraries(llm_cloud_plugin PRIVATE winhttp)
endif()
target_include_directories(llm_cloud_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../contract")
