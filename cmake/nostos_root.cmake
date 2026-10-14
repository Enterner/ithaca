# 定位 nostos 内核仓根。优先级：
include_guard(GLOBAL)
if(NOT DEFINED NOSTOS_ROOT OR NOSTOS_ROOT STREQUAL "")
    if(DEFINED ENV{NOSTOS_ROOT} AND NOT "$ENV{NOSTOS_ROOT}" STREQUAL "")
        set(NOSTOS_ROOT "$ENV{NOSTOS_ROOT}" CACHE PATH "nostos 内核仓根目录" FORCE)
    elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../external/nostos/CMakeLists.txt")
        set(NOSTOS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../external/nostos"
            CACHE PATH "nostos 内核仓根目录（仓内子模块）" FORCE)
    endif()
    
endif()
message(STATUS "ithaca: NOSTOS_ROOT = ${NOSTOS_ROOT}")
