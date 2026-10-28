<script setup>
// 布局：会话栏 + 主区（顶栏 / 对话流或 hero / 输入栏）。Naive UI 只做暗色主题基座。
// 会话栏 VSCode 化：拖拽调宽（180–420，localStorage 记忆）、折叠成 44px 图标条
// （头部按钮或 Ctrl+B），全屏/缩小时栏宽不变、内容区自适应。
import { ref, onMounted, onBeforeUnmount, computed, watch } from 'vue'
import { NConfigProvider, NTooltip, darkTheme } from 'naive-ui'
import { S, boot, pickFolder, newSession, fetchFile } from './store'
import { themeOverrides } from './theme'
import Sidebar from './components/Sidebar.vue'
import Stream from './components/Stream.vue'
import InputBar from './components/InputBar.vue'
import Settings from './components/Settings.vue'
import FileTree from './components/FileTree.vue'
import CodeViewer from './components/CodeViewer.vue'
import SubagentPane from './components/SubagentPane.vue'

const settingsRef = ref(null)
const fileTreeRef = ref(null)
onMounted(() => { boot() })
const curWs = computed(() => S.curWs ? S.curWs : '(未设置)')
// 点击主会话的子代理卡 ⇒ 右栏自动展开到过程视图
watch(() => S.subView, (v) => { if (v) filesOpen.value = true })

// ---- 左右栏的宽度/折叠记忆与拖拽（两栏同构，一个工厂出两套）----------------
// 差异只在持久化键、边界、指针→宽度换算与布尔字段名（左栏存 collapsed、右栏
// 存 open——沿用旧 localStorage 形态，升级不清掉用户的既有记忆）
function makePane({ lsKey, field, init, min, max, calc, initFlag }) {
  const w = ref(init)
  const on = ref(initFlag)
  try {
    const saved = JSON.parse(localStorage.getItem(lsKey) || '{}')
    if (saved.w >= min && saved.w <= max) w.value = saved.w
    on.value = !!saved[field]
  } catch (e) {}
  function persist() { localStorage.setItem(lsKey, JSON.stringify({ w: w.value, [field]: on.value })) }
  function toggle() { on.value = !on.value; persist() }
  function startDrag(e) {
    e.preventDefault()
    const move = (ev) => { w.value = Math.min(max, Math.max(min, calc(ev))) }
    const up = () => {
      window.removeEventListener('mousemove', move)
      window.removeEventListener('mouseup', up)
      persist()
    }
    window.addEventListener('mousemove', move)
    window.addEventListener('mouseup', up)
  }
  return { w, on, toggle, startDrag }
}
// 会话栏（拖右缘调宽）与文件树右栏（栏在右侧：宽 = 窗口宽 - 指针 x，拖左缘）
const { w: sideW, on: collapsed, toggle: toggleSide, startDrag } = makePane({
  lsKey: 'vcSidebar', field: 'collapsed', init: 248, min: 180, max: 420,
  calc: (ev) => ev.clientX, initFlag: false,
})
const { w: filesW, on: filesOpen, toggle: toggleFiles, startDrag: startFilesDrag } = makePane({
  lsKey: 'vcFiles', field: 'open', init: 232, min: 200, max: 1000,
  calc: (ev) => window.innerWidth - ev.clientX, initFlag: false,
})
// 右栏卡是否占位(树或过程视图任一可见):驱动主卡的让位缝与 fileside 的开合过渡
const filesShown = computed(() => !!S.curWs && (filesOpen.value || !!S.subView))
// 树根部条目数(FileTree defineExpose 的 computed,经模板 ref 读取保持响应式)
const ftCount = computed(() => { const t = fileTreeRef.value; return t ? t.rootCount : '' })
function onWinKey(e) {
  if (e.ctrlKey && !e.shiftKey && !e.altKey && e.key.toLowerCase() === 'b') {
    toggleSide(); e.preventDefault()
  }
}

// ---- 代码查看器（文件树点文件切入；只读，编辑归 agent 提案审批链）------------
function openFile(path) {   // 内容经 /api/fs/read 代理 workspace read；纯展示态不经 actions 层
  S.viewer = { path, content: '', truncated: false, loading: true, error: '', doc: '' }
  fetchFile(path).then((r) => {
    if (!S.viewer || S.viewer.path !== path) return   // 已返回树/已换文件：丢弃迟到响应
    S.viewer.loading = false
    if (!r.ok) { S.viewer.error = r.error || '读取失败'; return }
    S.viewer.content = r.content || ''
    S.viewer.truncated = !!r.truncated
    S.viewer.doc = r.doc || ''   // 文档抽取来源（docx / ifilter:pdf…）——查看器据此出「非原文排版」标识
  }).catch(() => {
    if (S.viewer && S.viewer.path === path) { S.viewer.loading = false; S.viewer.error = '网络错误' }
  })
}
watch(() => S.curWs, () => { S.viewer = null })   // 切工作区不留上一工作区的文件内容
onMounted(() => window.addEventListener('keydown', onWinKey))
onBeforeUnmount(() => window.removeEventListener('keydown', onWinKey))
</script>

