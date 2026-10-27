<script setup>
// 设置弹窗：标签页结构（模型接入 / MCP 工具 / 插件贡献）。
// 模型接入 = 供应商两层配置（第一层=供应商列表/详情，第二层=模型编辑对话框）。
// 语义对齐后端 /api/config：api_key 不回显（留空=按名保留）、内置供应商名称只读不可删。
// MCP 工具页 = McpSettings 组件（server 两层配置，保存 → mcp 插件热重载）。
// 插件贡献页 = PluginSettings（各插件 vc.ui.register 的 schema 动态生成，
// 有插件注册时 tab 才出现——零硬编码，新插件自带 UI 即插即现）。
import { ref, reactive, computed, watch } from 'vue'
import { NModal } from 'naive-ui'
import Cdl from '../Cdl.vue'
import { S, saveConfig, loadPluginUi, fmtCtx, parseCtx, DEFAULT_CONTEXT,
         fetchProviderModels } from '../store'
import { PRESETS } from '../presets'
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
const mSub = ref('')          // 子 agent 模型选中值（JSON 串 {provider,model}；'' = 默认自己）
const mSubDangling = ref('')  // 失效引用的展示名（供应商/模型已删；保存时后端会拦截）
const keyInputs = reactive({})   // 临时输入的 key（不回显，仅保存时带上）

const presetSel = ref('')
// 下拉选项 + 「当前套用模板」的显示口径：选完不复位（用户要求能看到当前用的
// 哪个模板）；重开面板/切换供应商时按 base_url 反推——套用后再手改 Base URL
// 或模型表则显示不追改（这是来源提示，不是双向同步的状态）。
const presetOptions = PRESETS.map(t => ({ label: t.name, value: t.key }))
function inferPreset() {
  const p = cur()
  if (!p) { presetSel.value = ''; return }   // 清单未就绪/空：无处匹配，清空显示
  presetSel.value = PRESETS.find(t => t.base_url === p.base_url)?.key || ''
}
watch(provSel, inferPreset)
function applyPreset(key) {
  const t = PRESETS.find(x => x.key === key)
  if (!t) { inferPreset(); return }   // 清空选择 = 仅取消选中，不动配置
  const p = cur()
  if (!p.name) {
    p.name = t.name
    // key 输入以 name 为键：模板补名后迁移，避免先粘的 key 悬空丢失
    if (keyInputs['']) { keyInputs[t.name] = keyInputs['']; delete keyInputs[''] }
  }
  p.base_url = t.base_url
  p.models = t.models.map(m => ({ ...m, enabled: true }))
  fetchMsg.value = `已套用「${t.name}」模板：Base URL + ${t.models.length} 个预设模型（可再手动增删）`
}

// 从 API 获取模型：浏览器直连平台 API 会被 CORS 拦，经后端代理发 OpenAI 兼容
// GET /models；结果走 SSE provider_models 事件，在下方 watch 里合并。
// key 时序两种都通：刚粘贴的（keyInputs）优先，留空则后端按名取已存的。
const fetchingModels = ref(false)
const fetchMsg = ref('')
async function fetchModels() {
  const p = cur()
  if (p.type === 'mock') return
  if (!p.base_url) { fetchMsg.value = '先填 Base URL 再获取'; return }
  fetchingModels.value = true
  fetchMsg.value = ''
  const r = await fetchProviderModels(p.name || '', p.base_url,
                                      keyInputs[p.name || ''] || '')
  if (!r.ok) { fetchingModels.value = false; fetchMsg.value = r.error || '请求发送失败' }
}
watch(() => S.provModelsEvt, (e) => {
  if (!e || !show.value) return   // 面板没开：事件作废（重开面板是新编辑态）
  fetchingModels.value = false
  if (!e.ok) { fetchMsg.value = '获取失败：' + e.error; return }
  const p = cur()
  const known = new Set(p.models.map(m => (m.id || '').toLowerCase()))
  // 「最新 10 个」口径：模型 ID 尾部 6 位日期后缀（YYMMDD，ark 等平台惯例）
  // 降序在前，无后缀的按平台返回原序排其后。ark /models 一次返回 138 个（混着
  // 两年前的老模型），全收只会刷爆列表——只收前 10，其余仍可手动添加。
  const dateOf = (s) => { const m = /(\d{6})\D*$/.exec(s); return m ? m[1] : '' }
  const fresh = e.models.map(String).filter(id => !known.has(id.toLowerCase()))
  fresh.sort((a, b) => (dateOf(b) || '000000').localeCompare(dateOf(a) || '000000'))
  const pick = fresh.slice(0, 10)
  for (const id of pick)
    p.models.push({ id, context: DEFAULT_CONTEXT, enabled: true })   // 平台不给窗口值，落默认可再编辑
  const clipped = fresh.length > pick.length ? `，已截取最新 ${pick.length} 个（其余 ${fresh.length - pick.length} 个可手动添加）` : ''
  fetchMsg.value = `平台返回 ${e.models.length} 个模型，新增 ${pick.length} 个${clipped}`
})

