<script setup>
// 上下文卡（环 + 悬停构成分解 + 会话用量）：上下文类显示面的统一组件——
// 数据、失效与拉取门禁都收在这一处，InputBar 只管排版。
// 统一更新纪律：
//   · 会话身份任一变化（curSession/draft/curWs）⇒ 构成卡整体失效（下次悬停
//     按新身份现拉）。草稿视图不属于任何服务端会话，悬停不拉取——否则显示
//     的是上一会话的构成（踩过: 草稿下 breakdown 卡显示别的会话，卡片半空
//     半满——usage 随 records 正确隐藏、构成却来自服务端活跃会话）。
//   · 悬停离开同样推进竞态序号——迟到响应不得复活已收起的卡。
//   · 构成数据每次悬停现拉（服务端 mtime 缓存兜底）；会话用量/速率由全局
//     records 派生，随 records 自动对齐，本组件不另设缓存。
import { computed, ref, watch } from 'vue'
import { S, ctxSizeOf, fmtCtx, fetchCtxBreakdown, fmtK } from '../store'

const fmt = fmtK   // 卡内 token 值统一走 fmtK（≥1000 k 化、≥1e6 M 化；窗口值走 fmtCtx）

// ---- 环（实测 last_prompt / 模型窗口）----------------------------------------
const ringPct = computed(() => {
  const size = ctxSizeOf(S.model)
  return Math.min(100, S.lastPrompt / size * 100)
})
const ringTitle = computed(() => {
  const size = ctxSizeOf(S.model)
  return '上下文 ' + S.lastPrompt.toLocaleString() + ' / ' + size.toLocaleString() +
         '（占比 ' + ringPct.value.toFixed(1) + '%）'
})

// ---- 构成分解卡（悬停现拉；方案 D 四段：系统/用户/回复/工具）-----------------
// 数据 = /api/context/breakdown：后端读活跃会话 state.json 按角色分桶估算，
// 实测总量（last_prompt_tokens）存在时按占比等比修正——合计永远准，分段标「估算」。
const ctxCard = ref(null)
let ctxSeq = 0   // 悬停竞态守卫：进入与离开都推进，迟到响应当弃
async function ctxHover(on) {
  if (!on) { ++ctxSeq; ctxCard.value = null; return }
  if (S.draft) return   // 草稿门禁：草稿视图不属于任何服务端会话，不拉构成
  const seq = ++ctxSeq
  const r = await fetchCtxBreakdown()
  if (seq === ctxSeq && r.ok) ctxCard.value = r
}
// 统一失效：会话身份任一变化 ⇒ 构成卡收起（迟到响应一并作废）
watch([() => S.curSession, () => S.draft, () => S.curWs], () => {
  ++ctxSeq
  ctxCard.value = null
})

const SEG_META = [
  { key: 'sys', label: '系统提示' },
  { key: 'user', label: '用户消息' },
  { key: 'asst', label: '模型回复' },
  { key: 'tool', label: '工具调用' },
]
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

// ---- 会话用量（卡内块）--------------------------------------------------------
// 用量源 = transcript 里 assistant 记录自带的 usage（每轮回报；provider 未回报时
// 形状为空对象 ⇒ 全 0，重放同样可重建）。输入/输出 = 会话累计；缓存率 = 逐轮
// （命中/输入）的算术均值——累计比是按 token 量加权的均值，大轮会淹没小轮的
// 表现；cache_tokens 字段缺失的轮 = provider 未计量（非真 0），不计入均值。
// hasUsage 区分「没回报」与「真是 0」：缺失显示「—」而非误导性的 0。
const usage = computed(() => {
  let prompt = 0, completion = 0, cache = 0, seen = false
  let crSum = 0, crN = 0   // 逐轮缓存率的和与轮数（均值分子/分母）
  for (const r of S.records)
    if (r.type === 'assistant' && r.usage && typeof r.usage.total_tokens === 'number') {
      seen = true
      const u = r.usage
      prompt += u.prompt_tokens || 0
      completion += u.completion_tokens || 0
      cache += u.cache_tokens || 0
      if (typeof u.cache_tokens === 'number' && (u.prompt_tokens || 0) > 0) {
        crSum += u.cache_tokens / u.prompt_tokens
        crN++
      }
    }
  return { prompt, completion, cache, seen, crSum, crN }
})
const cachePct = computed(() =>
  usage.value.crN > 0 ? Math.round(usage.value.crSum / usage.value.crN * 100) : 0)
</script>

<template>
  <span class="ctxhost" @mouseenter="ctxHover(true)" @mouseleave="ctxHover(false)">
    <!-- 无实测数据的环不出提示（草稿/新会话下「0 / 0.0%」只有噪音）；
         title 是原生样式，深色 UI 里本就该少用——只在有内容时给 -->
    <span id="ctxRing" :title="ctxCard || !S.lastPrompt ? null : ringTitle">
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
      <!-- 会话用量（输入含缓存命中 / 输出）：原左下角速率行与侧栏「累计
           tokens」的收编处——计量语境都在上下文卡里。provider 未回报 usage
           时显示「—」（区分「没数据」与「真是 0」） -->
      <div class="usage">
        <div class="urow">
          <span class="ulab">输入</span><b class="un">{{ usage.seen ? fmt(usage.prompt) : '—' }}</b>
          <small v-if="usage.cache" class="uextra">缓存命中 {{ cachePct }}%</small>
        </div>
        <div class="urow">
          <span class="ulab">输出</span><b class="un">{{ usage.seen ? fmt(usage.completion) : '—' }}</b>
          <small v-if="S.rate" class="uextra">平均 {{ S.rate }} tok/s</small>
        </div>
      </div>
      <div class="foot">
        <span><span class="est">分段估算</span></span>
        <span>余量 <b :class="{ red: ctxHot }">{{ fmt(Math.max(0, ctxSizeOf(S.model) - ctxCard.total)) }} tok</b></span>
      </div>
    </div>
  </span>
</template>
