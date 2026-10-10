<script setup>
// 布局：会话栏 + 主区（顶栏 / 对话流或 hero / 输入栏）。Naive UI 只做暗色主题基座。
// 会话栏 VSCode 化：拖拽调宽（180–420，localStorage 记忆）、折叠成 44px 图标条
// （头部按钮或 Ctrl+B），全屏/缩小时栏宽不变、内容区自适应。
import { ref, onMounted, onBeforeUnmount, computed } from 'vue'
import { NConfigProvider, NTooltip, darkTheme } from 'naive-ui'
import { S, boot, pickFolder, newSession } from './store'
import Sidebar from './components/Sidebar.vue'
import Stream from './components/Stream.vue'
import InputBar from './components/InputBar.vue'
import Settings from './components/Settings.vue'
import FileTree from './components/FileTree.vue'

const settingsRef = ref(null)
const fileTreeRef = ref(null)
onMounted(() => { boot() })
const shortWs = computed(() => S.curWs ? (S.curWs.split('\\').pop() || S.curWs) : '(未设置)')
const overrides = {
  common: {
    primaryColor: '#6366F1', primaryColorHover: '#7B83F5', primaryColorPressed: '#5257E0',
    bodyColor: '#0B0E15', cardColor: '#12171F', modalColor: '#12171F',
    borderColor: '#222B3B', textColorBase: '#E6EAF2',
  },
  Card: { borderRadius: '14px' },
  Dialog: { borderRadius: '14px' },
}

// ---- 会话栏折叠/调宽（VSCode 式）---------------------------------------------
const sideW = ref(248)
const collapsed = ref(false)
try {
  const saved = JSON.parse(localStorage.getItem('vcSidebar') || '{}')
  if (saved.w >= 180 && saved.w <= 420) sideW.value = saved.w
  collapsed.value = !!saved.collapsed
} catch (e) {}
function persistSide() {
  localStorage.setItem('vcSidebar', JSON.stringify({ w: sideW.value, collapsed: collapsed.value }))
}
function toggleSide() { collapsed.value = !collapsed.value; persistSide() }
function startDrag(e) {   // 拖拽右缘调宽
  e.preventDefault()
  const move = (ev) => { sideW.value = Math.min(420, Math.max(180, ev.clientX)) }
  const up = () => {
    window.removeEventListener('mousemove', move)
    window.removeEventListener('mouseup', up)
    persistSide()
  }
  window.addEventListener('mousemove', move)
  window.addEventListener('mouseup', up)
}
function onWinKey(e) {
  if (e.ctrlKey && !e.shiftKey && !e.altKey && e.key.toLowerCase() === 'b') {
    toggleSide(); e.preventDefault()
  }
}

// ---- 文件树右栏（可折叠/可拖宽，与左栏同构；只读视图）------------------------
const filesW = ref(232)
const filesOpen = ref(false)   // 默认收起（用户拍板）；localStorage 显式存 true 才展开
try {
  const saved = JSON.parse(localStorage.getItem('vcFiles') || '{}')
  if (saved.w >= 200 && saved.w <= 420) filesW.value = saved.w
  filesOpen.value = !!saved.open
} catch (e) {}
function persistFiles() {
  localStorage.setItem('vcFiles', JSON.stringify({ w: filesW.value, open: filesOpen.value }))
}
function toggleFiles() { filesOpen.value = !filesOpen.value; persistFiles() }
function startFilesDrag(e) {   // 拖左缘调宽（栏在右侧：宽 = 窗口宽 - 指针 x）
  e.preventDefault()
  const move = (ev) => { filesW.value = Math.min(420, Math.max(200, window.innerWidth - ev.clientX)) }
  const up = () => {
    window.removeEventListener('mousemove', move)
    window.removeEventListener('mouseup', up)
    persistFiles()
  }
  window.addEventListener('mousemove', move)
  window.addEventListener('mouseup', up)
}
onMounted(() => window.addEventListener('keydown', onWinKey))
onBeforeUnmount(() => window.removeEventListener('keydown', onWinKey))
</script>

