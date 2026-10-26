<script setup>
// 输入栏：textarea + 权限模式/模型自绘下拉 + 上下文环 + 发送/停止。
// hero（无工作区）与 dock 共用本组件（输入内容在 store，切换不丢）。
// 有待裁决提案 ⇒ 决策坞（ZCode 式）：Enter 批准（所选文件）/ 本轮免批 /
// Esc 拒绝——输入框里的文字随拒绝作为反馈回灌给 agent。
import { computed, ref, watch, nextTick, onMounted, onBeforeUnmount } from 'vue'
import { S, send, decide, setMode, setModel, newSession, pickFolder,
         ctxSizeOf, fmtCtx, liveStart, fetchCtxBreakdown, fmtK } from '../store'
import Cdl from '../Cdl.vue'

// 权限模式三态的图标（下拉项左侧 + 触发器随模式切换，同一出处）：手=改前先问、
// 盾勾=编辑放行、叹号=全自动——语义与图3（Codex 式模式选择器）对齐。
const ico = (paths) => `<svg width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">${paths}</svg>`
const PMODE_ICONS = {
  suggest: ico('<path d="M18 11V6a2 2 0 0 0-2-2a2 2 0 0 0-2 2"/><path d="M14 10V4a2 2 0 0 0-2-2a2 2 0 0 0-2 2"/><path d="M10 10.5V6a2 2 0 0 0-2-2a2 2 0 0 0-2 2v8"/><path d="M18 8a2 2 0 1 1 4 0v6a8 8 0 0 1-8 8h-2c-2.8 0-4.5-.86-5.99-2.34l-3.6-3.6a2 2 0 0 1 2.83-2.82L7 15"/>'),
  'auto-edit': ico('<path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/><path d="M9 12l2 2 4-4"/>'),
  'full-auto': ico('<circle cx="12" cy="12" r="10"/><line x1="12" y1="8" x2="12" y2="12"/><line x1="12" y1="16" x2="12.01" y2="16"/>'),
}
const pmodeItems = [
  { value: 'suggest', label: '逐条批准', desc: '编辑与命令逐条批准', icon: PMODE_ICONS.suggest },
  { value: 'auto-edit', label: '自动编辑', desc: '编辑免批、命令逐条批准', icon: PMODE_ICONS['auto-edit'] },
  { value: 'full-auto', label: '完全访问', desc: '减少确认次数，沙箱把关', icon: PMODE_ICONS['full-auto'] },
]
const pmIcon = computed(() => PMODE_ICONS[S.pmode])
// 模型选择器：全部「已启用供应商 × 启用模型」按供应商分组（设置过的 api 只要
// 打开都可切）。项值编码 {provider, model}（JSON 串做键——模型 id 跨供应商可重名，
// 与设置面子 agent 模型引用同一模式）；选中项经 onPickModel 解析后跨供应商热切。
const modelItems = computed(() => {
  const out = []
  for (const p of S.providers) {
    if (p.enabled === false) continue
    for (const m of (p.models || []).filter(m => m.enabled !== false))
      out.push({ value: JSON.stringify({ provider: p.name, model: m.id }),
                 label: m.id, sub: fmtCtx(m.context), group: p.name })
  }
  // 当前选择不在清单：无可用供应商（S.current 空）⇒ 显示「未选择模型」；
  // 其余兜底显示当前模型名（防按钮露出原始 JSON 键）
  const key = modelKey.value
  if (!out.some(i => i.value === key))
    out.push({ value: key, label: S.current && S.model ? S.model : '未选择模型' })
  return out
})
const modelKey = computed(() => JSON.stringify({ provider: S.current, model: S.model }))
function onPickModel(v) {
  try {
    const { provider, model } = JSON.parse(v)
    setModel(model, provider)
  } catch { setModel(v) }   // 非编码值（理论不可达）：退回纯模型切换
}
const ringPct = computed(() => {
  const size = ctxSizeOf(S.model)
  return Math.min(100, S.lastPrompt / size * 100)
})
const ringTitle = computed(() => {
  const size = ctxSizeOf(S.model)
  return '上下文 ' + S.lastPrompt.toLocaleString() + ' / ' + size.toLocaleString() +
         '（占比 ' + ringPct.value.toFixed(1) + '%）'
})
// ---- 上下文构成卡（悬停小环出现；方案 D 四段：系统/用户/回复/工具）-----------
// 数据 = /api/context/breakdown：后端读活跃会话 state.json 按角色分桶估算，
// 实测总量（last_prompt_tokens）存在时按占比等比修正——合计永远准，分段标「估算」。
const ctxCard = ref(null)
let ctxSeq = 0   // 悬停竞态守卫：离开后的迟到响应当弃
async function ctxHover(on) {
  if (!on) { ctxCard.value = null; return }
  const seq = ++ctxSeq
  const r = await fetchCtxBreakdown()
  if (seq === ctxSeq && r.ok) ctxCard.value = r
}
const SEG_META = [
  { key: 'sys', label: '系统提示' },
  { key: 'user', label: '用户消息' },
  { key: 'asst', label: '模型回复' },
  { key: 'tool', label: '工具调用' },
]
// 卡内 token 值统一走 fmtK（≥1000 k 化、≥1e6 M 化；窗口值走 fmtCtx）
const fmt = fmtK
const ctxPct = computed(() => {
  const w = ctxSizeOf(S.model)
  return ctxCard.value && w ? ctxCard.value.total / w * 100 : 0
})
const ctxHot = computed(() => ctxPct.value > 80)   // ≥80% 转红（与环的阈值同源）
// 分段条按「总窗口」渲染（含未使用段）：各段 = 该部分 token / 窗口。1M 级窗口
// 低用量时段会很细——这就是真实余量；占比数字在逐行明细里读。
const ctxWin = computed(() => ctxSizeOf(S.model))
const segWidths = computed(() => {
  if (!ctxCard.value || !ctxWin.value) return []
  return SEG_META.map(s => Math.min(100, ctxCard.value.segs[s.key] / ctxWin.value * 100))
})
const freePct = computed(() => Math.max(0, 100 - segWidths.value.reduce((a, b) => a + b, 0)))
function segCount(key) {
  const c = (ctxCard.value && ctxCard.value.counts) || {}
  if (key === 'tool') {
    const parts = []
    if (c.calls) parts.push(c.calls + ' 次')
    if (c.tools) parts.push(c.tools + ' 条')
    return parts.join(' · ')
  }
  return c[key] ? c[key] + ' 条' : ''
}
const submitting = ref(false)   // 防连点（仅提交窗口；turn 排队由后端管）
const inEl = ref(null)          // 自动增高：初始 1 行，随内容长高，5 行封顶内部滚动
function autoGrow() {
  const el = inEl.value
  if (!el) return
  el.style.height = '26px'   // 先归到基线再量内容（height:auto 对空 textarea 量不准）
  el.style.height = Math.min(el.scrollHeight, 108) + 'px'
}
watch(() => S.inputText, () => nextTick(autoGrow))
async function onSend() {
  if (!S.inputText.trim() || submitting.value) return
  submitting.value = true
  liveStart()
  try { await send(S.inputText) } finally { submitting.value = false }
}
// 生成中发送键即停止键（decide 4 = 取消当前 turn；挂起条取消同款——级联
// 取消在途子代理/后台任务）
async function onStop() {
  if (submitting.value) return
  await decide(4)
}
function onKey(e) {
  if (e.key === 'Enter' && !e.shiftKey && !S.pending && S.inputText.trim()) { onSend(); e.preventDefault() }
}
// ---- 决策坞（ZCode 式裁决）---------------------------------------------------
const multi = computed(() => S.pending && (S.pending._files || []).length >= 2)
async function approve() {
  if (!S.pending) return
  const p = S.pending
  const paths = (p._files || []).length >= 2 ? p._files.filter(x => p._checked[x]) : null
  await decide(0, paths)
}
async function approveAll() {
  if (S.pending) await decide(2)
}
async function deny() {
  if (!S.pending) return
  const msg = S.inputText.trim() || null   // 输入框文字 = 拒绝反馈
  S.inputText = ''
  await decide(1, null, msg)
}
function onWinKey(e) {
  if (!S.pending) return
  if (e.key === 'Enter' && !e.shiftKey) { approve(); e.preventDefault() }
  else if (e.key === 'Escape') { deny(); e.preventDefault() }
}
onMounted(() => window.addEventListener('keydown', onWinKey))
onBeforeUnmount(() => window.removeEventListener('keydown', onWinKey))
</script>

