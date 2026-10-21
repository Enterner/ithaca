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
  X.turnEdits = new Map(); X.turnEditsTurn = 0; X.lastTurnId = null
}
/**
 * 进草稿态：＋新建后的空态引导。零请求——注册表/磁盘在首条消息前不动。
 * @param {string} [ws] 预选工作区（侧栏组「＋」带，主「＋新建」不带）
 */
export function enterDraft(ws) {
  S.draft = { workspace: ws || '' }
  resetView()
  S.curSession = ''; S.curWs = ws || ''; S.sessTitle = ''; S.sessRecords = 0
}
/** 丢弃草稿：内存态消失即结束，无任何清理动作。 */
export function discardDraft() { S.draft = null }

// ---- 注册表与元数据 ----------------------------------------------------------

/** 会话元数据（/api/session 响应）对齐侧栏/顶栏/统计。 */
export function sessionMeta(m) {
  S.sessTitle = m.title; S.sessRecords = m.records
  S.fTurns = m.turns; S.fTokens = m.tokens
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

/** 拉注册表 + 当前会话元数据，对齐侧栏/顶栏/统计（各操作后的硬化刷新）。 */
export async function loadSessions() {
  if (S.shell) {
    const r = await api('/api/shell/sessions')
    if (r.shell) S.sessions = r.sessions || []
    const meta = await api('/api/session')
    S.curSession = meta.id
    S.curWs = meta.workspace || ''
    sessionMeta(meta)
    syncPendingWithServer(!!meta.pending)
    return
  }
  const list = await api('/api/sessions')
  const meta = await api('/api/session')
  S.sessions = list || []
  S.curSession = meta.id
  S.curWs = meta.workspace || ''
  sessionMeta(meta)
  syncPendingWithServer(!!meta.pending)
}

/** 切到指定会话；先丢弃草稿（零请求零落盘）。 */
export async function openSession(id) {
  if (S.inFlight) return
  S.draft = null
  if (id === S.curSession) return
  if (S.shell) {
    const row = S.sessions.find(x => x.id === id)
    if (!row) return
    if (row.alive && row.port) {   // 兄弟实例活着：整页导航过去（同构 UI，自动挂它自己的会话）
      window.location.href = 'http://127.0.0.1:' + row.port + '/'
    } else {                       // 停止态：请壳拉起，就绪后壳回发 navigate
      shellMsg({ type: 'activate', key: row.key })
    }
    return
  }
  await api('/api/session/open', { id })
  loadSessions()   // 硬化：不只依赖 SSE
}

/** ＋新建 = 进草稿态：零请求，注册表/磁盘在首条消息前不动。
 *  壳模式同构（页内 hero 选目录，选完由壳拉起新实例并整页切换——见 folder_picked）。 */
export function newSession(ws) {
  if (S.inFlight) return
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
