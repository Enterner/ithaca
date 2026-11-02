// 启动编排：health 轮询 → 壳模式探测 → 配置对齐 → SSE 连接 → 重放 → 会话装载；
// 兜底进草稿态（hero 引导）与壳消息通道注册。本文件是各领域模块的组装点
// （唯一的"知道所有人"的模块），不承载领域逻辑。
import { S } from './state'
import { api, cfgFromServer, shellMsg } from './api'
import { pollOnce, render } from './records'
import { loadSessions, startShellPoll, enterDraft } from './sessions'

// SSE 补洞：EventSource 的重连间隙有空洞——但「连接在、帧丢失」的静默形态
//（复现：双连接下单条被饿死，conn 恒 on、无 error 事件）靠 onerror 触发的
// 补洞永远不启动，页面从此丢实时流只能刷新。因此 pollOnce 恒跑（on 时 5s
// 兜底、off 时 2s 抢救），漏帧由 after 游标补齐，串行化与停靠见 records。
let holeTimer = null
function setHolePoll(on) {
  const period = on ? 5000 : 2000
  if (holeTimer === null) {
    holeTimer = setInterval(async () => {
      try { await pollOnce() } catch (e) { /* 宿主未回：下一轮再试 */ }
    }, period)
  } else if (holeTimer._period !== period) {
    clearInterval(holeTimer)
    holeTimer = setInterval(async () => {
      try { await pollOnce() } catch (e) { /* 宿主未回：下一轮再试 */ }
    }, period)
  }
  holeTimer._period = period
}

function connect() {
  const es = new EventSource('/events')
  es.onopen = () => {
    S.conn = 'on'
    setHolePoll(false)
    pollOnce().catch(() => {})   // 重连首件事：补上断口期间漏掉的记录
  }
  es.onerror = () => { S.conn = 'off'; setHolePoll(true) }
  es.onmessage = (e) => {
    // 非 JSON 帧就地丢弃（未来心跳帧/异常帧不得中断状态机——handler 内抛出
    // 会终止本次派发且无任何日志路径）
    try { render(JSON.parse(e.data)) } catch (err) { console.warn('ithaca.web: sse 非法帧丢弃', err) }
  }
}

/** 启动：轮询 health → 拉配置 → 连 SSE → 重放 → loadSessions；无会话则进草稿态。
 *  失败行为：health 60 次×500ms 耗尽仍继续；其后任一 api 失败令 boot 整体
 *  reject（App.vue onMounted 未捕获，页面停在 hero/空态）。 */
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
  await api('/api/session')   // 对账 in_flight（重放后僵尸轮检测的数据源，records 消费）
  await pollOnce()          // 重放期在此：期间到达的提案是历史，不进决策坞
  S.booted = true
  await loadSessions()
  // 无会话态（首装/全删/重启无残留）：进草稿态兜底 UI（hero 引导）；
  // 壳新建流程经 ?ws= 预填草稿工作区（选目录发生在旧页面，跨源只带得动 URL）
  const urlWs = new URLSearchParams(location.search).get('ws') || ''
  if (urlWs) enterDraft(urlWs)
  else if (!S.sessions.length || !S.curSession) enterDraft()
  if (S.shell) shellMsg({ type: 'ping', key: S.curSession })   // 消息通道自证（shellMsg 内部自吞错）
}
