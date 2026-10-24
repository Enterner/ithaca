<script setup>
// 输入栏：textarea + 权限模式/模型自绘下拉 + 上下文环 + 发送/停止。
// hero（无工作区）与 dock 共用本组件（输入内容在 store，切换不丢）。
// 有待裁决提案 ⇒ 决策坞（ZCode 式）：Enter 批准（所选文件）/ 本轮免批 /
// Esc 拒绝——输入框里的文字随拒绝作为反馈回灌给 agent。
import { computed, ref, watch, nextTick, onMounted, onBeforeUnmount } from 'vue'
import { S, send, decide, setMode, setModel, newSession, pickFolder,
         curProvModels, ctxSizeOf, fmtCtx, liveStart } from '../store'
import Cdl from '../Cdl.vue'

const pmodeItems = [
  { value: 'suggest', label: '逐条批准' },
  { value: 'auto-edit', label: '自动编辑' },
  { value: 'full-auto', label: '完全访问' },
]
const modelItems = computed(() =>
  curProvModels().filter(m => m.enabled !== false)
    .map(m => ({ value: m.id, label: m.id, sub: fmtCtx(m.context) })))
const ringPct = computed(() => {
  const size = ctxSizeOf(S.model)
  return Math.min(100, S.lastPrompt / size * 100)
})
const ringTitle = computed(() => {
  const size = ctxSizeOf(S.model)
  return '上下文 ' + S.lastPrompt.toLocaleString() + ' / ' + size.toLocaleString() +
         '（占比 ' + ringPct.value.toFixed(1) + '%）'
})
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
          <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 22s8-4 8-10V5l-8-3-8 3v7c0 6 8 10 8 10z"/></svg>
          <Cdl :items="pmodeItems" v-model="S.pmode" :amber="true" @select="setMode" />
        </span>
      </div>
      <div class="ctl-r">
        <span id="ctxRing" :title="ringTitle">
          <svg width="14" height="14" viewBox="0 0 36 36">
            <circle cx="18" cy="18" r="15.9" fill="none" stroke="var(--line)" stroke-width="4"/>
            <circle cx="18" cy="18" r="15.9" fill="none" stroke="currentColor" stroke-width="4"
                    pathLength="100" stroke-dasharray="100"
                    :stroke-dashoffset="100 - ringPct" stroke-linecap="round"
                    :stroke="ringPct > 80 ? 'var(--red)' : 'var(--dim)'"
                    transform="rotate(-90 18 18)"/>
          </svg>
        </span>
        <Cdl :items="modelItems" v-model="S.model" :align-right="true" @select="setModel" />
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
