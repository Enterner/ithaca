<script setup>
// 对话流：记录列表 + 流式块。消息级内容平铺，过程层收敛——工具调用收成一行
// （默认收起，展开看调用参数与输出）、run 输出折叠、applied/完成行并入汇总卡
// 与状态卡——records 层过滤。提案卡（逐文件批准）/「本轮修改」汇总卡内联实现，
// diff 着色走 diff.js 的 v-html（内容全部经 esc——信任级别与正文一致）。
// 折叠块（思考/工具卡/run 兜底）统一走 FoldBlock 轮子（.fold 网格动画 + Chev
// 旋转箭头 + 语义色脊柱）；编辑行的展开行留在汇总卡内不走轮子（卡片自带行几何）。
// 渲染成本两面收口：md/diff 逐记录缓存（收口记录不可变，算一次挂对象上）+
// 尾部窗口渐进挂载（历史窗口段——切会话/重放只挂最近一段，触顶扩窗补更早历史）。
import { ref, computed, watch, nextTick, onMounted } from 'vue'
import { trackPress, pressed } from '../press'
import { S, isImageAtt, attUrl, api, pushLocal } from '../store'
import { mdHtml, esc, toolLineHtml, toolName } from '../md'
import { diffHtml, stripHeads } from '../diff'
import Chev from './Chev.vue'
import FoldBlock from './FoldBlock.vue'

const streamEl = ref(null)
const thinkEl = ref(null)
function scrollBottom() {
  if (streamEl.value) streamEl.value.scrollTop = streamEl.value.scrollHeight
}
// 常规跟随：记录数/流式正文变化即滚底。用户上翻阅读（离底 > 48px）时不拽回
// ——与思考体同款守卫：流式增量（每个 delta 都进这条）不能把正在读历史的
// 用户拽回底部。守卫读的是更新前的位置：新增内容只会让离底更大，不会误判
watch(() => [S.records.length, S.live && S.live.text],
  () => {
    const el = streamEl.value
    if (el && el.scrollHeight - el.scrollTop - el.clientHeight > 48) return
    nextTick(scrollBottom)
  }, { deep: true })
// 思考阶段 = live 块在且正文未开始（reasoning 先于 content 流；收口后 live 置空）
const thinking = computed(() => !!(S.live && S.live.reasoning && !S.live.text))
// 思考跟随：展开时吸附思考体内部底部；尚未超出上限（无内部滚动、块体仍在长高）
// 时退化为推页面滚底。用户上翻阅读（离底 > 48px）时不拽回。
// NOTE: 思考体常驻 DOM（.fold 收起只压高度，不做 v-if）——滚动跟随以 live.open
// 为门卫，收起期不动作；这也是折叠动画（高度过渡）能成立的前提
watch(() => S.live && S.live.reasoning, () => nextTick(() => {
  const el = thinkEl.value
  if (!el || !S.live || !S.live.open) return
  const pinned = el.scrollHeight - el.scrollTop - el.clientHeight < 48
  if (!pinned) return   // 用户上翻阅读中：不拽回
  el.scrollTop = el.scrollHeight
  if (el.scrollHeight <= el.clientHeight) scrollBottom()
}))
// 思考中途手动展开：直接落到思考体底部（最新思考），后续增量交给上面的跟随
watch(() => S.live && S.live.open, (v) => {
  if (v) nextTick(() => { if (thinkEl.value) thinkEl.value.scrollTop = thinkEl.value.scrollHeight })
})
// NOTE: 初始定位必须主动做——本组件在会话模式下才挂载，挂载时记录往往已装载
// 完毕（数量不再变化 ⇒ watch 永不触发 ⇒ 刷新/切会话后停在顶部，踩过）
onMounted(() => nextTick(scrollBottom))
watch(() => S.curSession, () => nextTick(scrollBottom))

