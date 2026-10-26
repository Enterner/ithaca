# nostos 开发观察（活文档）

> 规则见 AGENTS.md：只收开发过程真实触发的问题（每条绑定触发场景），只提方向与
> 理由，是否实施由内核仓节奏决定。新批次覆盖式更新「当前发现」；已落地的结论
> 压缩进「落地状态」。

## 当前发现（批次：task 结果内联分档 · 2026-10-26）

本批次（agent 插件应用层：task 报告内联上限分档 + 结论头外置形态）无可优化点——全部改动收在应用层策略，未触碰内核能力面。

## 落地状态

| 日期 | 发现 | 状态 |
|------|------|------|
| 2026-10-26 | kit::PipedProcess 缺「stdin 管道 + env 块覆盖」两个形态，mcp 插件收编不了：mcp 的 stdio 传输需要**主动写子进程 stdin**（JSON-RPC 请求通道，kit 现为继承父 stdin）与**env 覆盖**（server 配置的 env 字段），只能自带 ChildPipes；「cmd /c npx 链只杀直接子进程 ⇒ 孤儿 node 持管道写端 ⇒ 读线程永挂、热重载死锁」修复时，Job 树杀模式在插件层本地复制了一份（触发场景：MCP 边界实测撞上热重载死锁，对齐 kit 现有能力时发现缺口）。方向：kit::PipedProcess 加 stdin 管道选项与 env 覆盖（env 合并块须按名排序——kit 头文件自记的 gle=87 坑），mcp 的 ChildPipes 可整体收编消重 | 方向已记录，待内核仓评估 |
| 2026-10-26 | vendored json 挂载点去掉 third_party 目录层级（用户裁定）：include/nostos/third_party/nlohmann → include/nostos/nlohmann——ODR 防护不变（头仍在 nostos/ 名字空间内，不与消费者自带 nlohmann 顶名），撤掉的只是路径里冗余的出处标记 | 已落地（内核 f969511 + 本仓 10 处 include 跟迁 + 子模块指针前移） |
| 2026-10-26 | 内核 7e0246c（审查修复批次）吸收：vendored nlohmann 移入 nostos/third_party 命名空间（本仓 10 处 include 跟迁，消 ODR 顶名隐患）、/utf-8 挂 core INTERFACE（本仓 host/单测两处重复旗标删除）、ABI 单层门控与事件单表/Scope::effect 修复经 wrapper 层自动受益（poster 均为 activate 期领取，in_session 新执法不咬） | 已落地（子模块指针前移 + 消费面跟随，check 全绿） |
| 2026-10-25 | kit::http 缺「挂起-补响应」原语：webui 双线程共享数据面只能插件自备递归互斥，且「锁内禁 broadcast/emit」锁序纪律全靠人工维持（触发场景：修 webui 跨线程竞态时逐点核对）。方向：handler 返回挂起、经 poster 补发响应，插件面单线程化 | 方向已记录，待内核仓评估 |
| 2026-10-25 | WinHTTP「GET 状态码探测」样板在本仓出现三份（shell HttpGetOk/ShutdownPort、webui prober 内联）：四层 open/connect/request/close 可收敛进 kit::win 薄探测小件（触发场景：审查冗余样板；当轮只修并发未消样板）。2026-10-26 第 4 份真实触发：webui「从 API 获取模型」需要 GET + Authorization 头 + 整读 body——比状态码探测更进一步，kit::win 小件若长成「一发 GET 收全文」形状可同时消四份 | 方向已记录，待内核仓评估 |
| 2026-10-25 | svc 单槽发布互斥维持够用（add 拒绝重复 id；子档案旁路服役），per-类型子模型/多会话槽需求出现时再立项命名实例 | 观察维持，不立项 |
| 2026-10-24 | EventHub 观测函数 slot 失明（knows/raw_listener_count 疑只统计广播通道）→ 复核确认两函数在 58dc137 里已并入 slot 通道计数（knows 探 slot_raw_、raw_listener_count 对全部 slot 槽位求和），观察条目当时未回收 | 已落地（随 58dc137 slot 路由同批实现，无需再动；per-slot 粒度的 slot_listener_count 无消费方，不立项） |
| 2026-10-24 | nostos_test 测试源码从未进内核 git 历史、已不可恢复（陈旧 exe 可跑通 18 例但源头丢失；旧路径 D:\CODE 已删除；远端当时不可达），CI 纪律与 README 引用悬空 → 裁决不重建：删除 README/src README 悬空指路与陈旧构建产物，两仓工作区指令同步摘除 nostos_test 要求 | 已清理（内核 b9cb9b4；产品仓 AGENTS.md/测试规范/本文档同步；内核后续如需回归，用例随真实批次重新立项） |
| 2026-10-24 | 插件事件回调内抛出被 emit_thunk 静默吞掉、且 EventHub 派发循环无订阅者隔离——同事件其余订阅者（如 UI 落盘）不再执行、无任何痕迹（排障触发：子代理任务回灌段异常后 turn 悬停，transcript 只有调用卡没有结果卡）。内核 on/on_slot 注册的订阅回调改为逐个隔离 + 吞掉必落 error 日志（log_swallowed noexcept 出口）；emit/emit_slot 机制级异常同样落日志，原生 C++ 异常语义不动 | 已落地（内核 c016617；产品仓同步加固：agent 事件处理器与回灌/发送续段统一异常兜底转 fail_turn 终态、开轮即落盘防重启丢请求、悬挂 tool_calls 尾巴清理防 400 错误循环） |
| 2026-10-24 | 事件总线无源标注（子代理事件隔离只能靠载荷尾追标志，run_line 泄漏产品侧不可修）→ 内核 slot 路由原语（on_slot/emit_slot，slot=0 广播兼容）+ 产品仓全量迁移（runner/llm 按槽发射、agent 槽订阅替代扇出、标志退役） | 已落地（内核 58dc137 + 产品仓迁移，提案与评审记录见 docs/temp/内核事件slot路由-提案.md） |
| 2026-10-21 | kit::process 缺管道能力 → 新增 `kit::PipedProcess`（stdout/stderr 管道 + Job 树杀 + LineSplitter，header-only，env 覆盖按「原语跟第一个真实消费方」先例延后）+ runner 非沙箱 spawn 收编（proc.hpp 只剩沙箱安全策略切片；产品 test_proc 四例成为该原语的行为回归） | 已落地（内核子模块工作树，随内核仓提交） |
| 2026-10-21 | 上下文压缩批次一（爆窗死局止血：硬保护独立/段内兜底/超窗自救/收口判定）与 UI e2e 修复（坏导入/桩面/全局杀/等待竞态 + 前端 /help 应答）——内核零改动 | 已落地（随批次实施，见 docs/上下文压缩-待办.md） |
| 2026-10-14 | 前端代码查看器（Monaco 只读 + /api/fs/read 代理）与查看器 diff 模式（反向重建对照） | 已落地（随批次实施，见 docs/查看器diff模式-方案.md） |
| 2026-10-08 | 上下文压缩阶段二（动态阈值 0.9×W + LLM 自总结折叠）与 llm 标签制并发——批次内直接实施，内核零改动 | 已落地（随批次实施，见 docs/ 两份方案） |
| 2026-10-08 | plugin_module.cmake 的 NOSTOS_ROOT 双语义（asan_runtime include 按 `${NOSTOS_ROOT}/nostos/cmake/` 拼路径；唯一消费方按内核仓根引用）——内核自身构建不经过此文件故 CI 不暴露，只咬外部插件消费方 | 已落地：统一为内核仓根语义，头注释用法示例同步（内核 aeedd2f） |
| 2026-10-06 | 跨插件同步服务调用无 SDK 便利件 | 已落地：kit `poster_submit_await`（内核 0236c61）——同步投递 + 丢弃路径唤醒等待方 + kit 块内自我等待防御；文件树批次消费 |
| 2026-10-06 | UI 形状原语不进内核（边界裁决：原语跟着第一个真实消费方走） | 已落地：pick_folder_dialog 下沉 webui 插件（置顶修复随迁），open_file_dialog/has_native_picker 零消费方直接删除，win.hpp 收窄为 UTF-8↔宽字符转换（内核 123f938） |
| 2026-10-05 | kit::http 404 零诊断 / broadcast 死锁无防护 | 方向已记录，待内核仓评估 |
