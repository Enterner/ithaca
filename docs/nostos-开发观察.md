# nostos 开发观察（活文档）

> 规则见 AGENTS.md：只收开发过程真实触发的问题（每条绑定触发场景），只提方向与
> 理由，是否实施由内核节奏决定（改内核在子模块 `external/nostos` 工作树直接改）。
> 新批次覆盖式更新「当前发现」；已落地的结论压缩进「落地状态」。

## 当前发现（批次：上下文缓存命中优化 · 2026-10-30；观察 1–4 自前批次携带，仍开放）

**本批次无内核侧触发**：mission 卡跨轮冻结（缓存前缀纪律）全落在产品层
（shared/vcagent/context.hpp 策略纯函数 + agent 建档/折叠时序 + 契约 mission 槽
注释口径更新），零 C ABI 面变化、零内核缺口。既有观察无新增消费点。

**观察 1：kit::win 缺 ASCII 大小写折叠件**（触发场景：全库审查清点小写化循环，
找到 7 处手写——context.hpp 的 provider 错误文案折叠、workspace/office/webui 的
扩展名折叠 ×4、webui config 的名称去重折叠 ×2、host/profile.hpp 的清单项折叠；
应用侧 6 处已收编到产品共享件 `vcws::ascii_lower`）。host 的 profile.hpp
不能 include 插件域共享头，只能保留第 7 份——ASCII 折叠是平台无关的通用原语，
归 kit::win（与 utf8_to_wide 同层）可让宿主/插件/内核三域共用一份。

**观察 2：kit::win_http 可附 WinHTTP 错误码 → 人话映射**（触发场景：全库审查
发现 webui `winhttp_error_text_` 与 llm_cloud `last_error_text` 逐字同构 5 分支，
前者注释自认「复制而非共享——两插件无公共 TU」；插件逐个审查批次在 llm_cloud
再次撞见同款双份，观察仍开放）。错误文案语义归属 WinHTTP 平台层，收 kit 最顺。

**观察 3：win::http_fetch 的 URL crack 结果可暴露结构化出参**（触发场景：全库
审查发现 llm_cloud stream.cpp 的 `parse_endpoint` 手写 WinHttpCrackUrl 四段解析，
与 http_fetch 内部同款样板；http_fetch 现只收 URL 串，crack 结果（host/port/
path/secure）不外露，流式客户端无法复用其解析）。

**观察 4：kit::http Request 缺 query 解析**（触发场景：webui 手写 %XX 解码器
（现收编为插件内通用工具 query_val，三端点共用）；新增 /api/attachment 端点
时再次需要同款解析——transcript 的 `find("after=")`、agent-log 的 `find("?turn=")`
两处手挖依旧，共 5 个消费点。Request.target 原样保留（含 query）、无结构化取值
面，每个带参端点各自手搓）。方向：Request 增 query(name) 或解析好的键值表——
URL 解码语义单点，'+' 不折叠为空格等口径也随之固定。

**不立项记录**：webui 的 base64 编解码（60 行标准实现，唯一消费方；结构审查后
已隔离进 attachments.cpp 附件管线切片）；JSON 文件 temp+rename 原子落盘样板
（agent/webui 共 4 处，应用层文件 IO 不归内核）；ctx_basic 与 agent 的回灌/回查/
摘要/阈值镜像——产品侧已收编为 context.hpp 共享纯函数，无内核缺口。

**挂账：runner shell 路线调研与 MXC 观察项（2026-10-27，仍有效）**

agent 三轮实测后对 shell 层做了系统调研（业界对照 + 双路探针），结论与挂账：

- **业界三流派**：权限确认派（OpenCode/Cursor/Cline——无 OS 沙箱靠人审+规则）、
  POSIX 搬迁派（Claude Code——Git Bash 优先/PS 兜底，native 无沙箱、WSL 2 才有）、
  原生沙箱工程派（Codex 独一家——MXC/elevated/unelevated 三层实现，迭代一年余）。
  **没有任何厂家用 AppContainer，也没有人解决"bash + native Windows 沙箱"**。
- **探针数据**：非沙箱 Git Bash 全语义可用（退出码/短路/树杀/UTF-8，65ms/次）；
  沙箱内 msys（0xC0000022，BaseNamedObjects 共享节被 AC 命名空间拒）与 PowerShell
  （.NET runtime 同死）**全灭**——沙箱内 cmd 是唯一已验证 shell，结构性限制非配置可救。