// ---- 历史窗口（切会话/重放的卡顿治理）---------------------------------------
// 对话流按「尾部窗口」挂载：重放/切换先只挂最近 WINDOW 条，上滚触顶再按 CHUNK
// 补挂更早历史。全量挂载让长会话的切换 = 一次数千节点的同步 mount（明显卡顿）；
// 窗口把首屏成本钉在常数上，读历史靠触顶扩窗补齐。锚点 = startIdx（渲染起始
// 下标）：重放的「清空→成批进记录」迁移（old=0）时锚到尾部；之后只进不退——
// 新记录追加不动锚点，正在读历史的人视口不漂移。
const WINDOW = 120
const CHUNK = 150
const startIdx = ref(0)
const shown = computed(() => {
  const recs = S.records
  if (startIdx.value <= 0) return recs
  // 挂起等待行是动态状态行，出窗也要保持可见（起点回拉到它——它必然靠近尾部）
  let waitAt = -1
  for (let i = recs.length - 1; i >= 0; i--) if (recs[i]._wait) { waitAt = i; break }
  const from = waitAt >= 0 ? Math.min(startIdx.value, waitAt) : startIdx.value
  return from <= 0 ? recs : recs.slice(from)
})
watch(() => S.records.length, (n, old) => {
  if (!n || !old) startIdx.value = Math.max(0, n - WINDOW)
}, { immediate: true })   // 挂载时记录可能已整装（boot 重放先于组件）：立即锚一次
function onScroll() {
  const el = streamEl.value
  if (!el || el.scrollTop > 2 || startIdx.value <= 0) return
  const top = el.scrollTop, h = el.scrollHeight
  startIdx.value = Math.max(0, startIdx.value - CHUNK)
  nextTick(() => {   // 上方长出了内容：补同样的高度差，视口停留的记录不动
    if (streamEl.value) streamEl.value.scrollTop = top + (streamEl.value.scrollHeight - h)
  })
}

// ---- 逐记录渲染缓存 ----------------------------------------------------------
// 收口记录与 diff 文本不可变（收口后只读），HTML 算一次挂在记录对象上：组件任何
// 响应式变化都会整体重跑 v-for，无缓存 = 每次重渲染都对全部历史重算 marked/diff
// ——长会话越用越卡、切回来更卡（明显卡顿的主源）。缓存字段带 _ 前缀，与交互
// 态（_open 等）同一约定：宿主记录上的本地展示态。
function msgHtml(rec) { return rec._mdh ??= mdHtml(rec.text || '') }
function propDiffHtml(rec) { return rec._dh ??= diffHtml(multi(rec) ? stripHeads(rec.diff) : rec.diff) }
function editDiff(f) {
  return f._dh ??= diffHtml(f.applies.length === 1 ? f.applies[0].replace(/^── [^\n]*\n/, '') : f.applies.join(''))
}
// ---- 工具卡（单行收起 + 展开详情）--------------------------------------------
// 展开详情 = 调用参数（args 原始 JSON，契约尾部追加字段；旧记录无此字段则无
// 参数节）+ run 输出块。结果摘要不进详情——它以行内右缀常显（.procres）。
// 点击整行切换展开；详情 = 结果摘要 + 参数 JSON + run 输出——任一存在即可展开
//（结果摘要已收进折叠体，仅有摘要（无参数无输出）的行同样要点得开）
function hasToolDetail(rec) {
  if (!!(rec.args && rec.args.trim()) || !!(rec._runs && rec._runs.length)) return true
  const row = rowOf(rec)
  return !!row && !!row.text && row.text !== '✓'   // 裸 ✓ 无内容可看，不值展开
}
function toggleTool(rec) {
  if (hasToolDetail(rec)) rec._open = !rec._open
}
function argsPretty(rec) {
  if (!rec.args) return ''
  try { return JSON.stringify(JSON.parse(rec.args), null, 2) } catch { return rec.args }
}
function toolFailed(rec) {
  return rec.type === 'tool' && !rec.ok   // 仅无配对兜底行红显；配对调用的失败由结果缀红显
}
/** 结果缀内容：null = 不出结果缀（无配对/运行中无输出/成功无输出） */
function resRow(rec) {
  if (rec.type !== 'tool_call') return null
  const r = rec._result
  const nOut = rec._runs ? rec._runs.reduce((n, b) => n + b.lines.length, 0) : 0
  if (!r) return nOut > 0 ? { text: nOut + ' 行输出' } : null   // 运行中已有输出
  if (r.exit !== undefined && r.exit !== null)
    // 失败：有输出只标 ✗（错误文本在自动展开的输出块里）；静默失败留 exit N
    //——输出块为空，退出码是唯一线索。成功同判：输出行数才值得占位
    return r.ok ? (nOut > 0 ? { text: nOut + ' 行输出' } : null)
                : (nOut > 0 ? { text: '✗', out: true, warn: true }
                            : { text: '✗ exit ' + r.exit, warn: true })
  return r.brief ? { text: (r.ok ? '' : '✗ ') + r.brief, warn: !r.ok } : null
}
// 每渲染周期每条可见调用卡只算一次结果行（模板里多处取用——直接调 resRow 会对
// rec._runs 反复 reduce；Map 按 record 对象取，O(1)）。只扫窗口内记录：组件任何
// 一处响应式变化都会让本 computed 重算，全量扫描会把重放变成 O(n²)
const resRows = computed(() => {
  const m = new Map()
  for (const rec of shown.value) if (rec.type === 'tool_call') m.set(rec, resRow(rec))
  return m
})
function rowOf(rec) { return resRows.value.get(rec) ?? null }
/** 行内结果缀：结果摘要 / 运行中（转圈）/ 已收口无可报（✓/✗）。 */
function toolStat(rec) {
  const row = rowOf(rec)
  if (row) return { text: row.text, warn: !!row.warn, run: !rec._done }
  if (rec._result)
    return { text: rec._result.ok === false ? '✗' : '✓',
             warn: rec._result.ok === false, run: false }
  return { text: '', warn: false, run: !rec._done }
}
function runOutText(rec) {
  return rec._runs.map((b) => b.lines.join('\n')).join('\n')
}

