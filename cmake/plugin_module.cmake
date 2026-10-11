# ithaca → 内核兼容壳：转发 nostos 的 plugin_module.cmake。
#
# 内核版按它自家文档的布局推路径——NOSTOS_ROOT 指"内核父目录"
# （include "${NOSTOS_ROOT}/nostos/cmake/asan_runtime.cmake"），而 ithaca 全仓
# 约定 NOSTOS_ROOT 指内核仓根。这里把变量临时换指内核父目录再转发内核版，
# 内核演进即跟随，插件侧一律 include 本文件、不直接 include 内核那份。
if(NOT NOSTOS_ROOT OR NOT EXISTS "${NOSTOS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR "cmake/plugin_module.cmake：先 include cmake/nostos_root.cmake 定位内核仓根")
endif()
set(_ITHACA_KERNEL_ROOT "${NOSTOS_ROOT}")
get_filename_component(_ITHACA_KERNEL_PARENT "${_ITHACA_KERNEL_ROOT}/.." ABSOLUTE)
set(NOSTOS_ROOT "${_ITHACA_KERNEL_PARENT}")
include("${_ITHACA_KERNEL_ROOT}/cmake/plugin_module.cmake")
unset(NOSTOS_ROOT)          # 还原：内核父目录只在那一次 include 里有效
unset(_ITHACA_KERNEL_ROOT)
unset(_ITHACA_KERNEL_PARENT)