<template>
  <NConfigProvider :theme="darkTheme" :theme-overrides="overrides" class="app"
                   style="height:100%; display:flex; overflow:hidden;">
    <!-- 会话栏（可折叠/可拖宽） -->
    <aside v-if="!collapsed"
           :style="{ width: sideW + 'px' }"
           style="flex:none; display:flex; flex-direction:column;
                  background:var(--panel); border-right:1px solid var(--line); position:relative;">
      <div id="brand" style="display:flex; align-items:center; gap:4px;">
        <div style="flex:1; min-width:0;">Ithaca<small>终端编程代理</small></div>
        <button class="side-toggle" title="折叠会话栏 (Ctrl+B)" @click="toggleSide">⟨</button>
      </div>
      <button class="new-chat" @click="newSession()">+ 新会话</button>
      <Sidebar :class="{ inflight: S.inFlight }"
               style="display:flex; flex-direction:column; min-height:0; flex:1;" />
      <!-- 齿轮与统计同行（行内布局）：曾绝对定位 bottom:30px，与 side-foot 的 border-top 重叠 -->
      <div class="side-foot">
        <n-tooltip placement="top" :show-arrow="false">
          <template #trigger>
            <button class="gear-btn" @click="settingsRef.openPanel()">⚙</button>
          </template>
          设置
        </n-tooltip>
        <span>累计 <b>{{ S.fTurns.toLocaleString() }}</b> 轮，<b>{{ S.fTokens.toLocaleString() }}</b> tokens</span>
      </div>
      <div class="side-drag" title="拖拽调宽" @mousedown="startDrag"></div>
    </aside>
    <aside v-else
           style="width:44px; flex:none; display:flex; flex-direction:column; align-items:center;
                  background:var(--panel); border-right:1px solid var(--line); padding:8px 0; gap:6px;">
      <button class="mini-btn" title="展开会话栏 (Ctrl+B)" @click="toggleSide">⟩</button>
      <button class="mini-btn" title="新会话" @click="newSession()">＋</button>
      <div style="flex:1"></div>
      <button class="mini-btn" title="设置" @click="settingsRef.openPanel()">⚙</button>
    </aside>

    <!-- 主区 -->
    <div style="flex:1; display:flex; flex-direction:column; min-width:0;">
      <div class="topbar">
        <span class="kv">工作区 <b>{{ shortWs }}</b></span>
        <span class="spacer"></span>
        <span id="conn" :class="{ off: S.conn !== 'on' }">{{ S.conn === 'on' ? '已连接' : '连接中' }}</span>
        <button class="mini-btn" :title="filesOpen ? '收起文件树' : '展开文件树'" @click="toggleFiles">🗂</button>
      </div>

      <!-- 无工作区：hero 引导；有工作区：对话流 + dock -->
      <template v-if="!S.curWs">
        <div class="hero">
          <div class="hero-title">Ithaca<span class="beta">预览版</span></div>
          <button class="hero-ws" title="选择工作文件夹" @click="pickFolder">
            📂 {{ S.curWs ? shortWs : '选择工作文件夹' }} ▾
          </button>
          <div class="inbar" style="width:min(var(--content-w), 92%); padding:0;">
            <InputBar />
          </div>
        </div>
      </template>
      <template v-else>
        <Stream />
        <div class="inbar" style="display:flex; flex-direction:column;">
          <InputBar />
        </div>
      </template>
    </div>

    <!-- 文件树右栏：当前会话工作区的只读视图；无工作区时不占位（hero 引导优先） -->
    <aside v-if="filesOpen && S.curWs" class="fileside" :style="{ width: filesW + 'px' }">
      <div class="fs-head">
        <span class="fs-title">文件</span>
        <span class="spacer"></span>
        <button class="fs-btn" title="刷新" @click="fileTreeRef && fileTreeRef.refresh()">⟳</button>
        <button class="fs-btn" title="收起文件树" @click="toggleFiles">⟩</button>
      </div>
      <FileTree ref="fileTreeRef" />
      <div class="filedrag" title="拖拽调宽" @mousedown="startFilesDrag"></div>
    </aside>

    <Settings ref="settingsRef" />
  </NConfigProvider>
</template>