// ---- 折叠行的选字豁免（仅汇总卡内编辑行用——流内行型已收编 FoldBlock）--------
// 在可折叠行上拖选文字松开，click 仍会触发折叠切换——mousedown 记起点、
// click 比位移，>5px 视为选择操作不放行
const pressPos = { x: null, y: null }   // 拖选豁免共用件（press.js）

// 提案卡：只负责展示（diff + 逐文件勾选）；裁决走输入栏上方的决策坞（ZCode 式）。
// 勾选/裁决标记挂在记录对象上（store render 时初始化），重放与实时共用。
// 状态列：只有决策坞挂着的「最新」提案显示 待你裁决；其余未决的（历史重放/被
// 更新提案接替的）一律 已失效 并弱化。
function multi(rec) { return (rec._files || []).length >= 2 }
function statText(rec) {
  if (rec === S.pending) return '待你裁决'
  if (rec._decided === 0) return '已批准'
  if (rec._decided === 1) return '已拒绝'
  if (rec._decided === 2) return '已免批'
  return '已失效'
}
function superseded(rec) { return rec !== S.pending && rec._decided === null }

// 中断卡的撤回：整轮回退（模型上下文 + 显示记录，服务端两侧守卫同款）；尾部
// 不适格（已续聊）时服务端拒绝，反馈落流；成功即本地摘除按钮
async function revokeInterrupt(rec) {
  const r = await api('/api/session/cmd', { cmd: 2 })
  if (r.ok) rec._revoked = true
  else pushLocal(r.error || '撤回失败')
}

async function onStreamClick(e) {   // 代码块复制按钮（v-html 内容的事件委托）
  const b = e.target.closest('.cpy')
  if (b) {
    await navigator.clipboard.writeText(decodeURIComponent(b.dataset.c))
    b.textContent = '已复制'
    setTimeout(() => { b.textContent = '复制' }, 1200)
  }
}
</script>

