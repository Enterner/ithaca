# ithaca → 内核兼容壳：纯转发 nostos 的 plugin_module.cmake。
#
# 插件侧一律 include 本文件、不直接 include 内核那份（内核演进即跟随）。
# NOSTOS_ROOT 必须指向内核仓根本身（不是其父目录），先用
# cmake/nostos_root.cmake 定位。
if(NOT NOSTOS_ROOT OR NOT EXISTS "${NOSTOS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "cmake/plugin_module.cmake：先 include cmake/nostos_root.cmake 定位内核仓根")
endif()
include("${NOSTOS_ROOT}/cmake/plugin_module.cmake")
