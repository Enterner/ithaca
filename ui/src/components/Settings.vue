<script setup>
// 设置弹窗：标签页结构（模型接入 / MCP 工具 / 插件贡献）。
// 模型接入 = 供应商两层配置（第一层=供应商列表/详情，第二层=模型编辑对话框）。
// 语义对齐后端 /api/config：api_key 不回显（留空=按名保留）、内置供应商名称只读不可删。
// MCP 工具页 = McpSettings 组件（server 两层配置，保存 → mcp 插件热重载）。
// 插件贡献页 = PluginSettings（方案 A：各插件 vc.ui.register 的 schema 动态生成，
// 有插件注册时 tab 才出现——零硬编码，新插件自带 UI 即插即现）。
import { ref, reactive } from 'vue'
import { NModal } from 'naive-ui'
import { S, saveConfig, loadPluginUi } from '../store'
import { confirmDialog } from '../confirm'
import McpSettings from './McpSettings.vue'
import PluginSettings from './PluginSettings.vue'

const show = defineModel({ type: Boolean, default: false })
const tab = ref('model')      // 'model' | 'mcp' | 'plug'
const provSel = ref(0)
const detail = ref(null)      // 编辑中的 providers 深拷贝（保存前不污染 store）
const curName = ref('')
const mDlg = ref(false)
const mEdit = ref(null)       // {mi}  mi=-1 新增
const mId = ref(''), mCtx = ref(''), mErr = ref('')
const keyInputs = reactive({})   // 临时输入的 key（不回显，仅保存时带上）

async function openPanel() {
  // 从 store 深拷贝；api_key 输入框恒空（后端掩码，留空=保留旧值）
  detail.value = JSON.parse(JSON.stringify(S.providers)).map(p => ({ ...p, api_key: '', models: p.models || [] }))
  curName.value = S.current
  provSel.value = Math.max(0, detail.value.findIndex(p => p.name === S.current))
  loadPluginUi()   // 插件贡献清单：每次打开拉最新（新插件激活后即现）
  show.value = true
}
defineExpose({ openPanel })

const cur = () => detail.value[provSel.value]
function addProvider() {
  detail.value.push({ name: '', type: 'cloud', builtin: false, base_url: '',
                      enabled: true, has_key: false, models: [] })
  provSel.value = detail.value.length - 1
}
async function delProvider() {
  const p = cur()
  if (!(await confirmDialog({
    title: '删除供应商',
    content: '「' + (p.name || '未命名') + '」将从列表移除（点保存后生效）。',
    positive: '移除',
  }))) return
  detail.value.splice(provSel.value, 1)
  if (curName.value === p.name) curName.value = detail.value[0] ? detail.value[0].name : ''
  provSel.value = Math.max(0, Math.min(provSel.value, detail.value.length - 1))
}
function setCur() { if (cur().name) curName.value = cur().name }
function openModelDlg(mi) {
  mEdit.value = { mi }
  const m = mi >= 0 ? cur().models[mi] : { id: '', context: 128000 }
  mId.value = m.id
  mCtx.value = fmtCtx(m.context)
  mErr.value = ''
  mDlg.value = true
}
function saveModel() {
  const p = cur()
  const id = mId.value.trim()
  const ctx = parseCtx(mCtx.value)
  if (!id) { mErr.value = '模型 ID 不能为空'; return }
  if (ctx === null) { mErr.value = '上下文窗口非法（≥1024，支持 1M / 256K / 数字）'; return }
  if (p.models.some((m, i) => i !== mEdit.value.mi && m.id.toLowerCase() === id.toLowerCase())) {
    mErr.value = '该供应商下已有同名模型 ID'; return
  }
  if (mEdit.value.mi >= 0)
    p.models[mEdit.value.mi] = { id, context: ctx, enabled: p.models[mEdit.value.mi].enabled }
  else
    p.models.push({ id, context: ctx, enabled: true })
  mDlg.value = false
}
async function onSave() {
  // 临时输入的 key 按名并入（留空 = 后端按名保留旧值）
  for (const p of detail.value) {
    const k = keyInputs[p.name]
    if (k) p.api_key = k
  }
  const r = await saveConfig(detail.value, curName.value)
  if (!r.ok) { alert('保存失败：' + (r.error || '未知错误')); return }   // 面板不关，本地状态不动
  show.value = false
}
function fmtCtx(n) {
  if (n >= 1e6 && n % 1e6 === 0) return (n / 1e6) + 'M'
  if (n >= 1e3 && n % 1e3 === 0) return (n / 1e3) + 'K'
  return String(n)
}
function parseCtx(s) {
  s = (s || '').trim().toUpperCase()
  const mul = s.endsWith('M') ? 1e6 : s.endsWith('K') ? 1e3 : 1
  const n = mul > 1 ? parseFloat(s.slice(0, -1)) : parseFloat(s)
  if (!isFinite(n) || n <= 0) return null
  const t = Math.round(n * mul)
  return t >= 1024 ? t : null
}
</script>

