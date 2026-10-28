<script setup>
// 会话栏：品牌 + 新会话 + 工作区分组列表（当前组排最前、可折叠/重命名/删除/组内新建）
import { ref, computed, watch } from 'vue'
import { S, openSession, newSession, delSession, renameSession, loadSessions } from '../store'
import { confirmDialog, promptDialog } from '../confirm'

// 徽标刷新：working/pending 由后端 /api/sessions 按实例状态返回，而 turn 开始/
// 收口、提案弹出/裁决只改前端 inFlight/pending——watch 这两个信号拉一次清单，
// 侧栏 ⟳/● 才能跟上（壳模式另有 3s 轮询，这里补非壳模式的刷新）
watch(() => [S.inFlight, S.pending], () => { loadSessions() })

function collapseSet() {
  try { return new Set(JSON.parse(localStorage.getItem('vcCollapsed') || '[]')) }
  catch (e) { return new Set() }
}
const collapsed = ref(collapseSet())
function saveCollapsed() {
  localStorage.setItem('vcCollapsed', JSON.stringify([...collapsed.value]))
}
const groups = computed(() => {
  // 分组顺序稳定 = 注册表顺序（不随当前会话动态重排——刷新时组跳到顶上很难受）
  const g = new Map()
  for (const s of S.sessions) {
    const ws = s.workspace || '(未设置)'
    if (!g.has(ws)) g.set(ws, [])
    g.get(ws).push(s)
  }
  return [...g.keys()].map(ws => ({ ws, sessions: g.get(ws) }))
})
function toggleGroup(ws) {
  if (collapsed.value.has(ws)) collapsed.value.delete(ws)
  else collapsed.value.add(ws)
  saveCollapsed()
}
function short(p) { return p.split(/[\\/]/).pop() || p }
function fmtTime(ts) {
  const d = new Date((ts || 0) * 1000)
  return isNaN(d) ? '' : (d.getMonth() + 1) + '-' + d.getDate() + ' ' +
    String(d.getHours()).padStart(2, '0') + ':' + String(d.getMinutes()).padStart(2, '0')
}
async function onRename(s) {
  const t = await promptDialog({
    title: '重命名会话',
    initial: s.title || '',
    placeholder: '新的会话名称',
    positive: '重命名',
  })
  if (t && t !== (s.title || '')) renameSession(s.id, t)
}
async function onDelete(s) {
  const ok = await confirmDialog({
    title: '删除会话',
    content: '「' + (s.title || s.id) + '」将被永久删除，不可恢复。',
    positive: '删除',
  })
  if (ok) delSession(s.id)
}
</script>

<template>
  <div class="sess-list">
    <div v-for="g in groups" :key="g.ws" class="wsGroup">
      <div class="wsHead" :title="g.ws" @click="toggleGroup(g.ws)">
        <span class="chev">{{ collapsed.has(g.ws) ? '▸' : '▾' }}</span> {{ g.ws }}
        <span class="add" title="在此工作区新建会话" @click.stop="newSession(g.ws === '(未设置)' ? '' : g.ws)">＋</span>
      </div>
      <div v-show="!collapsed.has(g.ws)" class="wsBody">
        <div v-for="s in g.sessions" :key="s.id" class="sess" :class="{ active: s.id === S.curSession }"
             @click="openSession(s.id)">
          <div class="t">{{ s.title || '(未命名)' }}<span v-if="s.pending" class="badge pend">●</span><span v-else-if="s.working" class="badge work">⟳</span></div>
          <div class="m">{{ fmtTime(s.updated) }}{{ s.turns != null ? '，' + (s.turns || 0) + ' 轮' : '' }}{{ s.alive === false ? ' · 已停止' : '' }}</div>
          <span class="ren" title="重命名" @click.stop="onRename(s)">✎</span>
          <span class="del" title="删除" @click.stop="onDelete(s)">×</span>
        </div>
      </div>
    </div>
  </div>
</template>
