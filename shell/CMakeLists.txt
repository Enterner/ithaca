cmake_minimum_required(VERSION 3.24)

# ── shell/ithaca —— ithaca 桌面壳（Win32 + WebView2 内嵌 webui）────────────
# 独立构建：cmake -S shell -B shell/build（本目录无 presets；顶层构建走仓根
#           CMakePresets，产物落 build/<preset>/bin/）
# 宿主由壳 SpawnInstance 经 cmd 链拉起 ithaca_host.exe（附着已有实例时退出不动宿主）
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ithaca LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

set(WV2 "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/webview2/build/native")

add_executable(ithaca WIN32 ithaca.cpp app.rc)   # app.rc：品牌图标（标题栏/任务栏）
target_include_directories(ithaca PRIVATE "${WV2}/include")
# 产品级共享件（vckit 平台件 win/win_http，经 shared/ 裸名取用）——
# 不再挂内核头，winhttp 库已在下面链接
target_include_directories(ithaca PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../shared")
# WebView2 加载器动态链接：DLL 随构建拷到 exe 旁（静态库约 10MB 不入仓，见
# third_party/webview2/README.md）
target_link_libraries(ithaca PRIVATE
    "${WV2}/x64/WebView2Loader.dll.lib"
    user32 gdi32 ole32 oleaut32 advapi32 winhttp shell32 uuid dwmapi)
add_custom_command(TARGET ithaca POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${WV2}/x64/WebView2Loader.dll" "$<TARGET_FILE_DIR:ithaca>")
