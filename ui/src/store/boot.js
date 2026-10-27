// 启动编排：health 轮询 → 壳模式探测 → 配置对齐 → SSE 连接 → 重放 → 会话装载；
// 兜底进草稿态（hero 引导）与壳消息通道注册。本文件是各领域模块的组装点
// （唯一的"知道所有人"的模块），不承载领域逻辑。
import { S } from './state'
import { api, cfgFromServer, shellMsg } from './api'
import { pollOnce, render } from './records'
import { loadSessions, sessionMeta, startShellPoll, enterDraft } from './sessions'

// SSE 断线补洞：EventSource 自动重连，但重连间隙的事件是空洞——conn=off 期间
// 用 pollOnce（after 游标）轮询补齐，恢复 on 即停。无此兜底，宿主重启/网络抖动
// 后页面永久失去实时事件，用户只能刷新整页。
let holeTimer = null
function setHolePoll(on) {
  if (on && holeTimer === null) {
    holeTimer = setInterval(async () => {
      try { await pollOnce() } catch (e) { /* 宿主未回：下一轮再试 */ }
    }, 2000)
  } else if (!on && holeTimer !== null) {
    clearInterval(holeTimer); holeTimer = null
  }
}

function connect() {
  const es = new EventSource('/events')
  es.onopen = () => {
    S.conn = 'on'
    setHolePoll(false)
    pollOnce().catch(() => {})   // 重连首件事：补上断口期间漏掉的记录
  }
  es.onerror = () => { S.conn = 'off'; setHolePoll(true) }
  es.onmessage = (e) => { render(JSON.parse(e.data)) }
}

/** 启动：轮询 health → 拉配置 → 连 SSE → 重放 → loadSessions；无会话则进草稿态。 */
export async function boot() {
  for (let i = 0; i < 60; i++) {   // 页面先于宿主就绪：轮询 health
    try { const h = await api('/api/health'); if (h.ok) break } catch (e) {}
    await new Promise(r => setTimeout(r, 500))
  }
  // 壳模式探测（多实例）：是则整页由壳托管切换；浏览器/npm run dev 单实例模式不受影响
  try {
    const sh = await api('/api/shell/sessions')
    if (sh.shell) {
      S.shell = true
      S.sessions = sh.sessions || []
      startShellPoll()
    }
  } catch (e) {}
  window.chrome?.webview?.addEventListener('message', (e) => {
    const m = e.data || {}
    if (m.type === 'navigate' && m.url) window.location.href = m.url
    if (m.type === 'ping-shell') shellMsg({ type: 'pong-page' })   // 通道双向自证
  })
  cfgFromServer(await api('/api/config'))
  connect()
  const sess = await api('/api/session')
  sessionMeta(sess)
  await pollOnce()          // 重放期在此：期间到达的提案是历史，不进决策坞
  S.booted = true
  await loadSessions()
  // 无会话态（首装/全删/重启无残留）：进草稿态兜底 UI（hero 引导）；
  // 壳新建流程经 ?ws= 预填草稿工作区（选目录发生在旧页面，跨源只带得动 URL）
  const urlWs = new URLSearchParams(location.search).get('ws') || ''
  if (urlWs) enterDraft(urlWs)
  else if (!S.sessions.length || !S.curSession) enterDraft()
  if (S.shell) {
    try {
      shellMsg({ type: 'ping', key: S.curSession })   // 消息通道自证（壳端日志）
    } catch (e) { console.error('shellMsg failed', e) }
  }
}
