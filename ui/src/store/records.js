// 记录状态机：SSE/重放的统一入口 render——流式合并、汇总卡聚合（同批同文件多次编辑折叠为一条）、去重只认数字 id（NOTE: session 字符串 id 会污染 lastId——踩过）；
// 附 HTTP 重放 pollOnce（重放与补洞共用）与会话输出速率累计。
// 视图重置/草稿态进出场属会话域（./sessions），session 事件在此分发到它。
import { S, X } from './state'
import { api, shellMsg } from './api'
import { mdHtml } from '../md'
import { diffFiles, diffStats, diffIsNew, parseAppliedDiff } from '../diff'
import { loadSessions, enterDraft, resetView } from './sessions'

// 伪记录（无宿主 id 的本地/汇总/输出块记录）的稳定键：对话流按尾部窗口渲染，
// 窗口起点随扩窗移动会让按下标派生的键漂移触发整段重挂——入库时发一次性序号
let recSeq = 0
function push(rec) {
  if (rec.id === undefined) rec._k = ++recSeq
  S.records.push(rec)
}
/** 本地伪记录：只进视图、不经宿主（发送链的前端提示，如创建失败回退）。 */
export function pushLocal(text) { push({ type: 'local', text }) }

// ---- 过程层收敛（对话流只平铺消息级内容，过程压成单行/可展开块）--------------
// 调用行与结果行合并为一行：结果按「同工具最近未决调用」归位（非 run 工具严格
// 顺序成对）。runner.run 例外——并发 run 的结果会乱序且结果记录不带 run_id，
// 转后台 ack（exit -1）直接吃掉，真实收口只认 run_exit 的 run_id 映射（归属由
// 「后台任务 run#N 已启动」通告建立：通告紧跟其调用行发出，见 agent dispatch）。

function lastOpenCall(tool) {
  for (let i = S.records.length - 1; i >= 0; i--) {
    const r = S.records[i]
    if (r.type === 'tool_call' && r.tool === tool && !r._done) return r
  }
  return null
}
/** turn 收口时强闭所有未决调用行：配对只认「最近未决」，悬挂的旧调用会截胡
    下一 turn 的同工具结果。 */
function closeOpenCalls() {
  for (const r of S.records)
    if (r.type === 'tool_call' && !r._done) r._done = true
}
/** 挂起通告只留一条动态行：等待数变化就地改写、核销即撤（过程状态不是历史，
    逐条落流只会连发刷屏 + 留下陈旧的「等待 1 个…」尸体）。 */
function upsertWaitline(text) {
  const cur = S.records.find((r) => r._wait)
  if (cur) cur.text = text
  else push({ type: 'notice', text, _wait: true })
}
function removeWaitline() {
  const i = S.records.findIndex((r) => r._wait)
  if (i >= 0) S.records.splice(i, 1)
}
/** run 输出块：逐行输出按 run_id 聚合（一行一记录曾是对话流的最大噪音源）。
    块挂归属调用行的展开区，映射缺失时自成一条可展开行。 */
function runBlock(runId) {
  let b = X.runs.get(runId)
  if (b) return b
  const raw = { type: 'run_out', run_id: runId, lines: [], exit: null, failed: false }
  const call = X.runCall.get(runId)
  if (call && !call._done) {
    call._runs.push(raw)
    b = call._runs[call._runs.length - 1]
  } else {
    push(raw)
    b = S.records[S.records.length - 1]
  }
  // NOTE: 必须存经 reactive 数组取出的代理而非裸对象——后续行追加走裸引用
  // 不触发重渲（模板跟踪的是代理上的依赖）
  X.runs.set(runId, b)
  return b
}

// ---- 汇总卡聚合（edit-diff 批次语义）-----------------------------------------