<template>
  <div ref="streamEl" class="stream" @click="onStreamClick" @scroll="onScroll">
    <div class="flow">
      <div v-if="!S.records.length && !S.live" class="empty">启动后在这里看到对话流——输入任务开始。</div>

      <!-- 键：宿主记录用 'h'+id，伪记录用入库序号（窗口渲染下下标会漂移） -->
      <template v-for="(rec, i) in shown" :key="rec.id !== undefined ? 'h' + rec.id : (rec._k ?? 'r' + i)">
        <template v-if="rec.type === 'user'">
          <!-- 附件挂在消息上方（ZCode 式）：图片直接出预览（图即全部语义），
               文档类保留文件名 chip；mime 为空 = 后端读取失败/非图片扩展名 -->
          <div class="uwrap">
            <div v-if="rec.attachments && rec.attachments.length" class="uatts">
              <template v-for="a in rec.attachments" :key="a.path">
                <img v-if="isImageAtt(a)" class="uatt-img" :src="attUrl(a.path)"
                     :alt="a.name" :title="a.name" loading="lazy">
                <span v-else class="uatt" :title="a.path">
                  <span class="ico">📎</span>{{ a.name }}
                </span>
              </template>
            </div>
            <div class="msg u">{{ rec.text }}</div>
          </div>
        </template>

        <template v-else-if="rec.type === 'assistant' && (rec.text || rec.reasoning)">
          <!-- 收口记录的思考块：默认折叠可展开（reasoning 由宿主落盘，重放同样可回看） -->
          <FoldBlock v-if="rec.reasoning" :open="!!rec._thinkOpen" clickable
                     @toggle="rec._thinkOpen = !rec._thinkOpen">
            <template #head>
              <span class="ico">💭</span><span class="t">深度思考</span><Chev :open="!!rec._thinkOpen" />
            </template>
            <div class="think-body">{{ rec.reasoning }}</div>
          </FoldBlock>
          <template v-if="rec.text">
            <div class="msg a" v-html="msgHtml(rec)"></div>
          </template>
        </template>

        <template v-else-if="rec.type === 'tool_call' || rec.type === 'tool'">
          <!-- 配对调用 = 单行工具卡：调用摘要 + 结果缀（.procres 保留独立语义：
               结果摘要），默认收起、整行点击展开（参数 JSON + run 输出）。
               运行中（结果未归位）复用思考行的流光/呼吸动画。无配对兜底结果行
               （type=tool）为纯展示头行 -->
          <FoldBlock v-if="rec.type === 'tool_call'" :open="!!rec._open"
                     :class="{ live: toolStat(rec).run }"
                     :clickable="hasToolDetail(rec)" :warn="toolStat(rec).warn" @toggle="toggleTool(rec)">
            <template #head>
              <span class="tico" :class="{ dim: rec._done }">⚙</span>
              <span class="tlabel" v-html="toolLineHtml(rec.tool, rec.brief, S.curWs)"></span>
              <Chev v-if="hasToolDetail(rec)" :open="!!rec._open" />
              <span class="spacer"></span>
              <span v-if="toolStat(rec).run" class="spinner" title="运行中"></span>
            </template>
            <!-- 结果摘要进折叠体（不再作行尾右缀）：收起行只剩调用摘要+箭头 -->
            <div v-if="toolStat(rec).text" class="procres" :class="{ warn: toolStat(rec).warn }">{{ toolStat(rec).text }}</div>
            <div v-if="rec.args && rec.args.trim()" class="targs"><pre>{{ argsPretty(rec) }}</pre></div>
            <div v-if="rec._runs && rec._runs.length" class="runout">
              <div class="runout-body">{{ runOutText(rec) }}</div>
            </div>
          </FoldBlock>
          <FoldBlock v-else :warn="toolFailed(rec)">
            <template #head>
              <span class="tico">⚙</span><span class="tlabel" v-html="toolLineHtml(rec.tool, rec.brief, S.curWs)"></span>
            </template>
          </FoldBlock>
        </template>

        <!-- 「本轮修改」汇总卡（记录流内伪记录：结算时已落位到自己轮次） -->
        <template v-else-if="rec.type === 'turn_edits'">
          <div class="who">ithaca · 本轮修改</div>
          <div class="card edits" :class="{ revoked: rec.revoked }">
            <div class="head">
              <b>本轮修改 {{ rec.files.length }} 个文件</b>
              <span class="stat">
                <template v-if="rec.revoked">已撤销</template>
                <template v-else>
                  <i class="add">+{{ rec.files.reduce((s, f) => s + f.add, 0) }}</i>
                  <i class="del">−{{ rec.files.reduce((s, f) => s + f.rem, 0) }}</i>
                </template>
              </span>
            </div>
            <template v-for="f in rec.files" :key="f.path">
              <div class="erow" :class="{ open: f.open }" @mousedown="trackPress($event, pressPos)"
                   @click="pressed($event, pressPos) && (f.open = !f.open)">
                <Chev :open="!!f.open" /><span class="p">{{ f.path }}</span>
                <span v-if="f.isNew" class="ntag">新文件</span>
                <span class="st">
                  {{ f.n > 1 ? f.n + ' 次编辑 · ' : '' }}
                  <i class="add">+{{ f.add }}</i> <i class="del">−{{ f.rem }}</i>
                </span>
              </div>
              <div class="fold" :class="{ open: f.open }">
                <div class="fold-in">
                  <div class="ebody">
                    <div class="diff" v-html="editDiff(f)"></div>
                  </div>
                </div>
              </div>
            </template>
          </div>
        </template>

        <template v-else-if="rec.type === 'undone'">
          <div class="toolline">↩ 已撤销</div>
        </template>

        <template v-else-if="rec.type === 'run_out'">
          <!-- 无归属 run 的兜底块（映射缺失时自成一行）：同样默认折叠 -->
          <FoldBlock :open="!!rec._open" clickable :warn="rec.failed"
                     @toggle="rec._open = !rec._open">
            <template #head>
              <span class="tlabel">run #{{ rec.run_id }} · {{ rec.lines.length }} 行输出<template v-if="rec.exit !== null"> · exit {{ rec.exit }}</template></span>
              <Chev :open="!!rec._open" />
            </template>
            <div class="runout">
              <div class="runout-body">{{ rec.lines.join('\n') }}</div>
            </div>
          </FoldBlock>
        </template>

        <template v-else-if="rec.type === 'subagent'">
          <!-- 子代理工作记录视图：spawn 卡可点击开右栏过程视图（实时+可取消）；
              明细/完成行由 records 层过滤，这里只见 0 与 5 -->
          <div v-if="rec.phase === 0" class="subcard" title="点击在右栏查看工作过程"
               @click="S.subView = rec.turn_id">
            <span class="subtag">🤖 子代理[{{ rec.tool }}]</span>
            <span class="subdesc">{{ rec.text }}</span>
            <span class="stat" :class="{ live: !S.subDone[rec.turn_id] }">
              {{ !S.subDone[rec.turn_id] ? '进行中 · 点击查看'
                 : S.subDone[rec.turn_id] >= 5 ? '已取消 · 点击查看' : '已完成 · 点击查看' }}
            </span>
          </div>
          <div v-else-if="rec.phase === 5" class="errcard">✗ 子代理失败：{{ rec.text }}</div>
        </template>

        <template v-else-if="rec.type === 'notice'">
          <div v-if="(rec.level || 0) >= 3" class="errcard">{{ rec.text }}</div>
          <!-- 挂起等待动态行（单条、核销即撤）：与系统通告同款居中弱化 -->
          <div v-else-if="rec._wait" class="sysline">⏳ {{ rec.text }}</div>
          <!-- 系统通告（sys=1）居中分隔；「模型已切换」「权限模式 = 」通告隐藏
              （选择器自身即反馈，写流属刷屏噪音——踩过: 反复切换连发刷屏）；
              「后台任务已启动」不落流（调用行已示，records 层已滤） -->
          <div v-else-if="rec.sys" class="sysline">── {{ rec.text }} ──</div>
          <div v-else-if="/^(模型已切换|权限模式 = |宿主停止|没有进行中的 turn|子代理已派出|子代理完成（|子代理失败: )/.test(rec.text || '')" style="display:none"></div>
          <div v-else class="msg">{{ rec.text }}</div>
        </template>
        <template v-else-if="rec.type === 'error'">
          <!-- 中断卡：文案即识别键（agent 取消哨兵），↩ 撤回该回合的模型上下文
              （图标无文字，交互语义在 tooltip）；不适格（已续聊）由服务端通告 -->
          <div class="errcard" :title="'错误码 ' + rec.code">⚠ {{ rec.msg }}<button
              v-if="rec.msg === '当前会话中断' && !rec._revoked" class="rc-btn"
              title="撤回本轮"
              @click.stop="revokeInterrupt(rec)">↩</button></div>
        </template>
        <template v-else-if="rec.type === 'local'">
          <div class="msg">{{ rec.text }}</div>
        </template>

        <template v-else-if="rec.type === 'proposal'">
          <div class="who">ithaca · 提案</div>
          <div class="card" :class="{ superseded: superseded(rec) }">
            <div class="head"><b>{{ toolName(rec.tool || 'workspace.apply') || '编辑' }}</b><span class="stat">{{ statText(rec) }}</span></div>
            <div v-if="multi(rec)" class="diff">
              <label v-for="p in rec._files" :key="p" class="fhead-row">
                <input type="checkbox" v-model="rec._checked[p]"> {{ p }}
              </label>
            </div>
            <div class="diff" v-html="propDiffHtml(rec)"></div>
          </div>
        </template>
      </template>

      <!-- 流式块：深度思考（默认折叠；展开后超上限内部滚动）+ 正文 + 光标 -->
      <template v-if="S.live">
        <FoldBlock v-if="S.live.reasoning" :open="!!S.live.open" clickable
                   :class="{ live: thinking }" @toggle="S.live.open = !S.live.open">
          <template #head>
            <span class="ico">💭</span>
            <span class="t">{{ thinking ? '思考中…' : '深度思考' }}</span>
            <Chev :open="!!S.live.open" />
          </template>
          <div ref="thinkEl" class="think-body">{{ S.live.reasoning }}</div>
        </FoldBlock>
        <div v-if="S.live.text || !S.live.reasoning" class="msg a plain">{{ S.live.text }}<span class="cursor"></span></div>
      </template>
    </div>
  </div>
</template>
