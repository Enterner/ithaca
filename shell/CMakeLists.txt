cmake_minimum_required(VERSION 3.24)

# ── shell/ithaca —— ithaca 桌面壳（Win32 + WebView2 内嵌 webui）────────────
# 独立构建：cd shell && cmake --preset debug && cmake --build --preset debug
# 产物：shell/build-<preset>/<config>/vc_app.exe（双击 = 开窗；宿主由壳经
#       vcweb.ps1 -NoBrowser 托管，附着已有实例时退出不动宿主）
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(ithaca LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(CMAKE_CXX_EXTENSIONS OFF)
endif()

set(WV2 "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/webview2/build/native")

add_executable(ithaca WIN32 ithaca.cpp app.rc)   # app.rc：品牌图标（标题栏/任务栏）
target_include_directories(ithaca PRIVATE "${WV2}/include")
target_link_libraries(ithaca PRIVATE
    "${WV2}/x64/WebView2LoaderStatic.lib"
    user32 gdi32 ole32 oleaut32 advapi32 winhttp shell32 uuid dwmapi)