<template>
  <NModal v-model:show="show" preset="card" draggable
          :style="{ width: 'min(880px, 94vw)' }" :bordered="false">
    <template #header>
      <div class="sethead">
        <span class="title">设置</span>
        <div class="settabs">
          <span class="tab" :class="{ on: tab === 'model' }" @click="tab = 'model'">模型接入</span>
          <span class="tab" :class="{ on: tab === 'mcp' }" @click="tab = 'mcp'">MCP 工具</span>
          <span v-if="S.pluginUi.length" class="tab" :class="{ on: tab === 'plug' }"
                @click="tab = 'plug'">插件</span>
        </div>
      </div>
    </template>
    <div class="nbset" v-if="tab === 'model'">
      <div class="pwrap" v-if="detail">
        <!-- 左：供应商列表 -->
        <div class="pside">
          <div class="grp">内置</div>
          <div v-for="(p, i) in detail.filter(x => x.builtin)" :key="'b' + p.name"
               class="prov" :class="{ sel: detail.indexOf(p) === provSel }"
               @click="provSel = detail.indexOf(p)">
            <span class="act" style="color: var(--acc); cursor: default">{{ p.type === 'mock' ? '⚙' : '◆' }}</span>
            <span>{{ p.name || '(未命名)' }}</span>
            <span class="dot" :class="p.name === curName ? 'on' : 'off'" :title="p.name === curName ? '当前使用' : ''"></span>
          </div>
          <div class="grp">自定义供应商</div>
          <div v-for="(p, i) in detail.filter(x => !x.builtin)" :key="'c' + i"
               class="prov" :class="{ sel: detail.indexOf(p) === provSel }"
               @click="provSel = detail.indexOf(p)">
            <span class="act" style="color: var(--acc); cursor: default">◆</span>
            <span>{{ p.name || '(未命名)' }}</span>
            <span class="dot" :class="p.name === curName ? 'on' : 'off'"></span>
          </div>
          <div class="prov" style="color: var(--acc)" @click="addProvider">＋ 添加供应商</div>
        </div>
        <!-- 右：详情 -->
        <div class="pdetail" v-if="cur()">
          <div class="dhead">
            <span class="name">{{ cur().name || '（未命名供应商）' }}</span>
            <span v-if="cur().name === curName" class="cur-chip">当前使用</span>
            <span v-else-if="cur().name" class="cur-chip click" title="对话实际走该供应商（保存后热切）"
                  @click="setCur">设为当前</span>
            <span class="spacer"></span>
            <span class="tgl" :class="{ off: !cur().enabled }" title="启用该供应商"
                  @click="cur().enabled = !cur().enabled"></span>
            <span v-if="!cur().builtin" class="act" title="删除供应商" @click="delProvider">✕</span>
          </div>
          <label>名称</label>
          <input v-model="cur().name" :disabled="cur().builtin" spellcheck="false"
                 :placeholder="cur().builtin ? undefined : '如：火山引擎'">
          <template v-if="cur().type !== 'mock'">
            <label>Base URL</label>
            <input v-model="cur().base_url" spellcheck="false" placeholder="https://api.deepseek.com">
            <label>API Key</label>
            <input type="password" v-model="keyInputs[cur().name || '']"
                   :placeholder="cur().has_key ? '已设置（留空 = 不修改）' : '未设置'">
          </template>
          <label style="display:flex; justify-content:space-between; align-items:center; margin-top:14px;">
            <span>模型列表</span><span class="addm" @click="openModelDlg(-1)">＋ 添加模型</span>
          </label>
          <div v-for="(m, mi) in cur().models" :key="mi" class="mrow">
            <span class="mid">{{ m.id }}</span>
            <span class="badge" :title="'上下文窗口 ' + m.context.toLocaleString() + ' tokens'">{{ fmtCtx(m.context) }}</span>
            <span class="tgl" :class="{ off: m.enabled === false }" title="启用（关闭后不出现在对话下拉）"
                  @click="m.enabled = m.enabled === false"></span>
            <span class="act" title="编辑模型配置" @click="openModelDlg(mi)">✎</span>
            <span class="act" title="删除模型" @click="cur().models.splice(mi, 1)">✕</span>
          </div>
          <div v-if="!cur().models.length" class="snote">（还没有模型——点右上「＋ 添加模型」）</div>
        </div>
      </div>
      <div class="sops">
        <button class="primary" @click="onSave">保存</button>
        <button @click="show = false">取消</button>
      </div>
    </div>
    <!-- MCP 工具页：server 两层配置（保存 → mcp 插件热重载），结构与模型接入同构 -->
    <div class="nbset" v-else-if="tab === 'mcp'">
      <McpSettings :active="tab === 'mcp' && show" @close="tab = 'model'" />
    </div>
    <!-- 插件贡献页（方案 A）：schema 动态渲染，各插件设置即插即现 -->
    <PluginSettings v-else-if="tab === 'plug'" />

    <!-- 第二层：模型编辑对话框（同样可拖动、默认居中） -->
    <NModal v-model:show="mDlg" preset="card" draggable
            :style="{ width: '420px' }" :bordered="false">
      <template #header><span style="font-weight: 600;">{{ mEdit && mEdit.mi >= 0 ? '编辑模型配置' : '添加模型' }}</span></template>
      <div class="nbset">
        <label>模型 ID</label>
        <input v-model="mId" spellcheck="false">
        <label>上下文窗口 <span style="color:var(--dim)">（支持 1M / 256K / 131072 直接输入）</span></label>
        <input v-model="mCtx" spellcheck="false" @keydown.enter="saveModel">
        <div v-if="mErr" style="color:var(--red); font-size:12px">{{ mErr }}</div>
        <div class="sops">
          <button @click="mDlg = false">取消</button>
          <button class="primary" @click="saveModel">保存</button>
        </div>
      </div>
    </NModal>
  </NModal>
</template>
