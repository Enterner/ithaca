<script setup>
// 右栏 · 代码查看器：Monaco 只读展示 + 编辑对照（docs/查看器diff模式-方案.md §3/§7）。
// 查看态：现盘内容（App.vue 经 /api/fs/read 拉好放进 S.viewer，本组件不发请求）。
// 对照态按新鲜度分档（§7.1）：reverseApply 校验通过（最新折叠 diff 的 after = 现盘）
// ⇒ Monaco diff editor（inline 单栏）——与查看态同渲染器，语法高亮 + 自带词级加重；
// 校验失败（文件已再次变更 / read 截断）⇒ 自绘管线兜底：fullTextDiff 展开 +
// diffHtml 着色（与会话卡同管线；hunk 内容是历史事实，行号可能错位，note 警告）。
// diff editor 懒挂载：首次新鲜对照才 create；切文件/回查看态不销毁（setModel 换
// 内容，旧 model 在 setModel 之后 dispose——反序会告警）；组件卸载时 dispose。
// 只读场景不配语言 worker（补全/诊断用不上），只挂 editor.worker 保基础运行时。
import { ref, computed, watch, onMounted, onBeforeUnmount, nextTick } from 'vue'
import * as monaco from 'monaco-editor'
// worker 子路径不带 esm/vs 前缀：包 exports 的 ./*.js 模式会把子路径映射到 esm/vs/ 下
import editorWorker from 'monaco-editor/editor/editor.worker.js?worker'
import { S } from '../store'
import { diffHtml, parseHunks, reverseApply, fullTextDiff } from '../diff'
import { diffForPath } from '../store/edits'

self.MonacoEnvironment = { getWorker: () => new editorWorker() }

// 只读查看不跑语言服务诊断：关掉后 json/html/css/ts 打开时不会尝试起语言 worker
// （本组件的 getWorker 对任何 label 都只给 editorWorker，语言服务拿到它会协议不匹配报错）
monaco.languages.json?.jsonDefaults?.setDiagnosticsOptions?.({ validate: false })
monaco.languages.css?.cssDefaults?.setDiagnosticsOptions?.({ validate: false })
monaco.languages.html?.htmlDefaults?.setOptions?.({ format: false, suggest: {} })
monaco.languages.typescript?.typescriptDefaults?.setDiagnosticsOptions?.(
  { noSemanticValidation: true, noSyntaxValidation: true })
monaco.languages.javascript?.javascriptDefaults?.setDiagnosticsOptions?.(
  { noSemanticValidation: true, noSyntaxValidation: true })

// Monaco 主题对齐全局 --panel（#12171F）：vs-dark 默认底色 #1e1e1e 会与右栏面板脱节
// diff 两个 Text 令牌 = 对话流 .add/.del 的行底色；.line-insert/.line-delete 未设
// Line 令牌，经 CSS 变量回退链取 Text 值——行级红绿一处配置全覆盖（词级加重由
// global.css 的 .char-insert/.char-delete 覆盖对齐 .mark，见其注释）
monaco.editor.defineTheme('ithaca', {
  base: 'vs-dark', inherit: true, rules: [],
  colors: {
    'editor.background': '#12171F', 'editorLineNumber.foreground': '#4A5872',
    'diffEditor.insertedTextBackground': '#34d3991b',
    'diffEditor.removedTextBackground': '#fb71851b',
  },
})

// 扩展名 → Monaco 语言 id。vue 无官方语言，用 html 近似（模板高亮够用）
const LANGS = {
  cpp: 'cpp', cc: 'cpp', c: 'cpp', h: 'cpp', hpp: 'cpp',
  js: 'javascript', mjs: 'javascript', ts: 'typescript',
  vue: 'html', html: 'html', htm: 'html', css: 'css',
  json: 'json', md: 'markdown', py: 'python', cmake: 'cmake',
}
const langOf = (p) => {
  const ext = (p || '').slice(p.lastIndexOf('.') + 1).toLowerCase()
  return LANGS[ext] || 'plaintext'
}

