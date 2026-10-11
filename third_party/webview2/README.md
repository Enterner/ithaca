# third_party/webview2 —— Microsoft WebView2 SDK（子集）

桌面壳（shell/ithaca.cpp）的编译依赖，取自 NuGet 包
`Microsoft.Web.WebView2` 1.0.4258.31（https://www.nuget.org/packages/Microsoft.Web.WebView2），
只随仓分发壳实际用到的那部分，其余（托管程序集、WinRT 投影、x86/arm64 等）不入库：

- `build/native/include/WebView2.h`、`WebView2EnvironmentOptions.h` —— C++ COM 头
- `build/native/x64/WebView2Loader.dll(.lib)` —— 加载器（壳**动态链接**导入库，
  构建时 DLL 拷到 bin，与 exe 同目录发布；静态库约 10MB 故不入仓）

运行前置：机器装有 Microsoft Edge WebView2 Runtime（Windows 10/11 一般自带；
缺了壳会弹窗提示，安装包也会静默补装）。许可证见 LICENSE.txt / NOTICE.txt。