<template>
  <div class="ibwrap">
    <div class="inbox">
    <div v-if="S.pending" class="ddock">
      <div class="ddock-head">
        <span class="shield">🛡</span>
        <b>{{ S.pending.tool || 'workspace.apply' }}</b>
        <span class="ddock-note">待你裁决——批准后才会执行</span>
        <span v-if="multi" class="ddock-note">{{ (S.pending._files || []).filter(f => S.pending._checked[f]).length
          }}/{{ S.pending._files.length }} 个文件已勾选</span>
      </div>
      <div class="ddock-ops">
        <button class="y" type="button" @click="approve">{{ multi ? '批准所选 (Enter)' : '批准 (Enter)' }}</button>
        <button class="a" type="button" @click="approveAll">本轮免批</button>
        <button class="n" type="button" @click="deny">拒绝 (Esc)</button>
      </div>
    </div>
    <textarea ref="inEl" class="in" v-model="S.inputText" @keydown="onKey" @input="autoGrow"
              :placeholder="S.pending ? '拒绝反馈（可选）——Enter 批准 / Esc 拒绝并反馈' : '提出后续修改要求'"></textarea>
    <div class="inbox-ctl">
      <div class="ctl-l">
        <button class="plus-btn" title="新建会话" @click="newSession()">+</button>
        <span class="pm" title="权限模式：suggest=编辑与命令逐条批准；auto-edit=编辑免批、命令逐条批准；full-auto=全自动">
          <Cdl :items="pmodeItems" v-model="S.pmode" :amber="true" :lead-icon="pmIcon" @select="setMode" />
        </span>
      </div>
      <div class="ctl-r">
        <span class="ctxhost" @mouseenter="ctxHover(true)" @mouseleave="ctxHover(false)">
          <span id="ctxRing" :title="ctxCard ? null : ringTitle">
            <svg width="14" height="14" viewBox="0 0 36 36">
              <circle cx="18" cy="18" r="15.9" fill="none" stroke="var(--line)" stroke-width="4"/>
              <circle cx="18" cy="18" r="15.9" fill="none" stroke="currentColor" stroke-width="4"
                      pathLength="100" stroke-dasharray="100"
                      :stroke-dashoffset="100 - ringPct" stroke-linecap="round"
                      :stroke="ringPct > 80 ? 'var(--red)' : 'var(--dim)'"
                      transform="rotate(-90 18 18)"/>
            </svg>
          </span>
          <div v-if="ctxCard" class="ctx-card" :class="{ hot: ctxHot }">
            <div class="tot">
              <span class="n">{{ fmt(ctxCard.total) }}</span>
              <span class="w">/ {{ fmtCtx(ctxSizeOf(S.model)) }} tok</span>
              <span class="p">已用 <b>{{ ctxPct.toFixed(1) }}%</b></span>
            </div>
            <div class="segbar">
              <i v-for="(s, si) in SEG_META" :key="s.key" :class="'seg-' + s.key"
                 :style="{ width: segWidths[si] + '%' }"></i>
              <i class="seg-free" :style="{ width: freePct + '%' }"></i>
            </div>
            <div class="rows">
              <div v-for="s in SEG_META" :key="s.key" class="row">
                <span class="sw" :class="'seg-' + s.key"></span>
                <span class="lab">{{ s.label }}<small v-if="segCount(s.key)">{{ segCount(s.key) }}</small></span>
                <span class="tok">{{ fmt(ctxCard.segs[s.key]) }}</span>
                <span class="pc">{{ (ctxCard.segs[s.key] / Math.max(1, ctxCard.total) * 100).toFixed(1) }}%</span>
              </div>
            </div>
            <div class="foot">
              <span><span class="est">分段估算</span><template v-if="!ctxCard.measured">（无实测回报，总量同为估算）</template></span>
              <span>余量 <b :class="{ red: ctxHot }">{{ fmt(Math.max(0, ctxSizeOf(S.model) - ctxCard.total)) }} tok</b></span>
            </div>
          </div>
        </span>
        <Cdl :items="modelItems" :model-value="modelKey" :align-right="true" @select="onPickModel" />
        <button class="send-btn" :class="{ stop: S.inFlight }" type="button"
                :title="S.inFlight ? '停止生成' : '发送'" @click="S.inFlight ? onStop() : onSend()">
          <svg v-if="S.inFlight" width="12" height="12" viewBox="0 0 24 24">
            <rect x="5" y="5" width="14" height="14" rx="2" fill="currentColor"/>
          </svg>
          <svg v-else width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"><path d="M12 19V5M5 12l7-7 7 7"/></svg>
        </button>
      </div>
    </div>
    </div>
    <div class="rate">输出速率 <b>{{ S.rate }}</b> tok/s</div>
  </div>
</template>
