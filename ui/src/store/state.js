// 全局响应式状态（唯一出处）：跨领域共享的 S 只在此定义，领域行为分见同目录
// 各模块（api/records/sessions/actions/boot）——本文件不含任何逻辑。
import { reactive } from 'vue'

export const S = reactive({
  records: [],          // 已定格记录（重放 + SSE 收口）
  live: null,           // 流式块 {reasoning, text, open}（SSE 专属，不落盘；open 默认折叠）
  since: 0, lastId: -1,
  conn: 'off',          // 'on' | 'off'
  inFlight: false,
  lastPrompt: 0,
  rate: '0',
  // /api/config（两层供应商）
  providers: [], current: '',
  model: '', pmode: 'auto-edit',
  // 会话
  sessions: [], curSession: '', curWs: '', sessTitle: '',
  draft: null,   // 草稿态{workspace} ——＋新建到首条消息之间纯前端状态，
                 // 不发任何创建请求；首条消息经 /api/session/new 创建即初始化
  fsRev: 0,   // 文件树刷新时钟：applied/undone 时 +1，FileTree watch 它静默重拉已展开目录
  // 代码查看器（右栏文件态 ⇄ 查看态）：{path, content, truncated, loading, error}；
  // null = 右栏显示文件树。纯展示态——内容由 App.vue 经 /api/fs/read 拉取，不经 actions 层
  viewer: null,
  // 待裁决提案（ZCode 式决策坞：只在实时提案上出现，重放的历史提案不阻塞输入）
  pending: null,
  // 挂起等待（子代理/后台任务进行中）：waitingSince = 「等待 N 个…」通告到达
  // 时刻（挂起条计时用）；waitingLeft = 尚未核销的挂起数（tool 结果逐个核销）。
  // 纯前端可视态——取消走 /api/decision 4（agent 级联取消在途子请求）。
  waitingSince: 0, waitingLeft: 0,
  // 子代理过程视图：subView = 选中的子会话槽 turn_no（0 = 关闭）；subLive =
  // 各子代理的流式增量缓冲（turn_id → 累积文本；delta 只广播不落盘，重放无此节）；
  // subDone = 已收口的子代理（turn_id → true，主会话 spawn 卡的状态判定）；
  // subDetail = 各子代理的工具明细实时缓冲（turn_id → 记录数组；明细落独立
  // 存档，主会话不收，SSE 实时推送 + 右栏打开时 /api/agent-log 拉历史合并）。
  subView: 0, subLive: {}, subDone: {}, subDetail: {},
  inputText: '',
  // 待发送附件（+号上传/拖拽进来，发送时随消息带上相对路径并清空）：
  // {path: 工作区相对路径, name: 显示名}。文件实体已在 .ithaca/uploads 落盘
  attachments: [],
  booted: false,
  shell: false,   // 多实例壳模式：会话清单来自 /api/shell/sessions，切换 = 整页导航
  // 插件 UI 贡献：registry = 各插件 schema（/api/ui/registry，设置面板
  // 打开时拉取）；pluginVals = 各插件设置值（/api/config 的 plugins 节）。
  pluginUi: [], pluginVals: {},
  // 设置面「从 API 获取模型」的回执（SSE provider_models 瞬态事件，records 转
  // 投于此；Settings.vue watch 消费）。n 单调递增防同值吞 watch。
  provModelsEvt: null,
})

// 非响应式转态（记录游标/汇总卡聚合/速率窗口）：SSE 与重放两路径共用的
// 可变私有面。放属性容器而非裸导出 let——ESM 导入绑定只读，属性才可写。
export const X = {
  turnEdits: new Map(),   // 当前轮编辑聚合（apply/applied 累计，flushTurnEdits 结算）
  turnEditsTurn: 0,
  lastTurnId: null,       // 轮次边界判定（turn_id 变化 ⇒ 结算汇总卡）
  runs: new Map(),        // run_id → 输出块 {lines, exit, failed}（输出折叠进调用行，见 records）
  runCall: new Map(),     // run_id → 归属的 runner.run 调用行（「已启动」通告时配对）
}
