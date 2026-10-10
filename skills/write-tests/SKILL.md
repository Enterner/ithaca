---
name: write-tests
description: 在 ithaca 仓库写或改任何测试（C++ 单测 / API e2e / UI e2e / 回归 golden）时使用——统一规范在 docs/测试规范.md（单一事实源），本 skill 只负责把 agent 引到那里
---

# 写测试前必读

测试规范的**单一事实源**是 `docs/测试规范.md`（仓库根相对路径）——
先完整读该文件，再动手写。它覆盖：

1. 目录与分层职责（tests/unit | regress | e2e，scripts 只留入口）
2. host fixture 用法（隔离 home、mock config 种子、pickStub、等待纪律）
3. UI 层专项（稳定选择器、请求面断言、重启重放写法）
4. 产品码测试钩子纪律、npm 接线与 `npm run check` 全绿才提交
5. 已知坑清单（`api()` 无 body 发 GET、handle 内禁 broadcast 等）

规范有更新时只改 `docs/测试规范.md`，本文件不承载内容。

> 本文件是 `skills/` 目录的 skill 正本。各家 agent 的专属目录
> （`.zcode/skills/`、`.claude/skills/` 等）只放指向这里的薄指针，新增 skill
> 照此办理——见 skills/README.md。