const host = ref(null)        // 查看态容器
const diffHost = ref(null)    // 对照态容器（v-show 常驻 DOM，供 diff editor 懒挂载复用）
const mode = ref('view')      // 'view' | 'diff'
const diffFresh = ref(false)  // 对照态分支：true → Monaco diff editor；false → v-html 自绘
const hasDiff = ref(false)    // 该文件在当前会话有无编辑记录（决定按钮可用性）
const diffVHtml = ref('')     // 陈旧降级分支的渲染结果（fullTextDiff + diffHtml）
const note = ref('')          // 对照态说明（新鲜度警告 / 正常提示）
const noteWarn = ref(false)

let ed = null          // 查看态 editor
let diffEd = null      // 对照态 diff editor（懒创建；切文件/回查看态不销毁）
let diffModels = null  // 当前 diff 的 {original, modified} model 对；setModel 换新后 dispose 旧对
let diffOriginal = ''  // 新鲜对照的编辑前全文（reverseApply 重建，喂 diff editor original 侧）

const btnTitle = computed(() => !hasDiff.value ? '该文件无编辑记录'
  : (mode.value === 'diff' ? '返回查看' : '显示编辑对照'))
const btnDisabled = computed(() => !hasDiff.value || !!S.viewer?.loading || !!S.viewer?.error)

onMounted(() => {
  hasDiff.value = !!diffForPath(S.viewer?.path)
  // 挂载时 viewer 必非空（App.vue 的 v-if 保证）；内容可能还在路上，空串起步等 setValue
  ed = monaco.editor.create(host.value, {
    value: S.viewer?.content || '',
    language: langOf(S.viewer?.path),
    theme: 'ithaca',
    readOnly: true,
    minimap: { enabled: false },
    automaticLayout: true,   // 右栏可拖宽：容器尺寸变化时自动 relayout
    wordWrap: 'on',
    fontSize: 13,
    scrollBeyondLastLine: false,
    lineNumbersMinChars: 3,
  })
})

// 同实例切文件复用 editor（反复 create/dispose 会闪且丢滚动位置）；切文件回到查看态
watch(() => [S.viewer?.path, S.viewer?.content], ([path, content]) => {
  if (!ed) return
  hasDiff.value = !!diffForPath(path)
  mode.value = 'view'
  monaco.editor.setModelLanguage(ed.getModel(), langOf(path))
  const text = content || ''
  if (ed.getValue() !== text) ed.setValue(text)
  ed.setScrollTop(0)
})

onBeforeUnmount(() => {
  if (diffEd) { diffEd.dispose(); diffEd = null }
  if (diffModels) { diffModels.original.dispose(); diffModels.modified.dispose(); diffModels = null }
  if (ed) { ed.dispose(); ed = null }
})

function toggleMode() {
  if (!hasDiff.value) return
  mode.value === 'view' ? showDiff() : showView()
}

function showView() {
  mode.value = 'view'
  note.value = ''; noteWarn.value = false
}

// 对照态 diff editor 配置（方案 §7.2）：inline 单栏、不折叠（默认关）、严格比对、
// 去 Δ 边条符号；readOnly 同时抑制 gutter 的 ↺ revert 图标；wordWrap 对齐查看态
// （diffWordWrap 默认 inherit）
// NOTE: hideOriginalLineNumbers ⇒ 行号单列（modified 侧，对齐对话流 gutter）——
// 未进 monaco d.ts 但运行时 validate 生效（VS Code multi-diff 同款）；original
// 编辑器宽度归 0。删除块的旧行号由 labelDeletedRows 注入 view zone margin
const DIFF_OPTS = {
  renderSideBySide: false,
  hideOriginalLineNumbers: true,
  readOnly: true,
  theme: 'ithaca',
  automaticLayout: true,   // 右栏可拖宽：容器尺寸变化时自动 relayout
  minimap: { enabled: false },
  scrollBeyondLastLine: false,
  fontSize: 13,
  wordWrap: 'on',
  lineNumbersMinChars: 3,
  ignoreTrimWhitespace: false,   // 严格比对，与后端 diff 语义一致
  renderIndicators: false,
}

