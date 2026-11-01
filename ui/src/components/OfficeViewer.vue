<script setup>
// 右栏 · 预览查看器：/api/fs/preview → office 插件（文档 COM 转 PDF / 图片直通，
// 服务端缓存）→ WebView2 原生渲染（PDF 用 <embed>、图片用 <img>，零前端依赖）。
// 转换失败（没装 Office/超时杀/格式不支持）：文档类 emit fallback——App.vue 摘
// kind 切回文本抽取态（文本抽取对文档恒可用）；图片类无文本可回退，就地显示
// 错误（失败语义 = 该文件预览不可用）。
import { computed, ref, onMounted, onBeforeUnmount } from 'vue'
import { S, previewUrl } from '../store'

const blobUrl = ref('')
const blobKind = ref('')     // pdf | img（按响应 content-type 分流渲染器）
const failed = ref('')
const errMsg = ref('')
const emit = defineEmits(['fallback'])

const isImgPath = computed(() => /\.(jpg|jpeg|png|gif|webp|bmp)$/i.test(S.viewer?.path || ''))

onMounted(load)
onBeforeUnmount(() => { if (blobUrl.value) URL.revokeObjectURL(blobUrl.value) })

async function load() {
  const path = S.viewer.path
  try {
    // 不走 api()（其 GET 恒 10s 超时）——转换服务端按源文件大小自限（30-120s
    // 看门狗杀进程），客户端放宽到 150s；换文件/返回树后迟到的响应当弃
    const resp = await fetch(previewUrl(path), { signal: AbortSignal.timeout(150_000) })
    const ct = resp.headers.get('content-type') || ''
    if (!S.viewer || S.viewer.path !== path) return
    if (resp.ok && (ct.includes('application/pdf') || ct.startsWith('image/'))) {
      blobKind.value = ct.startsWith('image/') ? 'img' : 'pdf'
      blobUrl.value = URL.createObjectURL(await resp.blob())
      return
    }
    const r = await resp.json().catch(() => ({}))
    fail(r.error || '转换服务未返回预览')
  } catch {
    if (S.viewer && S.viewer.path === path) fail('预览请求失败（超时或网络错误）')
  }
}
// 文档类失败 → 回落文本抽取（父组件切视图，本组件卸载）；图片类无文本可回退
// → 就地错误态（errcard 语义）
function fail(msg) {
  failed.value = msg
  errMsg.value = msg
  if (!isImgPath.value) emit('fallback', msg)
}
</script>

<template>
  <div class="cv">
    <div class="cv-bar">
      <button class="fs-btn" title="返回文件树" @click="S.viewer = null">←</button>
      <span class="cv-name" :title="S.viewer.path">{{ (S.viewer.path || '').split('/').pop() }}</span>
      <!-- 来源徽标：内容是 office 插件产出的预览形态，非原文件字节直读 -->
      <span class="cv-doc">{{ blobKind === 'img' ? '图片预览' : 'PDF 预览' }}</span>
      <span class="spacer"></span>
    </div>
    <div class="cv-wrap">
      <!-- blob URL：fetch→objectURL 而非直连 src——转换失败/非预期响应要在
           JS 里识别后分流，embed/img 对加载失败零事件可依 -->
      <img v-if="blobUrl && blobKind === 'img'" class="ov-frame" :src="blobUrl"
           :alt="S.viewer.path" draggable="false">
      <embed v-else-if="blobUrl" class="ov-frame" :src="blobUrl" type="application/pdf">
      <div v-if="!blobUrl && !failed" class="cv-state">转换中…</div>
      <div v-if="failed" class="cv-state cv-err">{{ errMsg }}</div>
    </div>
  </div>
</template>
