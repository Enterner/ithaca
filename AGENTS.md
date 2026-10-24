# ithaca 项目指令（DSH 沙盒控制 · 工作区级）

> 规范架构（对齐 agents.md 主流做法）：**每次编辑必踩的热规则内联于此，全文在 docs/
> 单一事实源**——本文件摘要与 docs/ 冲突时以 docs/ 为准。AGENTS.md 与 docs/ 均不入库
> （机器本地指令链，刻意设计），换机 clone 需重建本文件与 docs/。

## 常用命令

- 构建 + 单测：`npm run build`（自动清成对代理环境变量防 MSBuild MSB6001；含 vite 前端构建）
- **提交前验证（固定顺序，全绿才可提交）：`npm run check`** = build → regress → e2e:all
- 开发启动 `npm run dev`；打包 `npm run pack`；单测 `npm test`；回归 `npm run regress`
- 评测 `npm run eval`（上下文压缩 A/B 指标）——不进 check 链，改折叠逻辑后手动跑

## 测试纪律（写/改任何测试必踩；全文与裁决：docs/测试规范.md）
写或改任何测试（C++ 单测 / API e2e / UI e2e / 回归 golden）前，先读 docs/测试规范.md
（单一事实源：分层、host fixture、等待纪律、接线与已知坑），不要另起炉灶。

- **未经用户明确要求，不得编写测试用例**（包括修复过程中的"顺手补测"）——用户开口才写。
- **等事实，不等时间**：turn 收口 = 轮询 `/api/transcript` 出现 assistant 记录；
  禁止固定 sleep 等业务事件；时序抖动偶发失败先修等待逻辑，不许靠 retries 掩盖。
- 测试一律收编进 `tests/`（单测/回归 golden/API e2e/UI e2e 四层），`scripts/` 只放
  任务入口薄壳；新测试必须挂进 npm——没接线的测试视为不存在。
- 修 bug 两层各补一条：单测锁函数行为，e2e 锁用户可见行为。

## 注释纪律（写/改任何代码必踩；全文与裁决：docs/注释规范.md）

写或改任何代码前先读 docs/注释规范.md（注释随代码走，不是独立动作）。

- **中文注释**；写 why 与不变式，不写 what——复述代码的注释删。
- 自包含：不写「详见 docs/xxx」；历史归 commit，注释只陈述现状与不变式。
- 统一标记 `NOTE:` / `踩过:` / `TODO:`；公开面（store.js 的 export、plugin.cpp 的
  `api_*`）必须带文档注释；license 头闭源期不写，公开时统一补。

## Skills（跨 agent 通用）

正本统一在 `skills/<名>/SKILL.md`（Agent Skills 标准格式），各专属目录只放指回正本的
薄指针。现有：`write-tests`（写/改测试前必读，正文 = docs/测试规范.md）。接线步骤见
`skills/README.md`。
- **降级纪律**：runtime 不支持 skill discovery 就直读正本 SKILL.md；仍拿不到就停下问
  用户，不得凭记忆猜规范。

## 强制规则：nostos 可优化点探测

所有 ithaca 开发工作（写宿主、插件、契约、测试、构建脚本的任何时候）都必须探测 nostos
内核的可优化点。本规则为**必须**，按以下约束执行：

1. **时机**：每个开发批次（里程碑/大改动）进行中与收尾时各过一遍。
2. **准入**：只收**开发过程中真实触发**的问题（每条绑定触发场景：在写什么时撞上），
   不为了找而找；可优化点必须**符合 nostos 的职责边界**——内核只管可逆组合、C ABI、
   能力层与 SDK 便利性，应用层逻辑（编辑格式、审批策略、UI 等）不属于内核，不得塞给内核。
3. **记录**：写入 `docs/nostos-开发观察.md`——**只保留最新的一份**（活文档）：新批次
   覆盖式更新「当前发现」区；已落地的结论压缩进「落地状态」表；被覆盖的历史细节随 git
   历史留存，不在文件里堆积。
4. **例外**：本次批次确实无可优化时，在「当前发现」区记一行「本批次无可优化点」即可，
   不得硬凑。
5. **实施边界**：观察文档只提方向与理由；是否实施由内核仓节奏决定，未经用户明确要求
   不得直接改动内核仓。

## 边界（Ask first / Never）

**Ask first（先说明再动手）**：
- 改冻结契约 `contract/`（只许尾部追加；动既有面 = 破坏插件兼容，先确认）
- 重录回归 golden（`--update-golden`）——先说清基线变化为何是预期而非掩盖失败
- 新增依赖、改 npm/CI 接线、动 playwright 的 projects 分层

**Never（不做）**：
- 未经用户明确要求写测试用例
- 直接改动内核仓 `E:\Project\c++\nostos`（可优化点只进观察文档，见强制规则第 5 条）
- 破坏性 git 操作（reset --hard 丢弃工作区、force push）除非用户明确要求
- 新建 doc 文件直接放 `docs/` 根下——**默认一律 `docs/temp/`**（用户明确要求放 docs/
  时才例外；更新既有 docs/ 文档不受此限）

## 项目事实（避免每次重新发现）

- 内核仓：`E:\Project\c++\nostos`（独立 git 仓，有完整提交历史；旧路径 D:\CODE\c++\nostos
  已废弃。产品仓经 `external/nostos` 子模块消费；内核无自带测试套件，验证随本仓
  `npm run build` 与内核独立构建进行）。
- 本仓 git：本地提交，不推远端。
- MSBuild 对大小写成对代理变量敏感（HTTP_PROXY/http_proxy 同时在环境会抛
  MSB6001）——构建前清掉；Node 侧防御已内置在 `scripts/build.mjs`。
- 机器本地指令链：`.gitignore` 排除 AGENTS.md 与 docs/（clone 不携带规范，属刻意设计）。
