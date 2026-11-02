cmake_minimum_required(VERSION 3.24)

# ── host/ —— ithaca_host：ithaca 通用宿主（独立工程）────────────────────────────
#
# 它只做四件事：装配内核、按路径加载插件（LazyGate 懒激活）、排水循环（post 唤醒
# + 空闲 250ms 兜底拍）、干净退出。除 Ctrl+C 取消语义（vc.agent 特判）与 --selftest
# 冒烟外不含业务——应用知识只有契约头（contract/）与 profile 装配层。
#
# 独立构建（在本目录）：cmake -S . -B build-debug && cmake --build build-debug
# （本目录无 presets；顶层构建走仓根 CMakePresets，产物落 build/<preset>/bin/）

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ithaca_host LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/../cmake/nostos_root.cmake")

# 库：被别的工程包含时目标已存在，不能重复 add_subdirectory。
if(NOT TARGET nostos::loader)
    add_subdirectory("${NOSTOS_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/_nostos" EXCLUDE_FROM_ALL)
endif()
include("${NOSTOS_ROOT}/cmake/asan_runtime.cmake")

add_executable(ithaca_host main.cpp selftest.cpp)
target_link_libraries(ithaca_host PRIVATE nostos::loader)  # loader = 框架里唯一要编译的部分

target_include_directories(ithaca_host PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../contract"
    "${CMAKE_CURRENT_SOURCE_DIR}/../shared"
    "${NOSTOS_ROOT}/include")

if(MSVC)
    # /permissive- /EHsc /utf-8 经 nostos::core INTERFACE 传递（loader PUBLIC 链
    # core），本工程无需重复添加。
else()
    target_compile_options(ithaca_host PRIVATE -Wall -Wextra)
endif()

nostos_copy_asan_runtime(ithaca_host)
