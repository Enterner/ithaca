cmake_minimum_required(VERSION 3.24)

# ── plugin/mcp —— MCP 客户端插件（通用 MCP 工具接入；独立工程）──────────────
# 独立构建：cd plugins/mcp && cmake --preset debug && cmake --build --preset debug

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vc_plugin_mcp LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/nostos_root.cmake")

if(NOT TARGET nostos::core)
    add_subdirectory("${NOSTOS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_nostos" EXCLUDE_FROM_ALL)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/plugin_module.cmake")

nostos_plugin_module(mcp_plugin
    src/plugin.cpp   # 生命周期 + 状态回发落盘 + NOSTOS_PLUGIN 入口
    src/session.cpp  # stdio 传输 + JSON-RPC 会话
    src/config.cpp   # 配置读取 + server 拉起握手
    src/tools.cpp    # 统一工具表 + 会话查找
)
# 内部头（include/mcp_plugin.hpp：数据结构 + 类声明 + 翻译单元职责地图）
target_include_directories(mcp_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
# kit：nlohmann/json（JSON-RPC 与工具 schema 编解码）+ win（UTF-8 ↔ 宽字符）
target_link_libraries(mcp_plugin PRIVATE nostos::kit)
target_include_directories(mcp_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../contract")
