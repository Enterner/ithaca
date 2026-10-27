# nostos 开发观察（活文档）

> 规则见 AGENTS.md：只收开发过程真实触发的问题（每条绑定触发场景），只提方向与
> 理由，是否实施由内核节奏决定（改内核在子模块 `external/nostos` 工作树直接改）。
> 新批次覆盖式更新「当前发现」；已落地的结论压缩进「落地状态」。

## 当前发现（批次：内核观察点收口 · 2026-10-27）

1. webui 全量单线程化（E3）：挂起-补响应试点验证后确认 data_mu_ 的真正锚点是
   GET 类端点（select 线程读宿主线程写的共享状态）——锁整体退役须 20+ 端点
   （含 GET）全部走挂起-补响应，select 线程退化为纯转发器；改造面大，另行
   立项。（触发场景：批次 E 试点实施中盘点触碰面时确认）
2. workspace search 的 regex 走 std::regex ECMAScript（无超时/步数上限）——灾难性
   回溯的构造模式会在 agent 线程长阻塞（单文件 1MB 上限内仍可分钟级挂起）。缓解
   方向：换手写有限自动机/限定迭代步数/或 engine 换 RE2 形态；std::regex 无步数
   钩子，属内核 kit 无对应原语。用户裁定另行立项。（触发场景：R4 审查 workspace
   新增 search 工具）

## 落地状态

| 日期 | 发现 | 状态 |
|------|------|------|
| 2026-10-27 | kit::http 默认 `max_request = 16KB` 对"对话输入即粘贴"类应用过小（webui 粘贴 35KB 实测 413），且注释写"请求头上限"实为整个请求缓冲上限 | 已落地（内核 7ef08f5 默认上调 2MB + 注释对齐语义；产品 aa2af06 webui 启动期 2MB 覆写退役） |
| 2026-10-27 | kit::http 404 零诊断 | 复核不立项（唯一消费方 webui 已在路由未命中处落 log——api 分发尾部的 404 诊断行；kit 无路由表、handler 返回的 404 是消费方决策，内核再记一层即重复噪音） |
| 2026-10-27 | kit::http broadcast 死锁无防护（handler 在 select 线程持连接表锁同步执行，事件回调链里再进 broadcast = 同线程非递归锁二次进入，整服务无声挂死） | 已落地（内核 3698e49 线程局部重入闸门：派发中线程的 broadcast 丢弃并计入 stats.reentrant_broadcasts；跨线程锁序形态随后由挂起-补响应原语结构性消解） |
| 2026-10-27 | WinHTTP GET 状态码探测样板四份（shell HttpGetOk/ShutdownPort、webui prober 内联、webui 模型清单拉取需 GET+Auth+整读 body） | 已落地（内核 21aec9e feat(kit) win_http `http_fetch` 一发请求小件 + 产品 e2cf25d 四处八步管线收编为单调用，净 -105 行；shell 首次挂 vendored 内核 include 路径） |
| 2026-10-27 | kit::http 缺「挂起-补响应」原语：webui 双线程数据面锁序纪律全靠人工维持（提案 docs/temp/内核http挂起补响应-提案.md） | 已落地分期（内核 efdae1e 原语 + 产品 7f4f342 试点 /api/input 与 /api/decision——文件写离开 select 线程、pending 读复位竞态对消失；E3 全量迁移另行立项，见当前发现） |
| 2026-10-26 | kit::PipedProcess 缺「stdin 管道 + env 块覆盖」两个形态，mcp 插件收编不了：mcp 的 stdio 传输需要**主动写子进程 stdin**（JSON-RPC 请求通道，kit 现为继承父 stdin）与**env 覆盖**（server 配置的 env 字段），只能自带 ChildPipes；「cmd /c npx 链只杀直接子进程 ⇒ 孤儿 node 持管道写端 ⇒ 读线程永挂、热重载死锁」修复时，Job 树杀模式在插件层本地复制了一份（触发场景：MCP 边界实测撞上热重载死锁，对齐 kit 现有能力时发现缺口） | 已落地（内核 stdin_pipe / env_overrides_utf8 两形态 + 产品仓 ChildPipes 整体收编，插件层约 90 行 Win32 拷贝清退；mcp 侧只剩命令行拼装引号策略与 json→env 转换；副产品：旧 ChildPipes 无析构的管道句柄泄漏一并消除；边界/自然退出实盘 + check 全绿。内核侧已随 8259535 提交） |
| 2026-10-26 | vendored json 挂载点去掉 third_party 目录层级（用户裁定）：include/nostos/third_party/nlohmann → include/nostos/nlohmann——ODR 防护不变（头仍在 nostos/ 名字空间内，不与消费者自带 nlohmann 顶名），撤掉的只是路径里冗余的出处标记 | 已落地（内核 f969511 + 本仓 10 处 include 跟迁 + 子模块指针前移） |
| 2026-10-26 | 内核 7e0246c（审查修复批次）吸收：vendored nlohmann 移入 nostos/third_party 命名空间（本仓 10 处 include 跟迁，消 ODR 顶名隐患）、/utf-8 挂 core INTERFACE（本仓 host/单测两处重复旗标删除）、ABI 单层门控与事件单表/Scope::effect 修复经 wrapper 层自动受益（poster 均为 activate 期领取，in_session 新执法不咬） | 已落地（子模块指针前移 + 消费面跟随，check 全绿） |
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
