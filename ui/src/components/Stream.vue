<script setup>
// 对话流：记录列表 + 流式块。消息级内容平铺，过程层收敛（工具调用+结果一行、
// run 输出折叠、applied/完成行并入汇总卡与状态卡——records 层过滤）。提案卡
//（逐文件批准）/「本轮修改」汇总卡内联实现，diff 着色走 diff.js 的 v-html
//（内容全部经 esc——信任级别与正文一致）。
import { ref, computed, watch, nextTick, onMounted } from 'vue'
import { S } from '../store'
import { mdHtml, esc, toolLabel } from '../md'
import { diffHtml, stripHeads } from '../diff'

const streamEl = ref(null)
const thinkEl = ref(null)
function scrollBottom() {
  if (streamEl.value) streamEl.value.scrollTop = streamEl.value.scrollHeight
}
// 常规跟随：记录数/流式正文变化即滚底。思考增量不进这条——思考体有自己的
// 滚动域，推页面滚底会让「思考时页面一直往下走」
watch(() => [S.records.length, S.live && S.live.text],
  () => nextTick(scrollBottom), { deep: true })
// 思考阶段 = live 块在且正文未开始（reasoning 先于 content 流；收口后 live 置空）
const thinking = computed(() => !!(S.live && S.live.reasoning && !S.live.text))
// 思考跟随：展开时吸附思考体内部底部；尚未超出上限（无内部滚动、块体仍在长高）
// 时退化为推页面滚底。用户上翻阅读（离底 > 48px）时不拽回。
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

function toolHtml(rec) {
  const brief = rec.brief || ''
  const label = toolLabel(rec.tool, brief)
  // label 为空 = 摘要自含中文动词（read/list/search/run/recall/task），直接显示
  // 短句；否则「中文标签 + 摘要」
  return label ? '<b>' + esc(label) + '</b> ' + esc(brief) : esc(brief)
}
// ---- 过程行（两行制：调用行 + 独立结果行；run 输出折叠在结果行下）------------
// 结果独立成行（ZCode 式）：退出码/结果摘要住结果行，与命令文本物理分行——
// 命令串含 exit N 也不会拼出重叠；失败红显，run 失败恒带 exit N（分行后无
// 撞车顾虑，退出码是最廉价的失败诊断）