/** 新鲜对照的 Monaco diff editor：懒创建；model 对每次重建，先 setModel 再
 *  dispose 旧对（反序会告警）。切文件/回查看态只换 model 不销毁实例。 */
function mountDiff() {
  const lang = langOf(S.viewer?.path)
  const next = {
    original: monaco.editor.createModel(diffOriginal, lang),
    modified: monaco.editor.createModel(S.viewer?.content || '', lang),
  }
  if (!diffEd) {
    diffEd = monaco.editor.createDiffEditor(diffHost.value, DIFF_OPTS)
    diffEd.onDidUpdateDiff(() => requestAnimationFrame(labelDeletedRows))
  }
  // tokenization 异步完成时 zones 会整体重建（diffEditorViewZones 等 original
  // model 的 token），已注入的行号随之丢失——监听 model 事件幂等重标
  next.original.onDidChangeTokens(() => requestAnimationFrame(labelDeletedRows))
  const prev = diffModels
  diffEd.setModel(next)
  diffModels = next
  if (prev) { prev.original.dispose(); prev.modified.dispose() }
}

/** 删除行的旧行号注入：hideOriginalLineNumbers 后删除块是 modified 编辑器里的
 *  view zone，其 margin（.inline-deleted-margin-view-zone）不含行号——按
 *  getLineChanges 的块序（与 margin 生成同源同序，inline 模式下 inner 对齐关闭，
 *  一块一 zone）往每个 margin 注入块内删除行的旧行号，样式对齐真实行号（右对齐
 *  + tabular-nums + 行号前景色）。物理行偏移用 modified 视图模型的换行 computer
 *  求得（与 zone 内容渲染同宽度同源，块内长行换行也逐行对齐）；_getViewModel 是
 *  私有面，失配时降级按 1 物理行铺排（块内换行行会错位）。幂等：注入前先清本
 *  组件旧节点。 */
function labelDeletedRows() {
  if (!diffEd) return
  const changes = diffEd.getLineChanges()
  const modEd = diffEd.getModifiedEditor()
  const modDom = modEd.getDomNode()
  const origModel = diffEd.getOriginalEditor().getModel()
  if (!changes || !modDom || !origModel) return
  const blocks = changes.filter((c) => c.originalEndLineNumber > 0)   // 插入块 originalEnd=0，无删除行
  const margins = modDom.querySelectorAll('.inline-deleted-margin-view-zone')
  if (margins.length !== blocks.length) return   // 结构对不上宁可不出数，不错标
  const blockLines = blocks.map((c) => {
    const arr = []
    for (let ln = c.originalStartLineNumber; ln <= c.originalEndLineNumber; ln++) arr.push(ln)
    return arr
  })
  let rowsFlat = null   // 每逻辑行的物理行数（换行 >1），与 blockLines 展平同序
  try {
    const computer = modEd._getViewModel?.().createLineBreaksComputer({
      getLineContent: (ln) => origModel.getLineContent(ln),
      getLineInjectedText: () => null,
    })
    for (const ln of blockLines.flat()) computer.addRequest(ln, null)
    rowsFlat = computer.finalize().map((b) => b?.breakOffsets?.length ?? 1)
  } catch { /* 私有面失配 → 降级 */ }
  if (!rowsFlat || rowsFlat.length !== blockLines.flat().length)
    rowsFlat = blockLines.flat().map(() => 1)
  const lineHeight = modEd.getOption(monaco.editor.EditorOption.lineHeight)
  const numWidth = modEd.getLayoutInfo().lineNumbersWidth
  let k = 0
  margins.forEach((margin, bi) => {
    margin.querySelectorAll('.cv-del-ln').forEach((n) => n.remove())
    const frag = document.createDocumentFragment()
    let off = 0   // 块内物理行偏移（前面逻辑行的换行累积）
    for (const ln of blockLines[bi]) {
      const d = document.createElement('div')
      d.className = 'cv-del-ln'
      d.textContent = String(ln)
      d.style.cssText = 'position:absolute;left:0;top:' + (off * lineHeight) + 'px;width:' + numWidth +
        'px;height:' + lineHeight + 'px;line-height:' + lineHeight + 'px;text-align:right;' +
        'font-variant-numeric:tabular-nums;pointer-events:none;' +
        'color:var(--vscode-editorLineNumber-foreground,#4A5872)'
      frag.appendChild(d)
      off += rowsFlat[k++]
    }
    margin.appendChild(frag)
  })
}

