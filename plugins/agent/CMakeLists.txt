cmake_minimum_required(VERSION 3.24)

# ── plugin/agent —— turn 状态机插件（独立工程）────────────────────────────────
# 独立构建：cd plugin/agent && cmake --preset debug && cmake --build --preset debug
# 产物：plugin/agent/build-<preset>/<config>/agent_plugin.dll

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vc_plugin_agent LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/nostos_root.cmake")

if(NOT TARGET nostos::core)
    add_subdirectory("${NOSTOS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_nostos" EXCLUDE_FROM_ALL)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/plugin_module.cmake")

nostos_plugin_module(agent_plugin
    src/plugin.cpp      # 薄壳（订阅分发 + 子代理事件扇出）+ manifest + NOSTOS_PLUGIN 入口
    src/ctx_events.cpp  # 事件处理器（输入/done/错误/裁决）
    src/ctx_turn.cpp    # turn 状态机（分发/收口/上下文压缩执行/子代理 spawn 与回灌）
    src/ctx_session.cpp # 会话持久化 + mission 任务目标卡
    src/ctx_tools.cpp   # 工具桥 + 调用/结果人话摘要
    src/agent_types.cpp # 子代理类型注册表（.ithaca/agent/*.md + 内置 general）
)
# 内部头（include/agent_plugin.hpp：AgentCtx/AgentPlugin 声明 + 职责地图；
# context/permission/tools_bridge.hpp 留插件根——tests 直接引用，不迁）
target_include_directories(agent_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
# kit：nlohmann/json（messages/tools/工具结果的编解码）
target_link_libraries(agent_plugin PRIVATE nostos::kit)
target_include_directories(agent_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../contract")