- **已落地**：非沙箱 Git Bash 优先（检测链 env→PATH git 推导→常见路径）+ 脚本
  stdin 传递（bash -s + stdin_pipe，-c 引号转义链在 CreateProcess→bash 间断裂）+
  ITHACA_RUNNER_SHELL=cmd 强制覆盖（selftest/golden 跨机器一致性要求 shell 确定）；
  full-auto 保持 cmd。cmd 兜底的 OEM 码页乱码已修（见落地状态 2026-10-29）。
- **挂账（MXC 观察项）**：Microsoft eXecution Container（github.com/microsoft/mxc，
  开源多后端沙箱框架）的 BaseContainer 后端（新 OS 沙箱 API，Win11 24H2/25H2
  .9278+ 构建）是为"容器内跑真实工具链"设计——**msys 在 BaseContainer 下能否
  存活待实测**；若可行则"bash 语义 + 沙箱"合流（AppContainer 退役）。本机
  26200.9711 补丁已达标，MXC 铺开稳定后用探针重验。接入形态：打包 wxc-exec.exe
  （MXC 原生执行器）替代 runner 的 AppContainer 路径。

## 落地状态

| 日期 | 发现 | 状态 |
|------|------|------|
| 2026-10-29 | 跨插件反向 include（text.hpp/read_paged.hpp/iface.hpp/context.hpp 四头 14 条边；jail 一路此前已随 kit 收编清零）——产品级共享件不属内核职责，解绑方向 = 产品仓内共享 include 目录 | 已落地（产品仓 shared/ 共享树：namespace 子目录、准入 ≥2 插件消费、include 路径裸名形态；scripts/check_includes.mjs DEPS-lite 机械检查零白名单进 check 链首步；AGENTS.md「目录与 include 纪律」三级可见性成文。对齐 LLVM include/lib 分层与 Chromium DEPS 形态，反向验证探针确认拦截有效） |
| 2026-10-29 | poster 投递的两个语义缺口：闭包异常块内静默吞（挂起-补响应消费方闭包抛一次 = 连接槽位永久泄漏，只能每个消费方自建中央 catch）；亚生命周期对象投递不安全（agent 子会话 ctx 随 turn 销毁，闭包无法在会话级回收之外失效，推迟一拍只能收窄到主 ctx）。附带：EventHub 嵌套 emit 语义未文档化 | 已落地（内核 8bdae22：nostos_poster 尾部追加 post_owner/invalidate_owner 两槽——owner 令牌作废回收复用 on_drop 通路；kit poster_submit 异常钩子形态 + poster_submit_owner/poster_invalidate_owner；EventHub 头注释补嵌套 emit 实际语义。产品仓：webui defer_ 手写中央 catch 改传钩子；agent 子代理与主会话同规推迟一拍——cleanup_turn_ 销毁子 ctx 前作废 owner，旧宿主 BAD_STATE 回退同步；回归 golden 无基线变化） |
| 2026-10-29 | PipedProcess::release_handles 五句柄整体移交而消费方容器只收四句柄——stdin_pipe 的「推迟写后即关送 EOF」只能绕开 adopt 手动逐句柄接管；runner cmd 兜底路线中文输出乱码（OEM 码页字节流直按 UTF-8 净化，MXC 批次附带发现） | 已落地（内核 ebd583d：PipedHandles 补 take_stdin_write + 五句柄三面消费纪律；产品仓 ChildPipes 增 stdin 位、adopt 交五收五，bash 路径句柄体操收敛为 adopt + take_stdin——写端写后即关送 EOF 的语义不变；cmd 兜底输出按 CP_OEMCP 逐行解到 UTF-8 再净化，DBCS 尾字节不含 0x0A 故按行解码安全，bash 路线不 decode） |
| 2026-10-29 | kit 缺「路径沙箱」件：jail 三道闸（词法校验 + GetFullPathNameW 规范化复核 + reparse 逐组件扫描）是「接受模型给定路径」插件的共性安全原语，唯一实现挂在 workspace 插件内部，office/runner 反向 `../workspace/jail.hpp` include（独立构建被绑架） | 已落地（内核 a36a0d8：kit/path_jail.hpp 单源——三道闸原样上收、非 Windows 显式拒绝桩；产品仓 workspace/office/runner + test_jail/test_edit 六处 include 跟迁、workspace/jail.hpp 删除，跨插件反向 include 中 jail 一路清零。text/read_paged/iface 的反向 include 非安全原语，不属内核职责，维持观察） |
| 2026-10-29 | 「同服务多实现竞争 + 择一发布」样板第三份手写（llm_cloud/llm_mock/ctx_basic 逐字同构，热切语义与激活日志靠约定）；kit::win 缺可中断睡眠与环境块构建小件（prober/shell 分片睡眠两份手写；shell/runner 两份 env 块构建且策略已漂移——一份排序一份不排） | 已落地（内核 287c2cb：kit/active_gate.hpp 竞争发布门——env 门三态首发 + mine/other/keep 三值热切 + 幂等 + 激活/休眠日志统一落门内，插件只留表组装与载荷解读；kit::win 增 sleep_interruptible 与 build_env_block_w 单源，piped_process 内联实现退役。产品仓三 provider + prober/shell/runner 收编，约 90 行机械重复退役；mock 首发新增一行「已激活」、休眠文案统一（golden 按此重录）；ctx 首发发布失败从带病继续改为激活失败回滚） |
| 2026-10-27 | workspace search 的 regex 走 std::regex（回溯实现无步数钩子），灾难性回溯构造模式会在 agent 线程长阻塞 | 已落地（内核 kit/regex.hpp 有界正则——Thompson NFA 线性模拟，ECMAScript 子集，无回溯 ⇒ 灾难性模式构造上不可能挂死，步数预算另封顶；反向引用/环视解析期点名拒绝。抛弃式探针对拍 std::regex 25 例全过 + 灾难性模式 0ms + 420KB 文件 105ms；产品 workspace search 切换，std::regex 退役） |
| 2026-10-27 | webui 全量单线程化（挂起-补响应试点验证后的全量迁移）：data_mu_ 的真正锚点是 GET 类端点（select 线程读宿主线程写的共享状态） | 已落地（20+ 端点含全部 GET 经 defer_ 样板投宿主线程闭包、真响应经内核原语补发；data_mu_ 30 余处 lock_guard 与上下文缓存锁整体退役，tools_/in_flight_ 原子降为普通成员，select 线程只剩解析/投递与静态、SSE 直答；defer_ 编译期强制闭包返回 void——值返回闭包的 fall-through 是 UB 且爆点漂移到无关容器操作，check 全绿） |
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
| 2026-10-21 | 上下文压缩批次一（爆窗死局止血：硬保护独立/段内兜底/超窗自救/收口判定）与 UI e2e 修复（坏导入/桩面/全局杀/等待竞态 + 前端 /help 应答）——内核零改动 | 已落地（随批次实施；方案文件已随批次归档删除） |
| 2026-10-14 | 前端代码查看器（Monaco 只读 + /api/fs/read 代理）与查看器 diff 模式（反向重建对照） | 已落地（随批次实施；方案文件已随批次归档删除） |
| 2026-10-08 | 上下文压缩阶段二（动态阈值 0.9×W + LLM 自总结折叠）与 llm 标签制并发——批次内直接实施，内核零改动 | 已落地（随批次实施；方案文件已随批次归档删除） |
| 2026-10-08 | plugin_module.cmake 的 NOSTOS_ROOT 双语义（asan_runtime include 按 `${NOSTOS_ROOT}/nostos/cmake/` 拼路径；唯一消费方按内核仓根引用）——内核自身构建不经过此文件故 CI 不暴露，只咬外部插件消费方 | 已落地：统一为内核仓根语义，头注释用法示例同步（内核 aeedd2f） |
| 2026-10-06 | 跨插件同步服务调用无 SDK 便利件 | 已落地：kit `poster_submit_await`（内核 0236c61）——同步投递 + 丢弃路径唤醒等待方 + kit 块内自我等待防御；文件树批次消费 |
| 2026-10-06 | UI 形状原语不进内核（边界裁决：原语跟着第一个真实消费方走） | 已落地：pick_folder_dialog 下沉 webui 插件（置顶修复随迁），open_file_dialog/has_native_picker 零消费方直接删除，win.hpp 收窄为 UTF-8↔宽字符转换（内核 123f938） |
