# Ithaca（伊萨卡）

**代码的归途** —— 跑在 [Nostos](#依赖的-nostos-内核) 内核之上的可逆编程代理。

Ithaca 是一个本地优先的 AI 编程工作台：浏览器与桌面壳两种形态共用同一套
插件与事件流。它的差异化在**可逆**——每一次 AI 修改都先试算 diff、落盘即事实、
按轮次快照，任何一轮都可以整体撤销。你因此可以放心把代码交给它。

## 特性

- **插件化 agent 闭环**：LLM 编排（agent）与能力插件（workspace 文件事务、runner 命令执行）
  通过 C ABI 服务表解耦，新增工具插件零 agent 改动
- **可逆编辑**：dry-run 试算 → 落盘 → 按 turn 快照 → 一键撤销；hunk diff 行内高亮
- **三档权限模式**：suggest（逐条批准）/ auto-edit（编辑免批、命令逐条问）/ full-auto（沙箱自动执行）
- **多前端**：Web UI（Vue 3）、Windows 桌面壳（WebView2），同一份 HTTP/SSE + 事件契约
- **两层供应商配置**：多 API 供应商各自 Base URL / Key / 模型列表，热切即时生效
- **上下文压缩**：工具结果外置 + 迟滞批量压缩，长会话不爆上下文

## 依赖的 Nostos 内核

Ithaca 构建在 [Nostos](https://example.com/nostos)（C ABI 插件内核：可逆组合、事件总线、
能力层与 SDK）之上。内核是**独立仓库**，本仓不包含任何内核代码——构建期通过 `NOSTOS_ROOT`
指向内核仓根目录（见 `cmake/nostos_root.cmake`），两者各自演进、互不嵌入。

## 构建

环境：Windows 10/11、CMake ≥ 3.24、MSVC（VS 2022）、Node.js（任务入口）。

```powershell
# 1. 指向 Nostos 内核仓根（含 nostos/CMakeLists.txt 的目录）
$env:NOSTOS_ROOT = "<nostos 内核仓路径>"

# 2. 一条命令构建全部（宿主 + 桌面壳 + 全部插件 + 单元测试）
npm run build                # Debug（默认）
npm run build:release
```

产物统一在 `build/<preset>/bin/`（`ithaca_host.exe`、`ithaca.exe`、各插件 dll、单测）。

等价的原始 CMake 入口：`cmake --preset debug && cmake --build --preset debug`
（构建脚本会先清掉成对的代理环境变量——MSBuild 遇到仅大小写不同的重名代理变量抛 MSB6001）。

## 运行

| 入口 | 形态 | 说明 |
|---|---|---|
| `build\<preset>\bin\ithaca.exe`（双击） | 桌面窗口 | 自动拉起宿主，关窗即停 |
| `npm run dev` | 浏览器 + 控制台 | 宿主日志直打控制台，浏览器开 127.0.0.1:8790 |

首次使用在设置里填入 LLM API Key（DeepSeek 或任意 OpenAI 兼容端）。

## 测试

```powershell
npm test                    # 单元测试（57 例）
npm run selftest            # mock 全链路冒烟
npm run regress             # 回归（selftest 两遍一致 + golden）
npm run e2e                 # Web UI e2e（配置掩码/落盘/撤销/重启重放/SSE）
```

## 打包

```powershell
npm run pack                # 便携 zip + 安装包（pack/ 下各一份）
npm run pack:zip            # 只要便携 zip
npm run pack:redist         # 附带 VC++ 运行库
```

安装包为 per-user 安装（`%LOCALAPPDATA%\Programs\Ithaca`，免管理员），
缺 WebView2 运行时静默补装；编译需要 Inno Setup 6（`winget install JRSoftware.InnoSetup`）。

## 目录

```
contract/   宿主与插件的公共契约（C ABI、事件与服务名——冻结面，只尾部追加）
host/       通用宿主（ithaca_host.exe：装载插件、排水循环）
shell/      Windows 桌面壳（WebView2 内嵌 Web UI）
plugins/    agent 编排 / llm_cloud / llm_mock / runner / webui / workspace
ui/         Vue 3 + Naive UI 前端（构建产物 dist/ 随仓库分发）
tests/      单元测试 + 场景 + golden
scripts/    任务脚本（build/dev/regress/e2e/pack，零依赖 .mjs）
docs/       设计文档
```

## License

MIT
