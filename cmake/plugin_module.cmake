# ithaca → 内核兼容壳：转发 nostos 的 plugin_module.cmake。
#
# 历史包袱说明：旧内核版按"父目录"布局推路径（include
# "${NOSTOS_ROOT}/nostos/cmake/asan_runtime.cmake"），与全仓"内核仓根"语义
# 相悖，本壳曾需临时换指父目录再转发。内核 aeedd2f 起语义统一为内核仓根，
# 壳退化为纯转发——插件侧一律 include 本文件、不直接 include 内核那份，
# 内核演进即跟随。
if(NOT NOSTOS_ROOT OR NOT EXISTS "${NOSTOS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "cmake/plugin_module.cmake：先 include cmake/nostos_root.cmake 定位内核仓根")
endif()
include("${NOSTOS_ROOT}/cmake/plugin_module.cmake")
