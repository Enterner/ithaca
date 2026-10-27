<script setup>
// 输入栏：textarea + 权限模式/模型自绘下拉 + 上下文环 + 发送/停止。
// hero（无工作区）与 dock 共用本组件（输入内容在 store，切换不丢）。
// 有待裁决提案 ⇒ 决策坞（ZCode 式）：Enter 批准（所选文件）/ 本轮免批 /
// Esc 拒绝——输入框里的文字随拒绝作为反馈回灌给 agent。
import { computed, ref, watch, nextTick, onMounted, onBeforeUnmount } from 'vue'
import { S, send, decide, setMode, setModel, newSession, pickFolder,
         ctxSizeOf, fmtCtx, liveStart, fetchCtxBreakdown, fmtK, api, pushLocal } from '../store'
import Cdl from './Cdl.vue'

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
// ---- 会话用量与输出速率（卡内新增块）------------------------------------------
// 用量源 = transcript 里 assistant 记录自带的 usage（每轮回报；provider 未回报时
// 形状为空对象 ⇒ 全 0，重放同样可重建）。输入/输出 = 会话累计；缓存命中 = 累计
// 命中 tokens 及占累计输入的比例（缺失时整段不显示）。速率读 S.rate——records
// 的滚动窗口维护（含静默期归零），此处纯展示。
const usage = computed(() => {
  let prompt = 0, completion = 0, cache = 0
  for (const r of S.records)
    if (r.type === 'assistant' && r.usage && typeof r.usage.total_tokens === 'number') {
      prompt += r.usage.prompt_tokens || 0
      completion += r.usage.completion_tokens || 0
      cache += r.usage.cache_tokens || 0
    }
  return { prompt, completion, cache }
})
const cachePct = computed(() =>
  usage.value.prompt > 0 ? Math.round(usage.value.cache / usage.value.prompt * 100) : 0)
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
// ---- ＋菜单（会话动作 / 附件上传 / 主动压缩）与拖拽上传 -----------------------
// 附件 = 文件落盘到工作区 .ithaca/uploads（/api/upload），发送时在消息尾带
// 相对路径——模型据此用 read 工具读取（文档类读取见 workspace read 的 doc 面）。
const fileEl = ref(null)
const dropHot = ref(false)
const plusVal = ref('')   // 恒空：动作菜单不做选中态，重复点同一项仍触发
const plusItems = [
  { value: 'upload', label: '上传文件', desc: '附加到工作区，随消息发给 agent',
    icon: ico('<path d="m21.44 11.05-9.19 9.19a6 6 0 0 1-8.49-8.49l8.57-8.57A4 4 0 1 1 18 8.84l-8.59 8.57a2 2 0 0 1-2.83-2.83l8.49-8.48"/>') },
  { value: 'compress', label: '压缩上下文', desc: '立即折叠历史（不等自动阈值）',
    icon: ico('<polyline points="4 14 10 14 10 20"/><polyline points="20 10 14 10 14 4"/><line x1="14" y1="10" x2="21" y2="3"/><line x1="3" y1="21" x2="10" y2="14"/>') },
  { value: 'new', label: '新建会话', icon: ico('<rect x="3" y="3" width="18" height="18" rx="4"/><path d="M12 8v8M8 12h8"/>') },
]
function onMenu(v) {
  if (v === 'new') newSession()
  else if (v === 'upload') fileEl.value && fileEl.value.click()
  else if (v === 'compress') api('/api/session/cmd', { cmd: 1 })   // 结果走 agent 通告回流
}
const MAX_UPLOAD = 20 * 1024 * 1024
async function uploadFiles(files) {
  for (const f of files) {
    if (S.attachments.some(a => a.name === f.name)) continue   // 同名已在列：跳过
    if (f.size > MAX_UPLOAD) { pushLocal('「' + f.name + '」超过 20MB 上传上限'); continue }
    try {
      const data = await new Promise((res, rej) => {
        const r = new FileReader()
        r.onload = () => res(String(r.result).replace(/^data:[^,]*,/, ''))
        r.onerror = () => rej(r.error)
        r.readAsDataURL(f)
      })
      const r = await api('/api/upload', { name: f.name, data })
      if (r.ok) S.attachments.push({ path: r.path, name: f.name })
      else pushLocal('上传失败：「' + f.name + '」' + (r.error || ''))
    } catch (e) {
      pushLocal('上传失败：「' + f.name + '」读取文件出错')
    }
  }
}
function onFilesChosen(e) {
  const files = [...(e.target.files || [])]
  e.target.value = ''   // 允许重复选择同一文件
  if (files.length) uploadFiles(files)
}
function onDragOver(e) { e.preventDefault(); dropHot.value = true }
function onDragLeave(e) {
  // 子元素间过渡会触发 dragleave——只有真离开容器才熄灭高亮
  const to = e.relatedTarget
  if (!to || !e.currentTarget.contains(to)) dropHot.value = false
}
function onDrop(e) {
  e.preventDefault()
  dropHot.value = false
  const files = [...(e.dataTransfer?.files || [])]
  if (files.length) uploadFiles(files)
}
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
  <div class="ibwrap" @dragover="onDragOver" @dragleave="onDragLeave" @drop="onDrop">
    <div class="inbox" :class="{ drop: dropHot }">
    <div v-if="dropHot" class="drophint">松开以附加文件（将存入工作区 .ithaca/uploads）</div>
    <div v-if="S.attachments.length" class="atchips">
      <span v-for="(a, i) in S.attachments" :key="a.path" class="atchip" :title="a.path">
        <span class="nm">📄 {{ a.name }}</span>
        <span class="x" title="移除（文件保留在工作区）" @click="S.attachments.splice(i, 1)">×</span>
      </span>
    </div>
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
        <span class="plusmenu" title="会话动作：上传附件 / 压缩上下文 / 新建会话">
          <Cdl :items="plusItems" v-model="plusVal" placeholder="+" @select="onMenu" />
        </span>
        <span class="pm" title="权限模式：suggest=编辑与命令逐条批准；auto-edit=编辑免批、命令逐条批准；full-auto=全自动">
          <Cdl :items="pmodeItems" v-model="S.pmode" :amber="true" :lead-icon="pmIcon" @select="setMode" />
        </span>
      </div>
      <input ref="fileEl" type="file" multiple style="display:none" @change="onFilesChosen">
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
            <!-- 会话用量（输入含缓存命中 / 输出）+ 输出速率：原左下角速率行与
                 侧栏「累计 tokens」的收编处——计量语境都在上下文卡里 -->
            <div class="usage">
              <div class="urow">
                <span class="ulab">输入</span><b class="un">{{ fmt(usage.prompt) }}</b>
                <small v-if="usage.cache" class="uextra">缓存命中 {{ fmt(usage.cache) }}（{{ cachePct }}%）</small>
              </div>
              <div class="urow">
                <span class="ulab">输出</span><b class="un">{{ fmt(usage.completion) }}</b>
                <small class="uextra">速率 {{ S.rate }} tok/s</small>
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
  </div>
</template>