async function openPanel() {
  // 从 store 深拷贝（structuredClone：纯数据，JSON 往返对 undefined/循环引用更脆）；
  // api_key 输入框恒空（后端掩码，留空=保留旧值）
  detail.value = structuredClone(S.providers).map(p => ({ ...p, api_key: '', models: p.models || [] }))
  curName.value = S.current
  provSel.value = Math.max(0, detail.value.findIndex(p => p.name === S.current))
  inferPreset()   // 模板选择器显示与当前 base_url 匹配的平台（切换供应商时经 watch 跟随）
  fetchMsg.value = ''
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
// 子 agent 模型候选：全部启用供应商的启用模型（排除正在编辑的模型自身——自己
// 派生自己无意义）。选项值用 JSON 串做键：供应商名可含任意字符不会拆坏。
const subOptions = computed(() => {
  const out = []
  const self = cur()
  const selfMid = mEdit.value && mEdit.value.mi >= 0 ? self.models[mEdit.value.mi]?.id : mId.value
  for (const p of detail.value || []) {
    if (p.enabled === false || p.type === 'mock') continue
    for (const m of p.models || []) {
      if (m.enabled === false) continue
      if (p.name === self?.name && m.id === selfMid) continue
      out.push({ key: JSON.stringify({ provider: p.name, model: m.id }),
                 label: p.name + ' / ' + m.id })
    }
  }
  return out
})
// 子 agent 模型候选的 Cdl 形态：默认项 + 失效引用项（存在才出现）+ 按供应商
// 分组的候选（节头 = 供应商名，与输入栏选择器同一视觉）
const subItems = computed(() => {
  const out = [{ value: '', label: '默认（自己）' }]
  if (mSubDangling.value)
    out.push({ value: mSub.value, label: mSub.value + '（引用已失效）' })
  for (const o of subOptions.value) {
    let provider = ''
    try { provider = JSON.parse(o.key).provider } catch {}
    out.push({ value: o.key, label: o.label, group: provider })
  }
  return out
})
function openModelDlg(mi) {
  mEdit.value = { mi }
  const m = mi >= 0 ? cur().models[mi] : { id: '', context: DEFAULT_CONTEXT }
  mId.value = m.id
  mCtx.value = fmtCtx(m.context)
  const sub = m.sub_model
  mSub.value = sub && sub.provider && sub.model
    ? JSON.stringify({ provider: sub.provider, model: sub.model }) : ''
  mSubDangling.value = mSub.value && !subOptions.value.some(o => o.key === mSub.value)
    ? sub.provider + ' / ' + sub.model : ''
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
  // 子模型引用：'' = 默认自己（不落字段）；存在性校验在后端（保存时拦截悬空引用）
  let sub
  if (mSub.value) {
    try { sub = JSON.parse(mSub.value) } catch { sub = null }
    if (!sub || !sub.provider || !sub.model) { mErr.value = '子 agent 模型引用非法'; return }
  }
  const base = { id, context: ctx }
  if (sub) base.sub_model = { provider: sub.provider, model: sub.model }
  if (mEdit.value.mi >= 0)
    p.models[mEdit.value.mi] = { ...base, enabled: p.models[mEdit.value.mi].enabled }
  else
    p.models.push({ ...base, enabled: true })
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
            <span class="dot" :class="p.enabled === false ? 'off' : 'on'" :title="p.enabled === false ? '未启用' : '已启用'"></span>
          </div>
          <div class="grp">自定义供应商</div>
          <div v-for="(p, i) in detail.filter(x => !x.builtin)" :key="'c' + i"
               class="prov" :class="{ sel: detail.indexOf(p) === provSel }"
               @click="provSel = detail.indexOf(p)">
            <span class="act" style="color: var(--acc); cursor: default">◆</span>
            <span>{{ p.name || '(未命名)' }}</span>
            <span class="dot" :class="p.enabled === false ? 'off' : 'on'" :title="p.enabled === false ? '未启用' : '已启用'"></span>
          </div>
          <div class="prov" style="color: var(--acc)" @click="addProvider">＋ 添加供应商</div>
        </div>
        <!-- 右：详情 -->
        <div class="pdetail" v-if="cur()">
          <div class="dhead">
            <span class="name">{{ cur().name || '（未命名供应商）' }}</span>
            <span class="spacer"></span>
            <span class="tgl" :class="{ off: !cur().enabled }" title="启用该供应商"
                  @click="cur().enabled = !cur().enabled"></span>
            <span v-if="!cur().builtin" class="act" title="删除供应商" @click="delProvider">✕</span>
          </div>
          <template v-if="cur().type !== 'mock'">
            <label>快速接入模板
              <span style="color:var(--dim)">（选平台自动填 Base URL 与常用模型，只需再粘贴 API Key；套用会覆盖模型列表）</span>
            </label>
            <Cdl block down bordered :items="presetOptions" v-model="presetSel"
                 placeholder="选择平台模板…（自定义平台可跳过直接手填）"
                 @select="applyPreset" style="width:100%" />
          </template>
          <label>名称</label>
          <input v-model="cur().name" :disabled="cur().builtin" spellcheck="false"
                 :placeholder="cur().builtin ? undefined : '如：火山方舟 Coding Plan'">
          <template v-if="cur().type !== 'mock'">
            <label>Base URL</label>
            <input v-model="cur().base_url" spellcheck="false" placeholder="https://api.deepseek.com">
            <label>API Key</label>
            <input type="password" v-model="keyInputs[cur().name || '']"
                   :placeholder="cur().has_key ? '已设置（留空 = 不修改）' : '未设置'">
          </template>
          <label style="display:flex; justify-content:space-between; align-items:center; margin-top:14px;">
            <span>模型列表</span>
            <span>
              <span v-if="cur().type !== 'mock'" class="addm" style="margin-right:10px"
                    title="按 Base URL + API Key 请求平台 GET /models（OpenAI 兼容），合并进列表"
                    @click="fetchModels">{{ fetchingModels ? '⟳ 获取中…' : '⟳ 从 API 获取' }}</span>
              <span class="addm" @click="openModelDlg(-1)">＋ 添加模型</span>
            </span>
          </label>
          <div v-for="(m, mi) in cur().models" :key="mi" class="mrow">
            <span class="mid">{{ m.id }}</span>
            <span class="badge" :title="'上下文窗口 ' + m.context.toLocaleString() + ' tokens'">{{ fmtCtx(m.context) }}</span>
            <span v-if="m.sub_model" class="badge" :title="'子 agent 模型：' + m.sub_model.provider + ' / ' + m.sub_model.model">⇒ {{ m.sub_model.model }}</span>
            <span class="tgl" :class="{ off: m.enabled === false }" title="启用（关闭后不出现在对话下拉）"
                  @click="m.enabled = m.enabled === false"></span>
            <span class="act" title="编辑模型配置" @click="openModelDlg(mi)">✎</span>
            <span class="act" title="删除模型" @click="cur().models.splice(mi, 1)">✕</span>
          </div>
          <div v-if="fetchMsg" class="snote">{{ fetchMsg }}</div>
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
    <!-- 插件贡献页：schema 动态渲染，各插件设置即插即现 -->
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
        <label>子 agent 模型 <span style="color:var(--dim)">（该模型派发的子 agent 用哪个模型跑；默认 = 自己）</span></label>
        <Cdl block bordered :items="subItems" v-model="mSub" style="width:100%" />
        <div v-if="mErr" style="color:var(--red); font-size:12px">{{ mErr }}</div>
        <div class="sops">
          <button @click="mDlg = false">取消</button>
          <button class="primary" @click="saveModel">保存</button>
        </div>
      </div>
    </NModal>
  </NModal>
</template>
