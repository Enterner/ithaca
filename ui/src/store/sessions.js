// 会话域：草稿态进出场、注册表装载/切换/新建/删除/重命名 + 决策坞与后端
// pending 的对账 + 壳模式的注册表轮询。会话切换的视图清空（resetView）也在此
// ——它对齐 session 广播的清空面；records.render 消费 session 事件后调本模块。
import { S, X } from './state'
import { api, shellMsg } from './api'
import { pollOnce } from './records'   // 函数级循环引用（records ← sessions）：双方
                                       // 仅运行期互调、初始化无顶层互调，ESM 安全
import { confirmDialog } from '../confirm'

// ---- 视图重置与草稿态 --------------------------------------------------------

/** 会话切换 / 进草稿共用的视图重置（对齐 session 广播的清空面）。 */
export function resetView() {
  S.records = []; S.live = null; S.pending = null
  S.since = 0; S.lastId = -1; S.inFlight = false
  // 上下文环/速率/挂起条是「当前会话」的派生显示，不重置就会把旧会话的值带进
  // 新会话（踩过: 新建会话后上下文环仍显示上个会话的几 k 用量）
  S.lastPrompt = 0; S.rate = ''
  S.waitingSince = 0; S.waitingLeft = 0
  S.attachments = []   // 附件相对路径随会话（工作区）走，跨会话残留必然错配
  X.turnEdits = new Map(); X.turnEditsTurn = 0; X.lastTurnId = null
  X.runs = new Map(); X.runCall = new Map()   // run 归属随会话走，跨会话残留会错配输出
  X.rateToks = 0; X.rateMs = 0; X.rateLast = 0   // 速率累计随会话走（增量不落盘，换会话即重新统计）
}
/**
 * 进草稿态：＋新建后的空态引导。零请求——注册表/磁盘在首条消息前不动。
 * @param {string} [ws] 预选工作区（侧栏组「＋」带，主「＋新建」不带）
 */
export function enterDraft(ws) {
  S.draft = { workspace: ws || '' }
  resetView()
  S.curSession = ''; S.curWs = ws || ''
}

// ---- 注册表与元数据 ----------------------------------------------------------

function syncPendingWithServer(serverPending) {
  // 决策坞以后端事实为准（pending 是运行时状态，重启即 false）：
  //   后端无挂起 ⇒ 任何来源的幻影提案收起（卡片自然标「已失效」）；
  //   后端有挂起而前端没有（如页面断线期间到的提案）⇒ 恢复最后一张未裁决卡
  if (!serverPending) {
    if (S.pending) S.pending = null
    return
  }
  if (S.pending) return
  for (let i = S.records.length - 1; i >= 0; i--) {
    const r = S.records[i]
    if (r.type === 'proposal' && r._decided === null) { S.pending = r; break }
    if (r.type === 'user') break   // 只回溯到当前轮开始
  }
}

let shellPollTimer = null
/** 壳模式注册表轮询（幂等：重复调用不叠加 interval）。 */
export function startShellPoll() {
  if (shellPollTimer === null) shellPollTimer = setInterval(loadSessions, 3000)
}

/** 拉注册表 + 当前会话元数据，对齐侧栏/顶栏（各操作后的硬化刷新）。 */
export async function loadSessions() {
  if (S.shell) {
    const r = await api('/api/shell/sessions')
    if (r.shell) S.sessions = r.sessions || []
  } else {
    S.sessions = (await api('/api/sessions')) || []
  }
  // 元数据对齐两分支同链（meta → 顶栏/统计/决策坞门卫）
  const meta = await api('/api/session')
  S.curSession = meta.id
  S.curWs = meta.workspace || ''
  syncPendingWithServer(!!meta.pending)
}

/**
 * 停止运行中的 turn 链：跨工作区切换确认后的「停止并切换」路径。与
 * actions.decide(4) 的 UI 侧效果一致（解锁/收坞/收流式块），但不经 actions 层
 * ——依赖方向 sessions ← actions，会话域不得反向引用（actions 的 send/openSession
 * 都要先过这里）。
 */
