<script setup>
// 插件贡献设置：按 /api/ui/registry 的 schema 动态渲染各插件设置节。
// 控件集 v1：text/number/bool/select（path 型 v1 按 text 渲染——原生选目录走
// SSE folder_picked，那是会话语义，接进来会误触草稿/开新会话）。
// 编辑在本地副本（vals）进行，保存时整节写回 S.pluginVals → /api/config。
import { reactive, watch, ref } from 'vue'
import Cdl from './Cdl.vue'
import { S, savePluginVals } from '../store'

const vals = reactive({})   // {[plugin]: {[key]: value}} 编辑副本
const err = ref('')
const saved = ref(false)

// registry 变化（面板打开拉取后）即重建编辑副本：值取 plugins 节 → schema default → 类型零值
watch(() => S.pluginUi, () => {
  for (const p of S.pluginUi) {
    const cur = S.pluginVals[p.plugin] || {}
    const base = {}
    for (const f of p.schema) {
      if (cur[f.key] !== undefined) { base[f.key] = cur[f.key]; continue }
      if (f.default !== undefined) { base[f.key] = f.default; continue }
      base[f.key] = f.type === 'bool' ? false : f.type === 'number' ? 0 : ''
    }
    vals[p.plugin] = base
  }
}, { immediate: true, deep: false })

async function onSave() {
  err.value = ''; saved.value = false
  for (const p of S.pluginUi) {
    for (const f of p.schema) {
      const v = vals[p.plugin]?.[f.key]
      if (f.type === 'number' && (typeof v !== 'number' || !isFinite(v) || v < 0)) {
        err.value = `${p.label} · ${f.label}：请填写非负数字`; return
      }
    }
    S.pluginVals[p.plugin] = { ...(vals[p.plugin] || {}) }
  }
  const r = await savePluginVals()
  if (!r.ok) { err.value = r.error || '保存失败'; return }
  saved.value = true
  setTimeout(() => { saved.value = false }, 2000)
}
</script>

<template>
  <div class="nbset">
    <div v-if="!S.pluginUi.length" class="snote">
      （暂无插件声明设置界面——带 UI 的插件激活后这里会自动出现）
    </div>
    <div v-for="p in S.pluginUi" :key="p.plugin" class="plugsec">
      <div class="secT">{{ p.label }}</div>
      <div v-for="f in p.schema" :key="f.key" class="frow">
        <label>{{ f.label }}</label>
        <Cdl v-if="f.type === 'select'" block down bordered
             :items="(f.options || []).map(o => ({ value: o, label: o }))"
             v-model="vals[p.plugin][f.key]" />
        <span v-else-if="f.type === 'bool'" class="tglwrap">
          <span class="tgl" :class="{ off: !vals[p.plugin][f.key] }"
                :title="f.label"
                @click="vals[p.plugin][f.key] = !vals[p.plugin][f.key]"></span>
          <span class="bval">{{ vals[p.plugin][f.key] ? '开' : '关' }}</span>
        </span>
        <input v-else-if="f.type === 'number'" type="number" min="0"
               v-model.number="vals[p.plugin][f.key]" spellcheck="false"
               :placeholder="f.placeholder">
        <input v-else v-model="vals[p.plugin][f.key]" spellcheck="false"
               :placeholder="f.placeholder || (f.type === 'path' ? '粘贴绝对路径' : undefined)">
      </div>
    </div>
    <div v-if="S.pluginUi.length" class="sops">
      <button class="primary" @click="onSave">保存</button>
      <span v-if="err" class="perr">{{ err }}</span>
      <span v-else-if="saved" class="pok">已保存，相关插件热重载中</span>
    </div>
  </div>
</template>

<style scoped>
.plugsec { margin-bottom: 18px; }
.secT { font-weight: 600; margin: 6px 0 10px; color: var(--fg); }
.frow { display: flex; align-items: center; gap: 10px; margin: 8px 0; }
.frow label { width: 170px; flex: none; color: var(--dim); font-size: 13px; }
.frow input, .frow select { flex: 1; min-width: 0; }
.tglwrap { display: flex; align-items: center; gap: 8px; }
.bval { font-size: 12px; color: var(--dim); }
.perr { color: var(--red); font-size: 12px; }
.pok { color: var(--acc); font-size: 12px; }
</style>
