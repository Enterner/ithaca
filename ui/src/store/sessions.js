// 会话域：草稿态进出场、注册表装载/切换/新建/删除/重命名 + 决策坞与后端
// pending 的对账 + 壳模式的注册表轮询。会话切换的视图清空（resetView）也在此
// ——它对齐 session 广播的清空面；records.render 消费 session 事件后调本模块。
import { S, X } from './state'
import { api, shellMsg } from './api'

// ---- 视图重置与草稿态 --------------------------------------------------------

/** 会话切换 / 进草稿共用的视图重置（对齐 session 广播的清空面）。 */
export function resetView() {
  S.records = []; S.live = null; S.pending = null
  S.since = 0; S.lastId = -1; S.inFlight = false
  // 上下文环/速率/挂起条是「当前会话」的派生显示，不重置就会把旧会话的值带进
  // 新会话（踩过：新建会话后上下文环仍显示上个会话的几 k 用量）
  S.lastPrompt = 0; S.rate = '0'
  S.waitingSince = 0; S.waitingLeft = 0
  X.turnEdits = new Map(); X.turnEditsTurn = 0; X.lastTurnId = null
  X.runs = new Map(); X.runCall = new Map()   // run 归属随会话走，跨会话残留会错配输出
}
/**
 * 进草稿态：＋新建后的空态引导。零请求——注册表/磁盘在首条消息前不动。
 * @param {string} [ws] 预选工作区（侧栏组「＋」带，主「＋新建」不带）
 */
export function enterDraft(ws) {
  S.draft = { workspace: ws || '' }
  resetView()
  S.curSession = ''; S.curWs = ws || ''; S.sessTitle = ''
}

// ---- 注册表与元数据 ----------------------------------------------------------

/** 会话元数据（/api/session 响应）对齐侧栏/顶栏。 */
export function sessionMeta(m) {
  S.sessTitle = m.title
}

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
  sessionMeta(meta)
  syncPendingWithServer(!!meta.pending)
}

/**
 * 生成中切走的前置收口：取消当前 turn。与 actions.decide(4) 的 UI 侧效果一致
 * （解锁/收坞/收流式块），但不经 actions 层——依赖方向 sessions ← actions，
 * 会话域不得反向引用（actions 的 send/openSession 都要先过这里）。
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

/** 切到指定会话；先丢弃草稿（零请求零落盘）。 */
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
  // 单实例：后端是单活跃会话（agent 一次只跑一个 turn，session/load 在 turn
  // 进行中会被拒）——切走 = 先取消当前轮次（语义同停止键），再开目标会话。
  // 两请求都经宿主线程串行：decision 先清 turn，session/load 后到必是 Idle。
  if (S.inFlight) await cancelInflightTurn_()
  await api('/api/session/open', { id })
  loadSessions()   // 硬化：不只依赖 SSE
}

/** ＋新建 = 进草稿态：零请求，注册表/磁盘在首条消息前不动。
 *  壳模式同构（页内 hero 选目录，选完由壳拉起新实例并整页切换——见 folder_picked）。
 *  单实例生成中新建同样先取消当前轮次——首条消息要经 /api/session/new，
 *  in_flight 未清会被后端拒。 */
export async function newSession(ws) {
  if (!S.shell && S.inFlight) await cancelInflightTurn_()
  enterDraft(ws)
}

/** 删除会话（后端连带 transcript/state；loadSessions 对齐侧栏）。 */
export async function delSession(id) {
  if (S.shell) {
    const row = S.sessions.find(x => x.id === id)
    if (row) shellMsg({ type: 'delete', key: row.key })
    setTimeout(loadSessions, 1500)   // 壳删完回来自动对齐
    return
  }
  await api('/api/session/delete', { id })
  loadSessions()
}

/** 重命名会话（注册表 title 字段）。 */
export async function renameSession(id, title) {
  await api('/api/session/rename', { id, title })
  loadSessions()
}
