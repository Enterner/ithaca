<script setup>
// 右栏 · 文档版式预览：/api/fs/preview → office 插件 COM 转 PDF（服务端缓存）
// → WebView2 原生渲染器（<embed>，零前端依赖）。转换失败（没装 Office/超时杀
// /格式不支持）emit fallback——App.vue 摘 kind 切回文本抽取态，本组件随之卸载
//（卸载与 emit 同 tick，无需就地错误 UI）。
import { ref, onMounted, onBeforeUnmount } from 'vue'
import { S, previewUrl } from '../store'

const blobUrl = ref('')
const emit = defineEmits(['fallback'])

onMounted(load)
onBeforeUnmount(() => { if (blobUrl.value) URL.revokeObjectURL(blobUrl.value) })

async function load() {
  const path = S.viewer.path
  try {
    // 不走 api()（其 GET 恒 10s 超时）——转换服务端自限 30s（超时杀进程），
    // 客户端放宽到 45s；换文件/返回树后迟到的响应当弃
    const resp = await fetch(previewUrl(path), { signal: AbortSignal.timeout(45_000) })
    const ct = resp.headers.get('content-type') || ''
    if (!S.viewer || S.viewer.path !== path) return
    if (resp.ok && ct.includes('application/pdf')) {
      blobUrl.value = URL.createObjectURL(await resp.blob())
      return
    }
    const r = await resp.json().catch(() => ({}))
    emit('fallback', r.error || '转换服务未返回 PDF')
  } catch {
    if (S.viewer && S.viewer.path === path) emit('fallback', '预览请求失败（超时或网络错误）')
  }
}
</script>

<template>
  <div class="cv">
    <div class="cv-bar">
      <button class="fs-btn" title="返回文件树" @click="S.viewer = null">←</button>
      <span class="cv-name" :title="S.viewer.path">{{ (S.viewer.path || '').split('/').pop() }}</span>
      <!-- 来源徽标：内容是 office 插件转换的 PDF 版式，非原文件字节直读 -->
      <span class="cv-doc" title="office 插件转换的 PDF 版式预览">PDF 预览</span>
      <span class="spacer"></span>
    </div>
    <div class="cv-wrap">
      <!-- blob URL：fetch→objectURL 而非直连 src——转换失败/非 PDF 响应要在
           JS 里识别后回落，embed 对加载失败零事件可依 -->
      <embed v-if="blobUrl" class="ov-frame" :src="blobUrl" type="application/pdf">
      <div v-if="!blobUrl" class="cv-state">转换中…（首次打开需数秒，再次打开走缓存）</div>
    </div>
  </div>
</template>
