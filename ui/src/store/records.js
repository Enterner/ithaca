// 记录状态机：SSE/重放的统一入口 render——流式合并、汇总卡聚合（同批同文件多次编辑折叠为一条）、去重只认数字 id（NOTE: session 字符串 id 会污染 lastId——踩过）；
// 附 HTTP 重放 pollOnce（重放与补洞共用）与输出速率窗口。
// 视图重置/草稿态进出场属会话域（./sessions），session 事件在此分发到它。
import { S, X } from './state'
import { api, shellMsg } from './api'
import { mdHtml } from '../md'
import { diffFiles, diffStats, diffIsNew } from '../diff'
import { loadSessions, enterDraft, resetView } from './sessions'

function push(rec) { S.records.push(rec) }
/** 本地伪记录：只进视图、不经宿主（发送链的前端提示，如创建失败回退）。 */
export function pushLocal(text) { push({ type: 'local', text }) }

// ---- 汇总卡聚合（edit-diff 批次语义）-----------------------------------------

// applied 的 diff_text = 每文件一段「── path（N 处编辑）\n<diff>」；按段头切开，
// 段序与 paths/files 一致，段体 = 该文件的 hunk diff（不含段头）。
function splitAppliedDiff(diffText) {
  const segs = []
  let cur = null
  for (const line of diffText.split('\n')) {
    if (line.startsWith('── ')) { if (cur) segs.push(cur); cur = [] }
    else if (cur) cur.push(line)
  }
  if (cur) segs.push(cur)
  return segs.map((g) => g.join('\n'))
}
function accumulateApplied(rec) {
  // 统计与 is_new 以事件的结构化字段为准（EditApplied.files_json）——diff_text 是
  // 展示文本且可能被 8KB 护栏截断（截断砍掉的是文本尾行统计，从它反解会让大
  // diff 的修改卡恒 +0−0，踩过）；旧记录（历史 transcript 重放，无 files 字段）
  // 回退展示文本反解。
  const structured = Array.isArray(rec.files) && rec.files.length ? rec.files : null
  const bodies = splitAppliedDiff(rec.diff || '')
  if (structured) {
    structured.forEach((f, i) => {
      const e = X.turnEdits.get(f.path) || { applies: [], n: 0, add: 0, rem: 0, isNew: false }
      e.applies.push(bodies[i] ?? '')
      e.n++
      e.add += f.add || 0
      e.rem += f.rem || 0
      e.isNew = e.isNew || !!f.is_new
      X.turnEdits.set(f.path, e)
    })
  } else {
    const st = diffStats(rec.diff) || { add: 0, rem: 0 }
    const m = (rec.diff || '').match(/^── (.+)$/m)
    const paths = rec.paths || []
    const p = paths.length === 1 ? paths[0] : (m ? m[1].replace(/（[^）]*）\s*$/, '').trim() : '?')
    const isNew = diffIsNew(rec.diff || '')
    const e = X.turnEdits.get(p) || { applies: [], n: 0, add: 0, rem: 0, isNew: false }
    e.applies.push(bodies[0] ?? (rec.diff || ''))
    e.n++
    e.add += st.add
    e.rem += st.rem
    e.isNew = e.isNew || isNew
    X.turnEdits.set(p, e)
  }
  if (rec.turn_id) X.turnEditsTurn = rec.turn_id
}
/** 结算当前轮的编辑汇总卡（记录流内伪记录，落位到当前位置）。 */
export function flushTurnEdits() {
  if (X.turnEdits.size === 0) return
  const files = [...X.turnEdits.entries()].map(([path, e]) =>
    ({ path, applies: e.applies, n: e.n, add: e.add, rem: e.rem, isNew: e.isNew, open: false }))
  // 汇总卡 = 记录流内的伪记录：结算时落位到当前位置（跟随自己那轮）
  S.records.push({ type: 'turn_edits', turn: X.turnEditsTurn, files, revoked: false })
  X.turnEdits = new Map(); X.turnEditsTurn = 0
}

// ---- 记录状态机 --------------------------------------------------------------

