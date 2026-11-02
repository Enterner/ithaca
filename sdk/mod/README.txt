ithaca mod SDK（最小自扩展套件）
================================

ithaca 支持"mod 式"自扩展：把 DLL 插件放进 <宿主 exe>\mods\ 目录即被自动装载
（重启宿主生效）。本 SDK 提供在没有源码的环境下构建这样一个 mod 所需的全部
头文件与构建脚本——与内置插件同构（C ABI 契约 + 发布三连），不是阉割面。

目录
  include\      全部产品头，单一 include 根（契约平铺 + shared 树 + nostos 树，
                三种 include 形态从同一 /I 全部可解）
  example\      最小示例 mod：hello_mod.cpp，发布 ithaca.tool.hello（hello 工具）
  build.cmd     一键构建（需要 Visual Studio C++ 工具集，无需 cmake）

写一个 mod
  1. 以 example\hello_mod.cpp 为起点：改 svc 名（ithaca.tool.<你的名字>）与
     工具语义
  2. build.cmd 你的.cpp 你的_plugin.dll
  3. 把 DLL 拷进 <宿主 exe>\mods\
  4. 重启宿主（应用内 设置 → 重启后端，约 1 秒同端口恢复）——agent 对话里
     即可调用新工具

约定与限制
  · 装载序：mods 在内置插件之后装载；mod 发出 vc.tool.register 通告，agent
    自动接入其工具表，无需任何注册配置
  · svc 名建议 ithaca.tool.<mod 名>；工具的 LLM 可见名 = <工具集名>.<工具名>
    （如 hello）
  · 已加载的 DLL 被宿主锁死：覆盖同名 mod 需先重启宿主、再删旧拷新（Windows
    平台惯例）；或换文件名并存
  · 工具级失败返回 NOSTOS_OK + {"ok":false,...}（这是回灌模型的正常通路）；
    NOSTOS_ERR_* 只留给基础设施级失败
  · C ABI 契约（contract 头）只在尾部追加不破坏——老 mod 对新宿主前向兼容
