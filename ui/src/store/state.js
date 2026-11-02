// 全局响应式状态（唯一出处）：跨领域共享的 S 只在此定义，领域行为分见同目录
// 各模块（api/records/sessions/actions/boot）——本文件不含任何逻辑。
import { reactive } from 'vue'

/** 全局响应式状态唯一出处：各组件经 ../store 只读引用；字段行为见行内注释。 */
export const S = reactive({
  records: [],          // 已定格记录（重放 + SSE 收口）
  live: null,           // 流式块 {reasoning, text, open}（SSE 专属，不落盘；open 默认折叠）
  since: 0, lastId: -1,
  conn: 'off',          // 'on' | 'off'
  restarting: false,    // 重启后端在途（设置面板触发）：health 恢复前按钮停在「重启中…」
  inFlight: false,
  lastPrompt: 0,
  rate: '',   // 会话平均输出速率 tok/s（''=尚无计量，显示面隐藏）；records 增量采样维护
  // /api/config（两层供应商）
  providers: [], current: '',
  model: '', pmode: 'auto-edit',
  // 会话
  sessions: [], curSession: '', curWs: '',
  switchPending: false,   // 会话切换在途（乐观清屏后、服务端确认前）：补洞停拍
                          // ——/api/transcript 此时还在服务旧活跃会话，灌进来就
                          // 是上一会话的内容（同草稿门禁的机理）
  draft: null,   // 草稿态{workspace} ——＋新建到首条消息之间纯前端状态，
                 // 不发任何创建请求；首条消息经 /api/session/new 创建即初始化
  fsRev: 0,   // 文件树刷新时钟：applied/undone 时 +1，FileTree watch 它静默重拉已展开目录
  // 代码查看器（右栏文件态 ⇄ 查看态）：{path, kind, content, truncated, loading,
  // error, doc, note}；kind='pdf' = 版式预览态（OfficeViewer 拉取渲染），其余走
  // 文本抽取；note = 查看器警示条（预览回落原因等）。null = 右栏显示文件树。
  // 纯展示态——内容由 App.vue/OfficeViewer 拉取，不经 actions 层
  viewer: null,
  // 待裁决提案（ZCode 式决策坞：只在实时提案上出现，重放的历史提案不阻塞输入）
  pending: null,
  // 挂起等待（子代理/后台任务进行中）：waitingLeft = 尚未核销的挂起数
  // （tool 结果逐个核销，归零撤挂起条）。纯前端可视态——取消走 /api/decision 4
  //（agent 级联取消在途子请求）。
  waitingLeft: 0,
  // 子代理过程视图：subView = 选中的子会话槽 turn_no（0 = 关闭）；subLive =
  // 各子代理的流式增量缓冲（turn_id → 累积文本；delta 只广播不落盘，重放无此节）；
  // subDone = 已收口的子代理（turn_id → true，主会话 spawn 卡的状态判定）；
  // subDetail = 各子代理的工具明细实时缓冲（turn_id → 记录数组；明细落独立
  // 存档，主会话不收，SSE 实时推送 + 右栏打开时 /api/agent-log 拉历史合并）。
  subView: 0, subLive: {}, subDone: {}, subDetail: {},
  // 后台 turn 徽标（多会话并行）：bgRun = 会话 id → 该会话是否有 turn 在跑。
  // 实时源 = turn_state 帧与 /api/sessions 的 working 字段；与当前会话相关的
  // inFlight 不在此（上方专用字段）
  bgRun: {},
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

/** 非响应式转态容器（记录游标/汇总卡聚合/速率累计）：SSE 与重放两路径共用的
 *  可变私有面。放属性容器而非裸导出 let——ESM 导入绑定只读，属性才可写。 */
export const X = {
  turnEdits: new Map(),   // 当前轮编辑聚合（apply/applied 累计，flushTurnEdits 结算）
  turnEditsTurn: 0,
  lastTurnId: null,       // 轮次边界判定（turn_id 变化 ⇒ 结算汇总卡）
  runs: new Map(),        // run_id → 输出块 {lines, exit, failed}（输出折叠进调用行，见 records）
  runCall: new Map(),     // run_id → 归属的 runner.run 调用行（「已启动」通告时配对）
  // 输出速率累计（会话均值口径，records 增量采样维护）：rateToks=粗估输出
  // tokens、rateMs=输出时长、rateLast=最近增量时刻（0=无进行中的输出期）。
  // 放容器不放 records 模块局部——清零面在 sessions.js 的 resetView，双向 import 成环
  rateToks: 0, rateMs: 0, rateLast: 0,
}