function toolFailed(rec) {
  return rec.type === 'tool' && !rec.ok   // 仅无配对兜底行红显；配对调用的失败由结果行红显
}
/** 结果行内容：null = 不出结果行（无配对/运行中无输出/成功无输出） */
function resRow(rec) {
  if (rec.type !== 'tool_call') return null
  const r = rec._result
  const nOut = rec._runs ? rec._runs.reduce((n, b) => n + b.lines.length, 0) : 0
  if (!r) return nOut > 0 ? { text: nOut + ' 行输出', out: true } : null   // 运行中已有输出
  if (r.exit !== undefined && r.exit !== null)
    // 失败：有输出只标 ✗（错误文本在自动展开的输出块里）；静默失败留 exit N
    //——输出块为空，退出码是唯一线索。成功同判：输出行数才值得占位
    return r.ok ? (nOut > 0 ? { text: nOut + ' 行输出', out: true } : null)
                : (nOut > 0 ? { text: '✗', out: true, warn: true }
                            : { text: '✗ exit ' + r.exit, warn: true })
  return r.brief ? { text: (r.ok ? '' : '✗ ') + r.brief, warn: !r.ok, out: false } : null
}
function toggleRunOut(rec) {
  if (rec._runs && rec._runs.length) rec._open = !rec._open
}
function runOutText(rec) {
  return rec._runs.map((b) => b.lines.join('\n')).join('\n')
}

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
  <div ref="streamEl" class="stream" @click="onStreamClick">
    <div class="flow">
      <div v-if="!S.records.length && !S.live" class="empty">启动后在这里看到对话流——输入任务开始。</div>

      <template v-for="(rec, i) in S.records" :key="rec.id ?? 'r' + i">
        <template v-if="rec.type === 'user'">
          <div class="who u">你</div>
          <div class="msg u">{{ rec.text }}</div>
        </template>

        <template v-else-if="rec.type === 'assistant' && (rec.text || rec.reasoning)">
          <!-- 收口记录的思考块：默认折叠可展开（reasoning 由宿主落盘，重放同样可回看） -->
          <div v-if="rec.reasoning" class="think" :class="{ closed: !rec._thinkOpen }">
            <div class="think-head" @click="rec._thinkOpen = !rec._thinkOpen">
              <span class="ico">💭</span><span class="t">深度思考</span><span class="chev">▾</span>
            </div>
            <div v-if="rec._thinkOpen" class="think-body">{{ rec.reasoning }}</div>
          </div>
          <template v-if="rec.text">
            <div class="who">Ithaca</div>
            <div class="msg a" v-html="mdHtml(rec.text || '')"></div>
          </template>
        </template>

        <template v-else-if="rec.type === 'tool_call' || rec.type === 'tool'">
          <!-- 过程两行制：调用行（跑了什么）+ 结果行（结局，独立弱色行） -->
          <div class="toolline proc" :class="{ warn: toolFailed(rec) }" v-html="'⚙ ' + toolHtml(rec)"></div>
          <template v-if="rec.type === 'tool_call'">
            <div v-if="resRow(rec)" class="procres"
                 :class="{ warn: resRow(rec).warn, click: resRow(rec).out, open: rec._open }"
                 @click="resRow(rec).out && toggleRunOut(rec)">
              └ {{ resRow(rec).text }}<span v-if="resRow(rec).out" class="chev">▸</span>
            </div>
            <div v-if="rec._open && rec._runs && rec._runs.length" class="runout">
              <div class="runout-body">{{ runOutText(rec) }}</div>
            </div>
          </template>
        </template>

        <!-- 「本轮修改」汇总卡（记录流内伪记录：结算时已落位到自己轮次） -->
        <template v-else-if="rec.type === 'turn_edits'">
          <div class="who">Ithaca · 本轮修改</div>
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
              <div class="erow" :class="{ open: f.open }" @click="f.open = !f.open">
                <span class="chev">▶</span><span class="p">{{ f.path }}</span>
                <span v-if="f.isNew" class="ntag">新文件</span>
                <span class="st">
                  {{ f.n > 1 ? f.n + ' 次编辑 · ' : '' }}
                  <i class="add">+{{ f.add }}</i> <i class="del">−{{ f.rem }}</i>
                </span>
              </div>
              <div class="ebody">
                <div class="diff" v-html="diffHtml(f.applies.length === 1 ? f.applies[0].replace(/^── [^\n]*\n/, '') : f.applies.join(''))"></div>
              </div>
            </template>
          </div>
        </template>

        <template v-else-if="rec.type === 'undone'">
          <div class="toolline">↩ 已撤销（剩 {{ rec.depth }} 组快照）</div>
        </template>

        <template v-else-if="rec.type === 'run_out'">
          <!-- 无归属 run 的兜底块（映射缺失时自成一行）：同样默认折叠 -->
          <div class="toolline proc click" :class="{ warn: rec.failed, open: rec._open }"
               @click="rec._open = !rec._open">
            <span class="chev">▸</span> run #{{ rec.run_id }} · {{ rec.lines.length }} 行输出<template v-if="rec.exit !== null"> · exit {{ rec.exit }}</template>
          </div>
          <div v-if="rec._open" class="runout">
            <div class="runout-body">{{ rec.lines.join('\n') }}</div>
          </div>
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
              （选择器自身即反馈，写流属刷屏噪音——踩过：反复切换连发刷屏）；
              「后台任务已启动」不落流（调用行已示，records 层已滤） -->
          <div v-else-if="rec.sys" class="sysline">── {{ rec.text }} ──</div>
          <div v-else-if="/^(模型已切换|权限模式 = |宿主停止|没有进行中的 turn|子代理已派出|子代理完成（|子代理失败: )/.test(rec.text || '')" style="display:none"></div>
          <div v-else class="msg">{{ rec.text }}</div>
        </template>
        <template v-else-if="rec.type === 'error'">
          <div class="errcard" :title="'错误码 ' + rec.code">⚠ {{ rec.msg }}</div>
        </template>
        <template v-else-if="rec.type === 'local'">
          <div class="msg">{{ rec.text }}</div>
        </template>

        <template v-else-if="rec.type === 'proposal'">
          <div class="who">Ithaca · 提案</div>
          <div class="card" :class="{ superseded: superseded(rec) }">
            <div class="head"><b>{{ toolLabel(rec.tool || 'workspace.apply', '') || '编辑' }}</b><span class="stat">{{ statText(rec) }}</span></div>
            <div v-if="multi(rec)" class="diff">
              <label v-for="p in rec._files" :key="p" class="fhead-row">
                <input type="checkbox" v-model="rec._checked[p]"> {{ p }}
              </label>
            </div>
            <div class="diff" v-html="diffHtml(multi(rec) ? stripHeads(rec.diff) : rec.diff)"></div>
          </div>
        </template>
      </template>

      <!-- 流式块：深度思考（默认折叠；展开后超上限内部滚动）+ 正文 + 光标 -->
      <template v-if="S.live">
        <div v-if="S.live.reasoning" class="think" :class="{ closed: !S.live.open }">
          <div class="think-head" :class="{ live: thinking }" @click="S.live.open = !S.live.open">
            <span class="ico">💭</span><span class="t">{{ thinking ? '思考中…' : '深度思考' }}</span><span class="chev">▾</span>
          </div>
          <div v-if="S.live.open" ref="thinkEl" class="think-body">{{ S.live.reasoning }}</div>
        </div>
        <div v-if="S.live.text || !S.live.reasoning" class="who">Ithaca</div>
        <div v-if="S.live.text || !S.live.reasoning" class="msg a">{{ S.live.text }}<span class="cursor"></span></div>
      </template>
    </div>
  </div>
</template>
