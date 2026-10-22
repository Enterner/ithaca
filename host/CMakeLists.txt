cmake_minimum_required(VERSION 3.24)

# ── host/ —— ithaca_host：ithaca 通用宿主（独立工程）────────────────────────────
#
# 它只做四件事：装配内核、按路径加载插件（LazyGate 懒激活）、16ms 一拍排水、干净退出。
# 不含任何业务 —— 它不知道 agent / llm 是谁；唯一的应用知识是契约头（contract/）
# 与 --selftest（M0 冒烟：直接驱动 vc.llm.v1 校验全链路，业务编排永远在 agent，M3）。
#
# 独立构建：cmake -S app -B app/build-debug && cmake --build --preset debug（在本目录）

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
    "${NOSTOS_ROOT}/include")

if(MSVC)
    # /utf-8：源码与控制台输出都是 UTF-8；GCC/Clang 默认即 UTF-8。
    target_compile_options(ithaca_host PRIVATE /permissive- /EHsc /utf-8)
else()
    target_compile_options(ithaca_host PRIVATE -Wall -Wextra)
endif()

nostos_copy_asan_runtime(ithaca_host)
