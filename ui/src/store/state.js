// 全局响应式状态（唯一出处）：跨领域共享的 S 只在此定义，领域行为分见同目录
// 各模块（api/records/sessions/actions/boot）——本文件不含任何逻辑。
import { reactive } from 'vue'

export const S = reactive({
  records: [],          // 已定格记录（重放 + SSE 收口）
  live: null,           // 流式块 {reasoning, text}（SSE 专属，不落盘）
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
  fTurns: 0, fTokens: 0,
  fsRev: 0,   // 文件树刷新时钟：applied/undone 时 +1，FileTree watch 它静默重拉已展开目录
  // 代码查看器（右栏文件态 ⇄ 查看态）：{path, content, truncated, loading, error}；
  // null = 右栏显示文件树。纯展示态——内容由 App.vue 经 /api/fs/read 拉取，不经 actions 层
  viewer: null,
  // 待裁决提案（ZCode 式决策坞：只在实时提案上出现，重放的历史提案不阻塞输入）
  pending: null,
  inputText: '',
  booted: false,
  shell: false,   // 多实例壳模式：会话清单来自 /api/shell/sessions，切换 = 整页导航
  // 插件 UI 贡献：registry = 各插件 schema（/api/ui/registry，设置面板
  // 打开时拉取）；pluginVals = 各插件设置值（/api/config 的 plugins 节）。
  pluginUi: [], pluginVals: {},
})

// 非响应式转态（记录游标/汇总卡聚合/速率窗口）：SSE 与重放两路径共用的
// 可变私有面。放属性容器而非裸导出 let——ESM 导入绑定只读，属性才可写。
export const X = {
  turnEdits: new Map(),   // 当前轮编辑聚合（apply/applied 累计，flushTurnEdits 结算）
  turnEditsTurn: 0,
  lastTurnId: null,       // 轮次边界判定（turn_id 变化 ⇒ 结算汇总卡）
  turnStart: 0, turnChars: 0,   // 输出速率窗口（liveStart 起表）
}