// applied 的 diff_text = 每文件一段「── path（N 处编辑）\n<diff>」，切段与
// 路径提取统一走 diff.js 的 parseAppliedDiff（单点）
function accumulateApplied(rec) {
  // 统计与 is_new 以事件的结构化字段为准（EditApplied.files_json）——diff_text 是
  // 展示文本且可能被 8KB 护栏截断（截断砍掉的是文本尾行统计，从它反解会让大
  // diff 的修改卡恒 +0−0，踩过）；旧记录（历史 transcript 重放，无 files 字段）
  // 回退展示文本反解。
  const structured = Array.isArray(rec.files) && rec.files.length ? rec.files : null
  const segs = parseAppliedDiff(rec.diff || '')
  if (structured) {
    structured.forEach((f, i) => {
      const e = X.turnEdits.get(f.path) || { applies: [], n: 0, add: 0, rem: 0, isNew: false }
      e.applies.push(segs[i]?.body ?? '')
      e.n++
      e.add += f.add || 0
      e.rem += f.rem || 0
      e.isNew = e.isNew || !!f.is_new
      X.turnEdits.set(f.path, e)
    })
  } else {
    const st = diffStats(rec.diff) || { add: 0, rem: 0 }
    const paths = rec.paths || []
    const p = paths.length === 1 ? paths[0] : (segs[0]?.path ?? '?')
    const isNew = diffIsNew(rec.diff || '')
    const e = X.turnEdits.get(p) || { applies: [], n: 0, add: 0, rem: 0, isNew: false }
    e.applies.push(segs[0]?.body ?? (rec.diff || ''))
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
  push({ type: 'turn_edits', turn: X.turnEditsTurn, files, revoked: false })
  X.turnEdits = new Map(); X.turnEditsTurn = 0
}

// ---- 记录状态机 --------------------------------------------------------------

// SSE 与 HTTP 重放双通道的竞态防线（结构修复，非补丁）：补洞期间 SSE 到达的
// 记录先停靠，快照渲染完再放行——直接渲染会被 max-id 去重吞掉（SSE 先推 id=8、
// 快照里的 6/7 随后被 `<= lastId` 丢弃且 since 已推进 ⇒ 对话流永久缺行，只有切
// 会话重放才能恢复）。补洞自身串行化：boot 重放与 onopen 补洞并发时按序执行，
// 两个快照不再互相顶掉。
let parkSse = false
const parked = []
let pollChain = Promise.resolve()
let rollbackSeq = 0   // 撤回转录面的代次：poll 拉取期发生撤回 ⇒ 本轮结果作废重拉

// 后台 turn 链会打 session 标的事件类型（切走不杀 turn——链运行期事件按归属
// 会话落盘广播）。非当前会话的带标事件不进当前视图，切回时 HTTP 重放补齐
const TURN_TAGGED = new Set(['user', 'assistant', 'assistant_delta', 'assistant_reasoning',
                             'tool_call', 'tool', 'run_line', 'run_exit', 'notice',
                             'proposal', 'applied', 'undone', 'subagent', 'subagent_delta',
                             'subagent_detail', 'error', 'turn_state'])

// 无绑定视图（草稿/创建在途）的早到帧停靠：'session' 收口按新会话 id 放行，
// 缺口由 pollOnce 全量重放兜底（见 tag 守卫处的机理注释）。上限防失控。
const draftPark = []

/** SSE/重放的统一入口：所有宿主记录（含转态信号）都经此进状态机。 */
export function render(rec) {
  // TEMP-DEBUG: 帧级轨迹（CONV10 flaky 查因，收口后拆除）
  if (window.__evlog && window.__evlog.length < 400) {
    let tag = ''
    if (rec.session && rec.session !== S.curSession && TURN_TAGGED.has(rec.type)) {
      tag = !S.curSession ? ' PARK' : ' DROP'
    } else if (rec.id !== undefined && rec.id <= S.lastId) tag = ' DUP'
    window.__evlog.push(`in ${rec.type} id=${rec.id} ses=${(rec.session || '-').slice(-6)}${tag}`)
  }
  // 撤回转录面（中断卡 ↩）：按位裁剪记录流、对齐游标、清被撤轮次的挂起面。
  // 位裁剪是自愈的——竞态窗口内渲染的迟到记录同样被裁掉
  if (rec.type === 'records_rollback') {
    const cut = rec.count
    let at = S.records.length
    for (let i = 0; i < S.records.length; i++) {
      const id = S.records[i].id
      if (id !== undefined && id >= cut) { at = i; break }
    }
    for (const r of S.records.splice(at))
      if (r.turn_id) {   // 被撤轮次的子代理缓冲与状态面一并清理
        delete S.subDetail[r.turn_id]; delete S.subLive[r.turn_id]; delete S.subDone[r.turn_id]
      }
    S.since = cut; S.lastId = cut - 1
    X.turnEdits = new Map(); X.turnEditsTurn = 0
    // 被撤轮次的增量已混入速率累计、按位无法剔除——均值整体重统计
    X.rateToks = 0; X.rateMs = 0; X.rateLast = 0; S.rate = ''
    S.fsRev++   // 文件树对齐磁盘真态（被撤轮次的文件改动仍在盘上）
    return
  }
  // turn_state 无论归属都消费：后台会话刷徽标行；当前会话对账 inFlight
  if (rec.type === 'turn_state') {
    S.bgRun[rec.session] = !!rec.working
    const row = S.sessions.find((x) => x.id === rec.session)
    if (row) { row.working = !!rec.working; row.pending = !!rec.pending }
    if (rec.session === S.curSession) S.inFlight = !!rec.working
    return
  }
  if (rec.session && rec.session !== S.curSession && TURN_TAGGED.has(rec.type)) {
    // 无绑定视图（草稿/创建请求在途，curSession 仍空）的早到帧停靠而非丢弃：
    // 会话创建即开 turn（create+start 同请求），首批记录（用户消息/首个工具卡）
    // 在本页 curSession 置位前到达——丢弃后广播重放的快照与落盘存在竞态
    //（fetch 早于落盘 ⇒ 永久缺卡，CONV10 flaky 实测：服务端转录完整、UI 永缺
    // 首个工具卡）。'session' 收口按 id 放行，放行缺口由 pollOnce 全量重放
    // 兜底（renderNow 按 id 去重）；上限防失控，非无绑定状态维持丢弃语义
    //（多会话并行时他帧本就该丢，切回由 pollOnce 补）。
    if (!S.curSession && draftPark.length < 500) draftPark.push(rec)
    return
  }
  if (parkSse) { parked.push(rec); return }
  renderNow(rec)
}

function renderNow(rec) {
  // NOTE: 轮次边界结算在 turn_id 变化时触发——无 assistant 收口的轮次（拒绝/
  // 报错轮）的编辑汇总也能随自己那轮落位，不堆到下一轮（踩过: 曾全堆在底部）
  { const tid = rec.turn_id
    if (typeof tid === 'number') {
      if (X.lastTurnId !== null && tid !== X.lastTurnId) flushTurnEdits()
      X.lastTurnId = tid
    } }
  if (rec.id !== undefined && typeof rec.id === 'number') {
    if (rec.id <= S.lastId) return
    S.lastId = rec.id
  }
  // 挂起等待的收口：turn 收口/失败/取消、新输入（新 turn）都会终结挂起期；
  // 未决调用行一并强闭（防截胡下一 turn 的结果配对）
  if (rec.type === 'error' || rec.type === 'user' ||
      (rec.type === 'assistant' && rec.turn_done)) {
    S.waitingLeft = 0
    closeOpenCalls(); removeWaitline()
  }
  switch (rec.type) {
    case 'session':   // 会话已切换：清空视图并重放（草稿若有则已随创建提交）
      // 本页发起的切换已本地收口（openSession 乐观清屏 + 重放）——同 id 广播
      // 只对齐注册表，不再整场 reset+重放（重复渲染闪屏）。id 缺省（旧形态）
      // 与他页发起的切换走全量路径。
      if (rec.id && rec.id === S.curSession) { loadSessions(); return }
      S.draft = null
      S.subView = 0; S.subLive = {}; S.subDone = {}; S.subDetail = {}
      resetView()
      if (rec.workspace) S.curWs = rec.workspace
      // 放行无绑定期停靠的早到帧（属于本会话的）——随后 pollOnce 全量重放，
      // renderNow 按 id 去重，停靠与重放重叠不双渲染
      for (const p of draftPark.splice(0))
        if (!p.session || p.session === rec.id) {
          if (window.__evlog && window.__evlog.length < 400)
            window.__evlog.push(`replay-park ${p.type} id=${p.id}`)
          renderNow(p)
        }
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
    case 'provider_models':
      // 设置面「从 API 获取模型」的回执（瞬态事件，不落 transcript/记录流）：
      // 转投 S 供 Settings.vue watch 消费——records 状态机不存非会话内容
      S.provModelsEvt = { n: (S.provModelsEvt ? S.provModelsEvt.n : 0) + 1,
                          ok: !!rec.ok, models: rec.models || [],
                          error: rec.error || '' }
      return
    case 'user':
      push(rec); S.live = null
      S.inFlight = true
      rateClose()   // 新轮开始：防御性收口（上一响应通常已随 assistant 收口）
      return
    case 'assistant_delta':
      // 默认折叠（open:false）：思考/正文流不推着页面走，要点开才展开
      //（思考体的跟随滚动由 Stream.vue 接管——内部滚，不动页面）
      if (!S.live) S.live = { reasoning: '', text: '', open: false }
      S.live.text += rec.text
      rateUpdate(rec.text)
      return
    case 'assistant_reasoning':
      if (!S.live) S.live = { reasoning: '', text: '', open: false }
      S.live.reasoning += rec.text
      rateUpdate(rec.text)   // 思考增量也是模型输出：不喂会让推理模型的速率时钟停摆虚高
      return
    case 'assistant':
      // 收口后思考块保留可回看（reasoning 由宿主落盘在记录上）；展开态从流式块
      // 顺延——用户看着思考时收口，不跳回折叠。重放记录无 live ⇒ 默认折叠
      rec._thinkOpen = !!(S.live && S.live.reasoning && S.live.open)
      S.live = null                    // 思考结束：折叠流式块（记录块接管展示）
      push(rec)
      // inFlight = turn 进行中（带 tool_calls 的 done 只是一轮收束，工具/子代理
      // 阶段仍是同一 turn——停止入口必须全程在场，否则子代理挂起期无路可退）
      S.inFlight = rec.turn_done === false
      rateClose()   // 本响应流结束：收口输出期，其后的工具执行/空闲不计时（会话均值保留累计）
      if (rec.usage && rec.usage.total_tokens) {
        S.lastPrompt = rec.usage.prompt_tokens || 0
      }
      { const done = rec.turn_done !== undefined ? rec.turn_done : !!rec.text
        if (done) flushTurnEdits() }
      return
    case 'subagent':
      // 收口记账（phase>=4：4=完成 5=取消/失败）核销挂起条——task 的 tool
      // 结果卡已去重，不再有 tool 记录承担核销
      if (rec.phase >= 4) {
        S.subDone[rec.turn_id] = rec.phase
        if (S.waitingLeft > 0) {
          S.waitingLeft--
          if (S.waitingLeft === 0) removeWaitline()
        }
      }
      // phase 2/3 工具明细（宿主现行已写独立存档，旧会话重放仍会带来）与
      // phase 4 完成行不进主流：明细平铺是刷屏源；完成与 spawn 卡的状态翻转
      // 同信息。主流只留 spawn 卡（0）与失败卡（5）
      if (rec.phase === 2 || rec.phase === 3 || rec.phase === 4) return
      push(rec)
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
    case 'tool_call':
      rec._done = false; rec._runs = []
      push(rec); return
    case 'tool':
      // apply 的成功结果与本轮修改汇总卡同信息（同文件同增删），不渲染；
      // 失败结果保留——那是唯一的错误反馈（踩过: 双行同文）
      if (rec.ok && /apply$/.test(rec.tool || '')) return
      // 挂起核销：等待期内的每条工具结果 = 一个挂起的子代理/后台任务落定
      // （顺序纪律：等待通告只由 dispatch_loop 尾部发出，其后的 tool 结果
      // 必然属于挂起项）
      if (S.waitingLeft > 0) { S.waitingLeft--; if (S.waitingLeft === 0) removeWaitline() }
      { const call = lastOpenCall(rec.tool)
        if (!call) {
          // 孤儿结果：run 的终态结果（exit 形态）此刻调用行必已被 run_exit 收口
          // （EXIT 恒先于终态 RES 到达或已按 RES 收口）——再兜底成行就是重复行；
          // 非 exit 形态（创建失败等真实错误）必须保留
          if (rec.tool === 'runner.run' && /^exit \d+/.test(rec.brief || '')) return
          push(rec); return
        }
        if (call.tool === 'runner.run') {
          // 转后台 ack 直接吃掉不留行——ack 是两条派发路径唯一共带的归属信号，
          // 映射在此建立（决策坞批准路径不发「已启动」通告）。run_id 走契约
          // ToolResult 尾部追加字段；文案反解只兜旧会话重放（字段前时代的转录）
          const rid = rec.run_id > 0 ? rec.run_id
            : Number((/^已转后台（run#(\d+)）/.exec(rec.brief || '') || [])[1] || 0)
          if (rid > 0) {
            call._runId = rid
            X.runCall.set(rid, call)
            return
          }
          if (/^exit -1/.test(rec.brief || '')) return   // 旧数据 ack（无 run_id）：归属靠通告
          // 真实结果带输出预览但无 run_id，并发 run 会乱序——有映射的等 run_exit
          // 收口；无映射的（创建失败等，从未成为后台 run）就地收口
          if (call._runId !== undefined) return
          call._result = { ok: rec.ok, brief: rec.brief }; call._done = true
          return
        }
        call._result = { ok: rec.ok, brief: rec.brief }; call._done = true
        return }
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
      // 不再自成一行：与本轮修改汇总卡同信息（同文件同增删），卡还能展开看
      // diff——行是纯重复。聚合与文件树刷新仍要照常
      if (rec.diff) accumulateApplied(rec)
      S.fsRev++
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
    case 'run_line':
      // stdout/stderr 都进输出块（契约 0=stdout 1=stderr；旧守卫只收 1——775 行
      // stdout 曾因此整段从 UI 消失，只在结果行留首行预览）；失败面由 exit code
      // 与调用行红显承担
      runBlock(rec.run_id).lines.push(rec.text)
      return
    case 'fs_changed':
      // 磁盘真变化（webui watcher 的 SSE 瞬态信号）⇒ 文件树自动跟上；
      // 不进 records——它不是对话内容，落盘重放反而要在会话加载时压掉
      S.fsRev++
      return
    case 'run_exit': {
      // 不自成一行：exit code 并入输出块与调用行后缀（工具结果行同款信息，
      // 双行是纯重复）
      const b = X.runs.get(rec.run_id)
      if (b) { b.exit = rec.exit_code; b.failed = rec.exit_code !== 0 }
      const call = X.runCall.get(rec.run_id)
      if (call && !call._done) {
        call._result = { ok: rec.exit_code === 0, exit: rec.exit_code }
        call._done = true
        // 失败即展开输出块：错误文本就在里面，默认折叠会让失败看起来没有细节
        const b = X.runs.get(rec.run_id)
        if (rec.exit_code !== 0 && b && b.lines.length) call._open = true
      }
      return
    }
    case 'notice': {
      // 机器路由优先（Notice 尾部追加 kind 字段）；prose 正则只兜旧会话重放。
      // 后台 run 启动通告不落流（调用行已示），归属主要靠 ack 的 run_id 建立，
      // 通告只兜旧数据——已有映射不覆盖
      if (rec.kind === 'bg_run_started') {
        if (rec.run_id > 0 && !X.runCall.has(rec.run_id))
          X.runCall.set(rec.run_id, lastOpenCall('runner.run'))
        return
      }
      // 挂起等待信号：agent 在派发尾部仍有未决子代理/后台任务时发（run 单独
      // 等待与 task/混合等待同款式）。等待行 upsert 成单条动态行，核销即撤
      if (rec.kind === 'waiting') {
        S.waitingSince = Date.now(); S.waitingLeft = rec.count || 0
        upsertWaitline(rec.text); return
      }
      const started = /^后台任务 run#(\d+) 已启动/.exec(rec.text || '')
      if (started) {
        const rid = Number(started[1])
        if (!X.runCall.has(rid)) X.runCall.set(rid, lastOpenCall('runner.run'))
        return
      }
      const m = /^等待 (\d+) 个/.exec(rec.text || '')
      if (m) {
        S.waitingLeft = Number(m[1])
        upsertWaitline(rec.text); return
      }
      push(rec); return
    }
    case 'error':
      S.inFlight = false
      rateClose()   // 流式中途报错同样收口输出期（会话均值保留累计）
      // 流式中途报错也要收掉 live 块：只复位 inFlight 会留下半截流式正文与光标，
      // 观感是「还在输出」（错误卡与残留正文并存，直到下一条 user 记录才清）
      push(rec); S.live = null; flushTurnEdits(); return
  }
}

// ---- 输出速率（会话累计均值，只计输出时段）-----------------------------------
// 会话均值（S.rate，用量块显示）= 会话累计的流式输出粗估 tokens / 累计「有增量
// 到达」的时长。输出期由本响应首个增量开启、assistant/error 收口关闭
// （rateClose）——轮间空闲（工具执行/用户思考）与首增量前的等待（TTFT）不计入；
// 响应内单次增量间隔超过 RATE_GAP_MS 视为链路断续（补洞场景的静默），超出部分
// 同样不计。思考增量与正文增量同喂（推理输出也是输出，不喂会让速率时钟停摆虚高）。
// tokens 沿用增量字符粗估（ASCII ≈ 4 字符/token、CJK ≈ 0.7 token/字，与后端
// breakdown 的 utf8_tokens 同口径）：provider 的 completion_tokens 收口才可得，
// 且含未流式可见的工具调用 JSON，与观测时长不可比，故不做真值混算。增量不落盘
// ⇒ 重放不可重建，均值只统计本次打开以来的流式；S.rate '' = 尚无
// 计量（显示面隐藏）；切会话/撤回即清零重新统计。累计器放 X 而非模块局部：
// 重置面在 sessions.js 的 resetView，双向 import 会成环。
const RATE_GAP_MS = 10000
function rateTok(text) {
  let ascii = 0, non = 0
  for (let i = 0; i < text.length; i++) {
    const c = text.codePointAt(i)
    if (c > 0xffff) i++   // 代理对按一个码点计
    if (c < 0x80) ascii++; else non++
  }
  return ascii / 4 + non * 0.7
}
function rateUpdate(text) {
  const now = Date.now()
  if (X.rateLast && now - X.rateLast <= RATE_GAP_MS) X.rateMs += now - X.rateLast
  X.rateLast = now
  X.rateToks += rateTok(text)
  if (X.rateMs > 0) S.rate = String(Math.round(X.rateToks / (X.rateMs / 1000)))
}
/** 输出期收口：此后到下一响应首个增量之间的空闲不并入输出时长。 */
function rateClose() {
  X.rateLast = 0
}

// ---- 数据通道 ----------------------------------------------------------------

/** 重放/补洞：HTTP 拉取 after 起的记录并逐条渲染。串行化（并发调用排队），
 *  执行期间停靠 SSE 记录、结束后放行（竞态推导见文件头 parkSse 注释）。 */
export function pollOnce() {
  const run = pollChain.then(pollOnceImpl)
  pollChain = run.then(() => {}, () => {})
  return run
}

async function pollOnceImpl() {
    // 草稿/切换在途门禁：视图不属于任何服务端会话或切换尚未获服务端确认，而
    // /api/transcript 恒回「服务端活跃会话」的转录——此时补洞会把上一会话的
    // 全文按 after=0 灌进当前视图 = 「新建会话还没发消息就自动跳回旧会话」
    //（踩过: 补洞恒跑后必现）。启动首拉不受影响：彼时两者尚为 null/false
    //（enterDraft 在 boot 尾部才进，switchPending 只在 openSession 点击后置位）。
    if (S.draft || S.switchPending) return
    parkSse = true
  try {
    const seq0 = rollbackSeq
    const t = await api('/api/transcript?after=' + S.since)
    if (seq0 !== rollbackSeq) return pollOnceImpl()   // 拉取期发生撤回：游标已重置，按新位重拉
    t.records.forEach(renderNow)
    S.since = t.next
    flushTurnEdits()   // 重放结束 ⇒ 最后一轮的编辑汇总也落位（否则丢了）
    // 重放后以后端原子真值对账：user 记录在重放里无条件置 inFlight=true，失败/
    // 取消轮没有 assistant/error 收口时会把「进行中」复活（踩过: zombie——切回
    // 失败会话按钮恒为停止态），以后端 in_flight 为准兜掉一切记录流缺口
    try {
      const m = await api('/api/session')
      S.inFlight = !!m.in_flight
    } catch (e) { /* 对账端点失联：游标已推进，下次补洞继续 */ }
  } finally {
    parkSse = false
    const drain = parked.splice(0)
    drain.forEach(renderNow)
  }
}

/** 发送前的兜底收口（上一响应若未经 assistant/error 收口，也不把空闲带进下一
    响应）。会话均值跨轮累计，发送不再清零。 */
export function liveStart() {
  rateClose()
}