/** SSE/重放的统一入口：所有宿主记录（含转态信号）都经此进状态机。 */
export function render(rec) {
  // NOTE: 轮次边界结算在 turn_id 变化时触发——无 assistant 收口的轮次（拒绝/
  // 报错轮）的编辑汇总也能随自己那轮落位，不堆到下一轮（踩过：曾全堆在底部）
  { const tid = rec.turn_id
    if (typeof tid === 'number') {
      if (X.lastTurnId !== null && tid !== X.lastTurnId) flushTurnEdits()
      X.lastTurnId = tid
    } }
  if (rec.id !== undefined && typeof rec.id === 'number') {
    if (rec.id <= S.lastId) return
    S.lastId = rec.id
  }
  if (rec.type === 'error') {
    S.inFlight = false; S.rate = '0'
  }
  // 挂起等待的收口：turn 收口/失败/取消、新输入（新 turn）都会终结挂起期
  if (rec.type === 'error' || rec.type === 'user' ||
      (rec.type === 'assistant' && rec.turn_done)) {
    S.waitingSince = 0; S.waitingLeft = 0
  }
  switch (rec.type) {
    case 'session':   // 会话已切换：清空视图并重放（草稿若有则已随创建提交）
      S.draft = null
      S.subView = 0; S.subLive = {}; S.subDone = {}; S.subDetail = {}
      resetView()
      if (rec.workspace) S.curWs = rec.workspace
      loadSessions(); pollOnce()
      return
    case 'folder_picked':
      // 草稿态：选工作区只记在本地（目录创建延迟到发送）；非草稿 = 在选定目录
      // 开新会话（无工作区会话选目录也走此路径）
      if (S.draft) {
        if (S.shell) {
          // 多实例：会话 = 新实例。选完目录即请壳拉起（工作区注入新实例 env），
          // 就绪后壳回发 navigate（url 带 ?ws= 预填草稿）——首条消息在新页面发送
          const ws = rec.path
          S.draft = null
          S.curWs = ws
          shellMsg({ type: 'new-session', ws })
          return
        }
        S.draft.workspace = rec.path
        S.curWs = rec.path
      } else enterDraft(rec.path)
      return
    case 'user':
      push(rec); S.live = null
      S.inFlight = true
      return
    case 'assistant_delta':
      if (!S.live) S.live = { reasoning: '', text: '', open: true }
      S.live.text += rec.text
      X.turnChars += rec.text.length
      S.rate = String(Math.round(X.turnChars / Math.max(1, (Date.now() - X.turnStart) / 1000)))
      return
    case 'assistant_reasoning':
      if (!S.live) S.live = { reasoning: '', text: '', open: true }
      S.live.reasoning += rec.text
      return
    case 'assistant':
      S.live = null                    // 思考结束：折叠流式块
      push(rec)
      // inFlight = turn 进行中（带 tool_calls 的 done 只是一轮收束，工具/子代理
      // 阶段仍是同一 turn——停止入口必须全程在场，否则子代理挂起期无路可退）
      S.inFlight = rec.turn_done === false
      S.rate = '0'
      if (rec.usage && rec.usage.total_tokens) {
        S.lastPrompt = rec.usage.prompt_tokens || 0
        S.fTokens += rec.usage.total_tokens
        S.fTurns += 1
      }
      { const done = rec.turn_done !== undefined ? rec.turn_done : !!rec.text
        if (done) flushTurnEdits() }
      return
    case 'subagent':
      // 子代理状态卡（spawn/完成/失败）：落 transcript，主会话只显示状态。
      // 收口记录（phase>=4，值 4=完成 5=取消/失败）同时核销挂起条——task 的
      // tool 结果卡已去重，不再有 tool 记录承担核销
      push(rec)
      if (rec.phase >= 4) {
        S.subDone[rec.turn_id] = rec.phase
        if (S.waitingLeft > 0) {
          S.waitingLeft--
          if (S.waitingLeft === 0) S.waitingSince = 0
        }
      }
      return
    case 'subagent_detail':
      // 子代理工具明细（实时推送；持久化在服务端独立存档，不经主会话记录流）：
      // 按 turn_id 缓冲给右栏过程视图，与打开时拉取的历史按 id 去重合并
      {
        const arr = S.subDetail[rec.turn_id] || []
        arr.push(rec)
        S.subDetail[rec.turn_id] = arr
      }
      return
    case 'subagent_delta':
      // 子代理流式增量（只广播不落盘）：按 turn_id 缓冲给右栏过程视图
      S.subLive[rec.turn_id] = (S.subLive[rec.turn_id] || '') + rec.text
      return
    case 'tool_call': push(rec); return
    case 'tool':
      // apply 的成功结果与 ✔ applied 行同信息（同文件同增删），不重复渲染；
      // 失败结果（ok=false）保留——那是唯一的错误反馈（踩过：双行同文）
      if (rec.ok && /apply$/.test(rec.tool || '')) return
      // 挂起核销：等待期内的每条工具结果 = 一个挂起的子代理/后台任务落定
      // （顺序纪律：等待通告只由 dispatch_loop 尾部发出，其后的 tool 结果
      // 必然属于挂起项）
      if (S.waitingLeft > 0) { S.waitingLeft--; if (S.waitingLeft === 0) S.waitingSince = 0 }
      push(rec); return
    case 'proposal':
      // 卡片视图状态（逐文件勾选/裁决标记）挂在记录对象上；实时提案（booted 之后
      // 经 SSE 到达）⇒ 进入决策坞；重放的历史提案只展示，不阻塞输入
      rec._files = diffFiles(rec.diff)
      rec._checked = {}
      rec._files.forEach(p => { rec._checked[p] = true })
      rec._decided = null
      push(rec)
      if (S.booted) S.pending = rec
      return
    case 'applied':
      if (rec.diff) accumulateApplied(rec)
      push(rec)
      S.fsRev++   // 落盘完成 ⇒ 文件树静默刷新
      return
    case 'undone': {
      push(rec)
      S.fsRev++   // 快照回滚 ⇒ 文件树同步回退视图
      for (let i = S.records.length - 1; i >= 0; i--) {
        const r = S.records[i]
        if (r.type === 'turn_edits' && r.turn === rec.turn_id) { r.revoked = true; break }
      }
      return
    }
    case 'run_line': if ((rec.stream || 0) === 1) push(rec); return
    case 'fs_changed':
      // 磁盘真变化（webui watcher 的 SSE 瞬态信号）⇒ 文件树自动跟上；
      // 不进 records——它不是对话内容，落盘重放反而要在会话加载时压掉
      S.fsRev++
      return
    case 'run_exit': push(rec); return
    case 'notice':
      push(rec)
      // 挂起等待信号：agent 在派发尾部仍有未决子代理/后台任务时发「等待 N 个…」
      // （run 单独等待与 task/混合等待同款式）。取消级联的「已取消 N 个在途子代理」
      // 即挂起终结信号。转态记号，文本仍照常渲染。
      { const m = /^等待 (\d+) 个/.exec(rec.text || '')
        if (m) { S.waitingSince = Date.now(); S.waitingLeft = Number(m[1]) }
        else if (/^已取消 \d+ 个在途子代理/.test(rec.text || '')) {
          S.waitingSince = 0; S.waitingLeft = 0
        } }
      return
    case 'error':
      push(rec); flushTurnEdits(); return
  }
}

// ---- 数据通道 ----------------------------------------------------------------

/** 重放/补洞：HTTP 拉取 after 起的记录并逐条渲染（SSE 只管实时）。 */
export async function pollOnce() {
  const t = await api('/api/transcript?after=' + S.since)
  t.records.forEach(render)
  S.since = t.next
  flushTurnEdits()   // 重放结束 ⇒ 最后一轮的编辑汇总也落位（否则丢了）
  // 重放后以后端原子真值对账：user 记录在重放里无条件置 inFlight=true，失败/
  // 取消轮没有 assistant/error 收口时会把「进行中」复活（踩过: zombie——切回
  // 失败会话按钮恒为停止态），以后端 in_flight 为准兜掉一切记录流缺口
  const m = await api('/api/session')
  S.inFlight = !!m.in_flight
}

/** 发送即开始计时（输出速率窗口）。 */
export function liveStart() {
  X.turnStart = Date.now(); X.turnChars = 0
}