function showDiff() {
  const diffText = diffForPath(S.viewer?.path)
  if (!diffText) return
  const hunks = parseHunks(diffText)
  // 新鲜度分档（§7.1）：diff 的 modified 侧对得上现盘 ⇒ 新鲜 → Monaco diff editor；
  // 对不上 ⇒ 文件已再次变更，降级自绘管线（渲染仍继续——hunk 内容是历史事实）
  const r = reverseApply(S.viewer?.content || '', hunks)
  if (r.error) {
    diffFresh.value = false
    noteWarn.value = true
    note.value = '文件已再次变更，对照可能与当前内容有出入'
    diffVHtml.value = diffHtml(fullTextDiff(S.viewer?.content || '', hunks))
  } else {
    diffFresh.value = true
    noteWarn.value = false
    note.value = '对照 · 语法高亮渲染，未修改行为当前源码'
    diffOriginal = r.original
  }
  mode.value = 'diff'
  if (diffFresh.value) nextTick(mountDiff)   // 等 v-show 生效后挂载（容器须可见）
}
</script>

<template>
  <div class="cv">
    <div class="cv-bar">
      <button class="fs-btn" title="返回文件树" @click="S.viewer = null">←</button>
      <span class="cv-name" :title="S.viewer?.path">{{ (S.viewer?.path || '').split('/').pop() }}</span>
      <span class="cv-lang">{{ langOf(S.viewer?.path) }}</span>
      <span v-if="S.viewer?.truncated" class="cv-trunc">超 256KB 已截断</span>
      <span class="spacer"></span>
      <button class="mode-btn" :class="{ on: mode === 'diff' }" :disabled="btnDisabled"
              :title="btnTitle" @click="toggleMode">
        <svg width="14" height="14" viewBox="0 0 16 16" fill="none" stroke="currentColor"
             stroke-width="1.3" stroke-linecap="round" stroke-linejoin="round">
          <path d="M9.5 1.5H4a1 1 0 0 0-1 1v11a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1V5L9.5 1.5Z"/>
          <path d="M9.5 1.5V5H13"/>
          <path d="M5.5 10.5h2M6.5 9.5v2"/>
          <path d="M9 7h2.5"/>
        </svg>
      </button>
    </div>
    <div class="cv-wrap">
      <div ref="host" class="cv-host" v-show="mode === 'view'"></div>
      <div ref="diffHost" class="cv-host" v-show="mode === 'diff' && diffFresh"></div>
      <div v-if="mode === 'diff' && !diffFresh" class="cv-diff diff" v-html="diffVHtml"></div>
      <div v-if="S.viewer?.loading" class="cv-state">读取中…</div>
      <div v-else-if="S.viewer?.error" class="cv-state cv-err">{{ S.viewer.error }}</div>
    </div>
    <div v-if="mode === 'diff'" class="cv-note" :class="{ warn: noteWarn }">{{ note }}</div>
  </div>
</template>
