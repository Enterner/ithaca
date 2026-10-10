cmake_minimum_required(VERSION 3.24)

# ── Ithaca —— Nostos 内核之上的可逆编程代理 ──────────────────────────────────
#
# 顶层聚合工程：一条命令构建宿主/桌面壳/全部插件/单测。
#   cmake --preset debug && cmake --build --preset debug
# 内核（nostos）是独立仓库，不进本仓：构建期经 NOSTOS_ROOT 指向（见 cmake/nostos_root.cmake）。
#
# 各子目录（host/ shell/ plugins/* tests/unit）也可独立构建——它们各自带
# if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR) 的独立工程分支。
project(ithaca VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 多配置生成器（VS）：Debug/RelWithDebInfo 的产物统一收进 build/<preset>/bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
# 多配置生成器（VS）会自动追加 Debug/ 子目录——压平到 bin/，启动器与 e2e 路径才稳定
foreach(OUTPUTCONFIG ${CMAKE_CONFIGURATION_TYPES})
    string(TOUPPER ${OUTPUTCONFIG} OUTCFG)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${OUTCFG} ${CMAKE_BINARY_DIR}/bin)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${OUTCFG} ${CMAKE_BINARY_DIR}/bin)
endforeach()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/nostos_root.cmake")

if(NOT TARGET nostos::core)
    add_subdirectory("${NOSTOS_ROOT}" "${CMAKE_BINARY_DIR}/_nostos" EXCLUDE_FROM_ALL)
endif()

add_subdirectory(host)
add_subdirectory(shell)
add_subdirectory(plugins/agent)
add_subdirectory(plugins/llm_cloud)
add_subdirectory(plugins/llm_mock)
add_subdirectory(plugins/runner)
add_subdirectory(plugins/webui)
add_subdirectory(plugins/workspace)
add_subdirectory(plugins/mcp)
add_subdirectory(tests/unit)