<template>
  <NConfigProvider :theme="darkTheme" :theme-overrides="themeOverrides" class="app"
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
      <Sidebar style="display:flex; flex-direction:column; min-height:0; flex:1;" />
      <!-- 齿轮（行内布局）：曾绝对定位 bottom:30px，与 side-foot 的 border-top 重叠；
           会话累计 tokens 已并入输入栏的上下文构成卡（那里才是语境所在） -->
      <div class="side-foot">
        <n-tooltip placement="top" :show-arrow="false">
          <template #trigger>
            <button class="gear-btn" @click="settingsRef.openPanel()">⚙</button>
          </template>
          设置
        </n-tooltip>
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

    <!-- 主区(浮卡):panel 底圆角,与文件树卡之间留缝,开合时 margin 过渡 -->
    <div class="maincard" :class="{ 'with-files': filesShown }">
      <div class="topbar">
        <span class="kv">工作区 <b>{{ curWs }}</b></span>
        <span class="spacer"></span>
        <span id="conn" :class="{ off: S.conn !== 'on' }">{{ S.conn === 'on' ? '已连接' : '连接中' }}</span>
        <!-- 文件树开合:隔线式自绘图标(方框 + 右缘短隔线 + 箭头),树开=箭头朝右推合 -->
        <button class="mini-btn ft-toggle" :disabled="!S.curWs"
                :title="filesOpen ? '收起文件树' : '展开文件树'" @click="toggleFiles">
          <svg v-if="filesOpen" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"
               stroke-linecap="round" stroke-linejoin="round" style="width:16px;height:16px;">
            <rect x="3.5" y="3.5" width="17" height="17" rx="4.5"/>
            <line x1="15.5" y1="7.5" x2="15.5" y2="16.5"/>
            <polyline points="9.5 9.5 12.5 12 9.5 14.5"/>
          </svg>
          <svg v-else viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"
               stroke-linecap="round" stroke-linejoin="round" style="width:16px;height:16px;">
            <rect x="3.5" y="3.5" width="17" height="17" rx="4.5"/>
            <line x1="15.5" y1="7.5" x2="15.5" y2="16.5"/>
            <polyline points="12.5 9.5 9.5 12 12.5 14.5"/>
          </svg>
        </button>
      </div>

      <!-- 无工作区：hero 引导；有工作区：对话流 + dock -->
      <template v-if="!S.curWs">
        <div class="hero">
          <div class="hero-title">Ithaca<span class="beta">预览版</span></div>
          <button class="hero-ws" title="选择工作文件夹" @click="pickFolder">
            📂 {{ S.curWs ? curWs : '选择工作文件夹' }} ▾
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

    <!-- 右栏(浮卡):子代理过程视图（选中时优先）⇄ 文件树/查看器。
         折叠不卸载只收宽(width→0 过渡),保持树展开态与过渡动画 -->
    <aside v-if="S.curWs" class="fileside" :class="{ closed: !filesShown }"
           :style="{ width: (filesShown ? filesW : 0) + 'px' }">
      <div class="fs-head">
        <span class="fs-title">{{ S.subView ? '子代理' : '文件' }}</span>
        <span v-if="!S.subView && !S.viewer" class="fs-count">{{ ftCount }} 项</span>
        <span v-else class="spacer"></span>
        <!-- 过程视图无自带关闭入口,头部保留其专属关闭;刷新随 fsRev 自动,不再设钮 -->
        <button v-if="S.subView" class="fs-btn" title="关闭过程视图" @click="S.subView = 0">⟩</button>
      </div>
      <!-- 文件态 ⇄ 查看态/子代理态：树用 v-show 保展开态；查看器与子代理过程
           v-if（退出即释放实例/缓冲）。子代理优先——查看器在点击卡片前置空 -->
      <SubagentPane v-if="S.subView" />
      <template v-else>
        <FileTree v-show="!S.viewer" ref="fileTreeRef" @open-file="openFile" />
        <CodeViewer v-if="S.viewer" />
      </template>
      <div class="filedrag" title="拖拽调宽" @mousedown="startFilesDrag"></div>
    </aside>

    <Settings ref="settingsRef" />
  </NConfigProvider>
</template>
