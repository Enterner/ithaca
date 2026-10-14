<script setup>
// 右栏 · 代码查看器：Monaco 只读展示（文件树点文件切入，← 返回）。
// 纯展示组件：内容由 App.vue 经 /api/fs/read 拉好放进 S.viewer，本组件不发请求。
// 只读场景不配语言 worker（补全/诊断用不上），只挂 editor.worker 保基础运行时
// （缺它 Monaco 会在控制台报 worker 创建失败）。
import { ref, watch, onMounted, onBeforeUnmount } from 'vue'
import * as monaco from 'monaco-editor'
// worker 子路径不带 esm/vs 前缀：包 exports 的 ./*.js 模式会把子路径映射到 esm/vs/ 下
import editorWorker from 'monaco-editor/editor/editor.worker.js?worker'
import { S } from '../store'

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
monaco.editor.defineTheme('ithaca', {
  base: 'vs-dark', inherit: true, rules: [],
  colors: { 'editor.background': '#12171F', 'editorLineNumber.foreground': '#4A5872' },
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

const host = ref(null)
let ed = null

onMounted(() => {
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

// 同实例切文件复用 editor（反复 create/dispose 会闪且丢滚动位置）
watch(() => [S.viewer?.path, S.viewer?.content], ([path, content]) => {
  if (!ed) return
  monaco.editor.setModelLanguage(ed.getModel(), langOf(path))
  const text = content || ''
  if (ed.getValue() !== text) ed.setValue(text)
  ed.setScrollTop(0)
})

onBeforeUnmount(() => { if (ed) { ed.dispose(); ed = null } })
</script>

<template>
  <div class="cv">
    <div class="cv-bar">
      <button class="fs-btn" title="返回文件树" @click="S.viewer = null">←</button>
      <span class="cv-name" :title="S.viewer?.path">{{ (S.viewer?.path || '').split('/').pop() }}</span>
      <span class="cv-lang">{{ langOf(S.viewer?.path) }}</span>
      <span v-if="S.viewer?.truncated" class="cv-trunc">超 256KB 已截断</span>
    </div>
    <div class="cv-wrap">
      <div ref="host" class="cv-host"></div>
      <div v-if="S.viewer?.loading" class="cv-state">读取中…</div>
      <div v-else-if="S.viewer?.error" class="cv-state cv-err">{{ S.viewer.error }}</div>
    </div>
  </div>
</template>
