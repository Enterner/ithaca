# 定位 nostos 内核仓根。优先级：
#   1. -DNOSTOS_ROOT=... （显式传入，非空则不覆写）
#   2. 环境变量 NOSTOS_ROOT（内核仓开发者日常覆盖用——本地内核领先子模块时指过去）
#   3. 仓内子模块 external/nostos（git clone --recurse-submodules 或
#      `git submodule update --init` 后即存在——clone 即建，零配置）
#
# 注意 FORCE 的必要性：`set(... CACHE ...)` 在条目已存在时不覆写。若先以空值落了缓存
# （环境变量不存在时 $ENV{} 展开为空串），不带 FORCE 的回退值永远写不进去。
# 只在"未定义或为空"时才 FORCE，用户显式 -DNOSTOS_ROOT 传入的非空值不会被覆盖。
if(NOT DEFINED NOSTOS_ROOT OR NOSTOS_ROOT STREQUAL "")
    if(DEFINED ENV{NOSTOS_ROOT} AND NOT "$ENV{NOSTOS_ROOT}" STREQUAL "")
        set(NOSTOS_ROOT "$ENV{NOSTOS_ROOT}" CACHE PATH "nostos 内核仓根目录" FORCE)
    elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../external/nostos/CMakeLists.txt")
        set(NOSTOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../external/nostos"
            CACHE PATH "nostos 内核仓根目录（仓内子模块）" FORCE)
    endif()
endif()
if(NOT DEFINED NOSTOS_ROOT OR NOSTOS_ROOT STREQUAL ""
   OR NOT EXISTS "${NOSTOS_ROOT}/CMakeLists.txt")
    message(FATAL_ERROR
        "NOSTOS_ROOT 不指向 nostos 内核仓根（找不到 ${NOSTOS_ROOT}/CMakeLists.txt）。\n"
        "仓内子模块缺失时执行：git submodule update --init\n"
        "或自行指内核仓：-DNOSTOS_ROOT=<内核仓根> 或环境变量 NOSTOS_ROOT。")
endif()
message(STATUS "ithaca: NOSTOS_ROOT = ${NOSTOS_ROOT}")
