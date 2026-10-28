<script setup>
// 右侧栏 · 文件树：当前会话工作区的只读目录视图（浮卡内，方案「图标语义」）。
// 数据源 = /api/fs/list（webui 代理 workspace 的 list 工具，端点钉 flat 逐文件列名；
// 黑名单已在后端过滤，越界路径由 jail 拒绝）。懒加载：目录展开才拉一层；刷新 =
// S.fsRev 时钟（store 在 applied/undone 时 +1，agent 改完盘文件树自动跟上）。
// 行观感：类型图标着色（代码青/文档琥珀/图片青蓝/文本灰）、文件尺寸右对齐、
// 祖先引导线与选中高亮——全部纯展示态，不改盘不发起写。
import { ref, reactive, computed, watch } from 'vue'
import { S, fetchFsList } from '../store'

const root = reactive({ children: [], truncated: false, error: '', loading: false })
const expanded = reactive({})   // 目录路径 → {children, truncated, error, loading}
const selPath = ref('')         // 当前点开的文件（查看器打开时高亮来源）

const emit = defineEmits(['open-file'])

function slotOf(path) {
  return { children: [], truncated: false, error: '', loading: false }
}

async function fetchDir(path, slot) {
  slot.loading = true
  try {
    const r = await fetchFsList(path)
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
  if (e.type !== 'dir') { selPath.value = e.path; emit('open-file', e.path); return }
  if (expanded[e.path]) { delete expanded[e.path]; saveExpanded(); return }
  const slot = reactive(slotOf())
  expanded[e.path] = slot
  saveExpanded()
  await fetchDir(e.path, slot)
}

// 展开目录记忆：重启/刷新后沿用上次展开到哪层。按工作区隔离——换工作区不沿用
// （路径不属于这个 jail，展开只会拉一堆错误）。折叠态本身由 App.vue 的 vcFiles 管。
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
  selPath.value = ''
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

// 类型图标（自绘 24×24 描边，随 currentColor 着色；不用图标库——不引新依赖）。
// 代码族青、文档族琥珀、图片青蓝、日志次灰、文本灰；目录恒用文件夹形。
const ICON_FILE = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M6.5 3.5h6.6l5 5V19a1.6 1.6 0 0 1-1.6 1.6h-10A1.6 1.6 0 0 1 4.9 19V5.1a1.6 1.6 0 0 1 1.6-1.6z"/><path d="M13.1 3.5v5.1h5"/></svg>'
const ICON_CODE = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M6.5 3.5h6.6l5 5V19a1.6 1.6 0 0 1-1.6 1.6h-10A1.6 1.6 0 0 1 4.9 19V5.1a1.6 1.6 0 0 1 1.6-1.6z"/><path d="M13.1 3.5v5.1h5"/><path d="m10 12.4-2.1 2.1 2.1 2.1"/><path d="m14 12.4 2.1 2.1-2.1 2.1"/></svg>'
const ICON_TEXT = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M6.5 3.5h6.6l5 5V19a1.6 1.6 0 0 1-1.6 1.6h-10A1.6 1.6 0 0 1 4.9 19V5.1a1.6 1.6 0 0 1 1.6-1.6z"/><path d="M13.1 3.5v5.1h5"/><path d="M9 13.6h6"/><path d="M9 16.6h6"/></svg>'
const ICON_PRES = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="4" y="4.5" width="16" height="11.5" rx="1.6"/><path d="M12 16v2.2"/><path d="m8.5 20.5 3.5-2.4 3.5 2.4"/></svg>'
const ICON_IMAGE = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><rect x="4" y="5" width="16" height="14" rx="1.8"/><circle cx="9.2" cy="10" r="1.5"/><path d="m6.5 16.5 3.2-3.6 2.4 2.7 2-2.3 3.4 3.8"/></svg>'
const ICON_FOLDER = '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3.5 7.2A1.7 1.7 0 0 1 5.2 5.5h3.9a1.7 1.7 0 0 1 1.36.68l1.04 1.4h7A1.7 1.7 0 0 1 20.2 9.3v7a1.7 1.7 0 0 1-1.7 1.7H5.2a1.7 1.7 0 0 1-1.7-1.7z"/></svg>'

function iconFor(name) {
  const ext = name.slice(name.lastIndexOf('.') + 1).toLowerCase()
  if (['png', 'jpg', 'jpeg', 'gif', 'bmp', 'webp', 'ico', 'svg'].includes(ext))
    return { c: 'c-cyan', s: ICON_IMAGE }
  if (ext === 'pptx') return { c: 'c-amber', s: ICON_PRES }
  if (['docx', 'xlsx', 'pdf'].includes(ext)) return { c: 'c-amber', s: ICON_TEXT }
  if (['py', 'js', 'mjs', 'ts', 'c', 'cc', 'cpp', 'h', 'hpp', 'css', 'json', 'toml', 'yaml', 'yml', 'sh', 'bat', 'cmd', 'ps1'].includes(ext))
    return { c: 'c-teal', s: ICON_CODE }
  if (ext === 'log') return { c: 'c-dim2', s: ICON_TEXT }
  return { c: 'c-dim', s: ext === 'txt' || ext === 'md' ? ICON_TEXT : ICON_FILE }
}

defineExpose({ refresh, rootCount: computed(() => root.children.length) })
</script>

<template>
  <div class="ftree">
    <div v-if="!S.curWs" class="ft-empty">选择工作文件夹后展示文件树</div>
    <template v-else>
      <div v-if="root.loading" class="ft-empty">读取中…</div>
      <div v-else-if="root.error" class="ft-empty">{{ root.error }}</div>
      <div v-else-if="!rows.length" class="ft-empty">（空目录）</div>
      <template v-else>
        <div v-for="r in rows" :key="r.path" class="ft-row"
             :class="{ dir: r.type === 'dir', open: expanded[r.path],
                       sel: r.type === 'file' && selPath === r.path }"
             :style="{ '--d': r.depth }"
             :title="r.type === 'file' ? nameOf(r.path) + (r.size ? ' · ' + fmtSize(r.size) : '') : ''"
             @click="toggle(r)">
          <span class="ft-chev" :class="{ ghost: r.type !== 'dir' }"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round"><polyline points="9 7.5 14 12 9 16.5"/></svg></span>
          <span v-if="r.type === 'dir'" class="ft-ic dir" v-html="ICON_FOLDER"></span>
          <span v-else class="ft-ic" :class="iconFor(r.path).c" v-html="iconFor(r.path).s"></span>
          <span class="ft-name">{{ nameOf(r.path) }}</span>
          <span v-if="r.type === 'file'" class="ft-meta">{{ fmtSize(r.size) }}</span>
          <span v-if="expanded[r.path] && expanded[r.path].error" class="ft-err">{{ expanded[r.path].error }}</span>
        </div>
        <div v-if="root.truncated" class="ft-note">条目过多，仅显示部分</div>
      </template>
    </template>
  </div>
</template>
