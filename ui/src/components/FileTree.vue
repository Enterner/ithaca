<script setup>
// 右侧栏 · 文件树：当前会话工作区的只读目录视图（docs/文件树右侧栏-方案.md）。
// 数据源 = /api/fs/list（webui 代理 workspace 的 list 工具；黑名单已在后端过滤，
// 越界路径由 jail 拒绝）。懒加载：目录展开才拉一层；刷新 = S.fsRev 时钟
// （store 在 applied/undone 时 +1，agent 改完盘文件树自动跟上）+ 头部手动按钮。
import { ref, reactive, computed, watch } from 'vue'
import { S, api } from '../store'

const root = reactive({ children: [], truncated: false, error: '', loading: false })
const expanded = reactive({})   // 目录路径 → {children, truncated, error, loading}

const emit = defineEmits(['open-file'])   // 文件点击交父层开查看器（树不持有查看态）

function slotOf(path) {
  return { children: [], truncated: false, error: '', loading: false }
}

async function fetchDir(path, slot) {
  slot.loading = true
  try {
    const r = await api('/api/fs/list?path=' + encodeURIComponent(path) + '&depth=1')
    if (!r.ok) { slot.error = r.error || '读取失败'; slot.children = [] }
    else {
      slot.children = [...(r.entries || [])].sort((a, b) => {
        const d = (a.type === 'dir' ? 0 : 1) - (b.type === 'dir' ? 0 : 1)
        return d || a.path.toLowerCase().localeCompare(b.path.toLowerCase())
      })
      slot.truncated = !!r.truncated
      slot.error = ''
    }
  } catch { slot.error = '网络错误' }
  slot.loading = false
}

async function toggle(e) {
  if (e.type !== 'dir') { emit('open-file', e.path); return }
  if (expanded[e.path]) { delete expanded[e.path]; saveExpanded(); return }
  const slot = reactive(slotOf())
  expanded[e.path] = slot
  saveExpanded()
  await fetchDir(e.path, slot)
}

// 展开目录记忆：重启/刷新后沿用上次展开到哪层。按工作区隔离——换工作区不沿用
// （路径不属于这个 jail，展开只会拉一堆错误）。折叠态本身由 App.vue 的 vcFiles/vcSidebar 管。
function loadExpanded() {
  try {
    const saved = JSON.parse(localStorage.getItem('vcExpanded') || 'null')
    if (saved && saved.ws === S.curWs) return saved.paths || []
  } catch (e) {}
  return []
}
function saveExpanded() {
  localStorage.setItem('vcExpanded', JSON.stringify({ ws: S.curWs, paths: Object.keys(expanded) }))
}

async function refresh() {
  if (!S.curWs) return
  await Promise.all([fetchDir('', root),
                     ...Object.keys(expanded).map((p) => fetchDir(p, expanded[p]))])
}

function reset() {
  for (const k of Object.keys(expanded)) delete expanded[k]
  root.children = []; root.error = ''; root.truncated = false
  if (S.curWs) {
    // 根就绪后恢复上次展开的目录（逐个重拉；目录已消失的会显示加载错误，不致命）
    fetchDir('', root).then(() => {
      for (const p of loadExpanded()) {
        const slot = reactive(slotOf())
        expanded[p] = slot
        fetchDir(p, slot)
      }
    })
  }
}

watch(() => S.curWs, reset, { immediate: true })
// fsRev 跳变（applied/undone）⇒ 已展开目录静默重拉；根为空（还没展开过）不扰动
watch(() => S.fsRev, () => {
  if (S.curWs && (root.children.length || Object.keys(expanded).length)) refresh()
})

const rows = computed(() => {
  const out = []
  const walk = (entries, depth) => {
    for (const e of entries) {
      out.push({ ...e, depth })
      const slot = expanded[e.path]
      if (e.type === 'dir' && slot && slot.children.length) walk(slot.children, depth + 1)
    }
  }
  walk(root.children, 0)
  return out
})

const nameOf = (p) => p.split('/').pop()
function fmtSize(n) {
  if (!n && n !== 0) return ''
  if (n >= 1048576) return (n / 1048576).toFixed(1) + ' MB'
  if (n >= 1024) return (n / 1024).toFixed(1) + ' KB'
  return n + ' B'
}

defineExpose({ refresh })
</script>

<template>
  <div class="ftree">
    <div v-if="!S.curWs" class="ft-empty">选择工作文件夹后展示文件树</div>
    <template v-else>
      <div v-if="root.loading" class="ft-empty">读取中…</div>
      <div v-else-if="root.error" class="ft-empty">{{ root.error }}</div>
      <div v-else-if="!rows.length" class="ft-empty">（空目录）</div>
      <template v-else>
        <div v-for="r in rows" :key="r.path" class="ft-row" :class="{ dir: r.type === 'dir' }"
             :style="{ paddingLeft: 6 + r.depth * 14 + 'px' }"
             :title="r.type === 'file' ? nameOf(r.path) + (r.size ? ' · ' + fmtSize(r.size) : '') : ''"
             @click="toggle(r)">
          <span class="ft-chev">{{ r.type === 'dir' ? (expanded[r.path] ? '▾' : '▸') : '' }}</span>
          <span class="ft-name" :class="{ open: r.type === 'dir' && expanded[r.path] }">{{ nameOf(r.path) }}</span>
          <span v-if="expanded[r.path] && expanded[r.path].error" class="ft-err">{{ expanded[r.path].error }}</span>
        </div>
        <div v-if="root.truncated" class="ft-note">条目过多，仅显示部分</div>
      </template>
    </template>
  </div>
</template>
