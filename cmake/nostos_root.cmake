# 定位 nostos 内核仓根（ithaca 不复制内核，构建期指过去）。
# 优先级：-DNOSTOS_ROOT=... > 环境变量 NOSTOS_ROOT > 本机默认值（改机器改这一行或设环境变量）。
#
# 注意 FORCE 的必要性：`set(... CACHE ...)` 在条目已存在时不覆写。若先以空值落了缓存
# （环境变量不存在时 $ENV{} 展开为空串），不带 FORCE 的机器默认值永远写不进去。
# 只在"未定义或为空"时才 FORCE，用户显式 -DNOSTOS_ROOT 传入的非空值不会被覆盖。
if(NOT DEFINED NOSTOS_ROOT OR NOSTOS_ROOT STREQUAL "")
    if(DEFINED ENV{NOSTOS_ROOT} AND NOT "$ENV{NOSTOS_ROOT}" STREQUAL "")
        set(NOSTOS_ROOT "$ENV{NOSTOS_ROOT}" CACHE PATH "nostos 内核仓根目录" FORCE)
    else()
        set(NOSTOS_ROOT "E:/Project/c++/nostos" CACHE PATH "nostos 内核仓根目录（本机默认）" FORCE)
    endif()
endif()
if(NOT EXISTS "${NOSTOS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "NOSTOS_ROOT 不指向 nostos 内核仓根（找不到 ${NOSTOS_ROOT}/CMakeLists.txt）。\n"
        "请设 -DNOSTOS_ROOT=<内核仓根> 或环境变量 NOSTOS_ROOT 后重新配置。")
endif()
message(STATUS "ithaca: NOSTOS_ROOT = ${NOSTOS_ROOT}")
