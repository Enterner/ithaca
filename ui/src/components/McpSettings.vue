<script setup>
// 设置面板 · MCP 工具页：server 两层配置（左列表右详情），与模型接入同构。
// 数据源 = config.json 的 mcp.servers（保存 → /api/config → mcp 插件热重载）；
// 状态行 = /api/mcp/status（mcp 插件回发的实时连接状态与工具清单）。
import { ref, computed, watch } from 'vue'
import { fetchConfig, fetchMcpStatus, saveMcpVals } from '../store'
import { confirmDialog } from '../confirm'

const props = defineProps({ active: { type: Boolean, default: false } })
defineEmits(['close'])

const servers = ref([])   // 编辑草稿 [{name, command, args:[], env:{}, enabled}]
const sel = ref(0)
const status = ref({})    // name → {alive, tools:[], diag}（mcp 插件实时状态）
const loaded = ref(false)

const cur = () => servers.value[sel.value]

async function load() {
  const cfg = await fetchConfig()
  servers.value = structuredClone(cfg.mcp?.servers || [])
  await refreshStatus()
}
async function refreshStatus() {
  const st = await fetchMcpStatus()
  const map = {}
  for (const s of st.servers || []) map[s.name] = s
  status.value = map
}
watch(() => props.active, (on) => { if (on) load() }, { immediate: true })   // 每次切入都重拉（配置/状态可能已变）

function addServer() {
  servers.value.push({ name: '', command: 'node', args: [], env: {}, enabled: true })
  sel.value = servers.value.length - 1
}
async function delServer() {
  const s = cur()
  if (!(await confirmDialog({
    title: '删除 server',
    content: '「' + (s.name || '未命名') + '」将从列表移除（点保存后生效）。',
    positive: '移除',
  }))) return
  servers.value.splice(sel.value, 1)
  sel.value = Math.max(0, Math.min(sel.value, servers.value.length - 1))
}

// args ↔ 多行文本（每行一个参数）
function argsText(s) { return (s.args || []).join('\n') }
function setArgs(s, text) {
  s.args = text.split('\n').map((l) => l.trim()).filter((l) => l !== '')
}
// env ↔ KEY=VALUE 行
function envText(s) {
  return Object.entries(s.env || {}).map(([k, v]) => k + '=' + v).join('\n')
}
function setEnv(s, text) {
  const env = {}
  for (const line of text.split('\n')) {
    const l = line.trim(); if (!l) continue
    const eq = l.indexOf('='); if (eq <= 0) continue
    env[l.slice(0, eq).trim()] = l.slice(eq + 1).trim()
  }
  s.env = env
}

const stOf = (name) => status.value[name]
function chipOf(s) {
  if (s.enabled === false) return { cls: 'err', text: '已停用' }
  const st = stOf(s.name)
  if (st && st.alive) return { cls: 'ok', text: '已连接 · 工具 ' + (st.tools || []).length + ' 个' }
  if (st && st.diag) return { cls: 'err', text: '启动失败' }
  return { cls: 'off', text: '未连接' }
}
const toolsOf = (name) => stOf(name)?.tools || []
const diagOf = (name) => stOf(name)?.diag || ''

function dotCls(s) {
  if (s.enabled === false) return 'dis'
  const st = stOf(s.name)
  return st && st.alive ? 'on' : 'off'
}

async function onSave() {
  for (const s of servers.value) {
    if (!s.name || /[^A-Za-z0-9_-]/.test(s.name)) {
      alert('server 名称非法（仅限字母数字下划线连字符）：' + (s.name || '(空)'))
      return
    }
  }
  const r = await saveMcpVals(servers.value)
  if (!r.ok) { alert('保存失败：' + (r.error || '未知错误')); return }
  await load()   // 保存成功 = mcp 插件已热重载，重拉配置与状态
}
function onCancel() { load() }   // 取消 = 丢弃草稿重拉
</script>

<template>
  <div class="nbset">
    <div class="pwrap">
      <!-- 左：MCP server 列表 -->
      <div class="pside">
        <div class="grp">MCP SERVER</div>
        <div v-for="(s, i) in servers" :key="'s' + i" class="prov"
             :class="{ sel: i === sel }" @click="sel = i">
          <span class="ic" style="color: var(--acc); cursor: default">⛓</span>
          <span class="nm">{{ s.name || '(未命名)' }}</span>
          <span class="dot" :class="dotCls(s)"></span>
        </div>
        <div class="addserver" @click="addServer">＋ 添加 server</div>
      </div>
      <!-- 右：连接详情 -->
      <div class="pdetail" v-if="cur()">
        <div class="dhead">
          <span class="name">{{ cur().name || '（未命名 server）' }}</span>
          <span class="chip" :class="chipOf(cur()).cls">{{ chipOf(cur()).text }}</span>
          <span class="spacer"></span>
          <span class="tgl" :class="{ off: cur().enabled === false }" title="启用该 server"
                @click="cur().enabled = cur().enabled === false"></span>
          <span class="act" title="删除 server" @click="delServer">✕</span>
        </div>
        <label>名称 <span class="hint">（工具名前缀 mcp.{{ cur().name || 'server' }}.*，仅限字母数字下划线连字符）</span></label>
        <input v-model="cur().name" spellcheck="false" placeholder="如：fs">
        <label>启动命令</label>
        <input v-model="cur().command" spellcheck="false" placeholder="node / cmd.exe / npx …">
        <label>参数 <span class="hint">（每行一个；server 入口与允许目录）</span></label>
        <textarea :value="argsText(cur())" @input="setArgs(cur(), $event.target.value)"
                  spellcheck="false" class="grow"></textarea>
        <label>环境变量 <span class="hint">（可选，每行 KEY=VALUE，覆盖继承值）</span></label>
        <textarea :value="envText(cur())" @input="setEnv(cur(), $event.target.value)"
                  spellcheck="false" placeholder="留空 = 继承宿主环境" class="autoheight"></textarea>
        <label style="margin-top:14px">状态</label>
        <div class="tools">
          <div class="trow" v-for="t in toolsOf(cur().name)" :key="t">{{ t }}</div>
          <div class="trow" v-if="!toolsOf(cur().name).length" style="color:var(--dim2)">（未连接或无工具）</div>
        </div>
        <div class="snote" v-if="diagOf(cur().name)" style="color:var(--red)">{{ diagOf(cur().name) }}</div>
      </div>
      <div class="pdetail" v-else>
        <div class="snote">（还没有 server——点左侧「＋ 添加 server」）</div>
      </div>
    </div>
    <div class="sops">
      <button class="primary" @click="onSave">保存</button>
      <button @click="onCancel">取消</button>
    </div>
  </div>
</template>
