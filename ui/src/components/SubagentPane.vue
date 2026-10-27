<script setup>
// 子代理过程视图（右栏）：选中子会话槽（S.subView）的工作过程。明细的持久化
// 在服务端独立存档（sessions/agents/…，不经主会话记录流）——打开时经
// /api/agent-log 拉取历史，与 SSE 实时推送（subagent_detail 缓冲）按 id
// （turn 内单调序号）去重合并；老会话无存档时回退主 transcript 里的历史明细行。
// 流式增量（subagent_delta）照旧只缓冲不落盘。
// 取消 = 级联取消在途子代理与当前 turn（decide 4），与主会话停止键同一路径。
import { ref, computed, watch, nextTick } from 'vue'
import { S, decide, fetchAgentLog } from '../store'
import { esc, toolLabel } from '../md'

// 明细行显示（与主对话流同款纪律）：标签为空 = 摘要自含中文动词
function line(r) {
  const label = toolLabel(r.tool, r.text)
  return label ? label + ' ' + r.text : (r.text || '')
}

const bodyEl = ref(null)
const history = ref([])   // 存档明细（打开时一次拉取）

// 老会话兜底：无独立存档（明细还留在主 transcript 的旧格式）时回退记录流
const legacy = computed(() =>
  S.records.filter((r) => r.type === 'subagent' && r.turn_id === S.subView &&
                       (r.phase === 2 || r.phase === 3)))

const recs = computed(() => {
  const live = S.subDetail[S.subView] || []
  if (history.value.length === 0 && live.length === 0) return legacy.value
  const seen = new Set(history.value.map((r) => r.id))
  return [...history.value, ...live.filter((r) => !seen.has(r.id))]
})
const spawn = computed(() =>
  S.records.find((r) => r.type === 'subagent' && r.phase === 0 && r.turn_id === S.subView))
const live = computed(() => S.subLive[S.subView] || '')
const running = computed(() => !S.subDone[S.subView])

watch([() => recs.value.length, live], () =>
  nextTick(() => { if (bodyEl.value) bodyEl.value.scrollTop = bodyEl.value.scrollHeight }))

// 打开（或切换选中槽）时拉一次存档；之后过程行全靠实时推送续
watch(() => S.subView, async (turn) => {
  history.value = []
  if (!turn) return
  try {
    const d = await fetchAgentLog(turn)
    if (turn === S.subView && Array.isArray(d.records)) history.value = d.records
  } catch { /* 拉取失败 = 只显示实时增量 */ }
}, { immediate: true })

async function onCancel() { await decide(4) }
</script>

<template>
  <div class="subpane">
    <div class="subpane-head">
      <b>🤖 子代理[{{ spawn?.tool || '?' }}]</b>
      <span class="spacer"></span>
      <button v-if="running" class="wcancel" type="button" @click="onCancel">取消</button>
      <button class="fs-btn" title="关闭过程视图" @click="S.subView = 0">✕</button>
    </div>
    <div v-if="spawn" class="subpane-desc">{{ spawn.text }}</div>
    <div ref="bodyEl" class="subpane-body">
      <template v-for="(r, i) in recs" :key="i">
        <div v-if="r.phase === 2" class="toolline"
             v-html="'⚙ ' + esc(line(r))"></div>
        <div v-else-if="r.phase === 3" class="toolline"
             :class="{ warn: (r.text || '').startsWith('✗') }">⚙ {{ line(r) }}</div>
      </template>
      <div v-if="live" class="msg a">{{ live }}<span class="cursor"></span></div>
      <div v-if="!running && !live" class="msg" style="color:var(--dim)">— 子代理已结束 —</div>
    </div>
  </div>
</template>