async function cancelInflightTurn_() {
  const r = await api('/api/decision', { decision: 4 })
  if (r.ok) {
    S.pending = null   // 挂起的提案随取消作废（卡片保留展示）
    S.inFlight = false
    S.live = null
  }
  return r
}

/** 切到指定会话；先丢弃草稿（零请求零落盘）。
 *  切走不杀 turn：链运行中照常切（事件按归属会话路由，侧栏徽标跟进）；跨
 *  工作区目标会与运行中 turn 的文件面错配（workspace 插件单实例 jail）——先
 *  确认停止再切（服务端同规则兜底拒绝）。 */
export async function openSession(id) {
  S.draft = null
  if (id === S.curSession) return
  if (S.shell) {
    // 多实例：切换 = 整页导航，旧实例的后端照常跑自己的 turn——无需取消
    const row = S.sessions.find(x => x.id === id)
    if (!row) return
    if (row.alive && row.port) {   // 兄弟实例活着：整页导航过去（同构 UI，自动挂它自己的会话）
      window.location.href = 'http://127.0.0.1:' + row.port + '/'
    } else {                       // 停止态：请壳拉起，就绪后壳回发 navigate
      shellMsg({ type: 'activate', key: row.key })
    }
    return
  }
  const chainRunning = S.inFlight || Object.values(S.bgRun).some(Boolean)
  const tgt = S.sessions.find(x => x.id === id)
  if (chainRunning && tgt && tgt.workspace && tgt.workspace !== S.curWs) {
    const ok = await confirmDialog({
      title: '切换会话',
      content: '当前有 turn 正在后台运行，且目标会话的工作区不同——需要先停止它才能切换。',
      positive: '停止并切换',
    })
    if (!ok) return
    await cancelInflightTurn_()
  }
  // 乐观即时切换：清屏 + 侧栏高亮先走（点击零延迟反馈，不等 open 响应与 SSE
  // 回环），服务端切换与重放异步跟进。switchPending 期间补洞停拍——转录端点
  // 在服务端确认前还在服务旧活跃会话（踩过: 草稿态同款机理）。失败回滚 =
  // loadSessions 把 curSession 纠回服务端现状 + 补洞重放原会话。
  S.switchPending = true
  resetView()
  S.curSession = id
  try {
    await api('/api/session/open', { id })
  } catch (e) { /* 网络失败：下方回滚对齐服务端现状 */ }
  S.switchPending = false
  loadSessions()   // 侧栏高亮/元数据对齐（成功 = 目标，失败 = 回滚服务端现状）
  pollOnce()       // 重放当前服务端活跃会话（成功 = 目标，失败 = 原会话）
}

/** ＋新建 = 进草稿态（enterDraft 的公开名——组件侧语义入口）：零请求，注册表/
 *  磁盘在首条消息前不动。壳模式同构（页内 hero 选目录，选完由壳拉起新实例并
 *  整页切换——见 folder_picked）。多会话并行：运行中的 turn 不受新建影响；
 *  跨工作区并发由服务端拒绝并在发送处报错。 */
export const newSession = enterDraft

/** 删除会话（后端连带 transcript/state；loadSessions 对齐侧栏）。 */
export async function delSession(id) {
  if (S.shell) {
    const row = S.sessions.find(x => x.id === id)
    if (row) shellMsg({ type: 'delete', key: row.key })
    setTimeout(loadSessions, 1500)   // 壳删完回来自动对齐
    return
  }
  await api('/api/session/delete', { id })
  await loadSessions()
  // 删的是当前会话且后端进入无会话态：后端全空分支不广播 session 记录，
  // 视图与游标必须本地重置——残留旧记录 + 旧 lastId 会把后续新会话从 0 发号
  // 的记录全部挡掉（后端 records_ 已清零重排）。非空切换由 SSE 的 session
  // 记录走标准重放路径，无需本地干预
  if (!S.curSession) resetView()
}

/** 重命名会话（注册表 title 字段）。 */
export async function renameSession(id, title) {
  await api('/api/session/rename', { id, title })
  loadSessions()
}
