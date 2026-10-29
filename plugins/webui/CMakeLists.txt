cmake_minimum_required(VERSION 3.24)

# ── plugin/webui —— Web 前端插件（独立工程）──────────────────────────────────
# 独立构建：cd plugin/webui && cmake --preset debug && cmake --build --preset debug
# 产物：plugin/webui/build-<preset>/<config>/webui_plugin.dll

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(vc_plugin_webui LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/nostos_root.cmake")

if(NOT TARGET nostos::core)
    add_subdirectory("${NOSTOS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_nostos" EXCLUDE_FROM_ALL)
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/plugin_module.cmake")

nostos_plugin_module(webui_plugin
    src/plugin.cpp      # 生命周期 + HTTP 路由 + 通用工具 + NOSTOS_PLUGIN 入口
    src/picker.cpp      # 原生选目录对话框 + /api/pick-folder
    src/events.cpp      # 事件总线订阅
    src/sessions.cpp    # 会话注册表/transcript + /api/session* + 上下文构成
    src/config.cpp      # 供应商两层配置 + /api/config + 模型清单拉取
    src/fs_watcher.cpp  # 文件树真监听
    src/prober.cpp      # 兄弟实例健康探测 + /api/shell/sessions
    src/api.cpp         # 对话动作端点（输入/裁决/权限模式/模型热切/会话命令）
    src/attachments.cpp # 附件管线（上传落盘 + 多模态载荷 + base64 编解码）
    src/api_fs.cpp      # 工作区代理端点（文件树/查看器/MCP 状态）
)
# 内部头（include/webui_plugin.hpp：类声明 + 翻译单元职责地图）
target_include_directories(webui_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
# kit::http 的实现在 kit 目标里；事件订阅与 JSON 编解码同款依赖
target_link_libraries(webui_plugin PRIVATE nostos::kit nostos::kit_http)
# 原生文件夹选择对话框：shell32（SHBrowseForFolder）+ ole32（CoInitializeEx/CoTaskMemFree）
# 实例健康探测（多实例聚合 alive 真值）：winhttp
if (WIN32)
    target_link_libraries(webui_plugin PRIVATE shell32 ole32 winhttp)
endif()
target_include_directories(webui_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../contract")
target_include_directories(webui_plugin PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../shared")
