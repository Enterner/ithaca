// 用户动作：发送（草稿态 = /api/session/new 创建即初始化，唯一出口）、裁决、
// 模型/权限热切、供应商两层配置保存、原生选目录。写对话流的只有 pushLocal
// （本模块零渲染决策——记录进出场归 records 状态机）。
import { S } from './state'
import { api, cfgFromServer, shellMsg } from './api'
import { pushLocal } from './records'
import { loadSessions } from './sessions'

/**
 * 发送输入：草稿态 = 调 /api/session/new 创建即初始化（唯一出口）；已初始化会话 =
 * /api/input。/help 本地应答；/undo /cancel 映射为裁决。
 */
export async function send(text) {
  const line = (text || '').trim()
  if (!line) return
  S.inputText = ''
  S.inFlight = true
  if (S.draft) {   // 草稿态首条消息 = 创建会话（唯一出口；草稿期前端零创建请求）
    const ws = S.draft.workspace || ''
    if (S.shell) {
      // 多实例：会话 = 新实例。壳按 ws 拉起实例（工作区注入 env）并回发
      // navigate（url 带 ?ws= 预填草稿）——首条消息在新实例的页面上发送
      if (!ws) {
        S.inFlight = false
        pushLocal('请先在上方选择工作文件夹，再发送首条消息')
        S.inFlight = true   // 保持草稿态视觉（输入可继续改）
        return
      }
      S.draft = null
      S.inFlight = false
      shellMsg({ type: 'new-session', ws })
      return
    }
    S.draft = null
    const r = await api('/api/session/new', ws ? { workspace: ws, message: line } : { message: line })
    if (!r.ok) {   // 创建失败（如目录不可建）：回到草稿态，消息留在输入框
      S.draft = { workspace: ws }
      S.inputText = line
      S.inFlight = false
      pushLocal('创建会话失败：' + (r.error || '未知错误'))
    }
    return
  }
  if (line.startsWith('/')) {   // 本地命令映射为裁决（草稿态无 turn，不拦）
    if (line === '/undo') return decide(3)
    if (line === '/cancel') return decide(4)
    if (line === '/help') {   // 本地伪记录应答：命令一览，不走 agent（发键即收口）
      pushLocal('/undo 撤销上一 turn\n/cancel 取消当前 turn\n/help 显示本帮助')
      S.inFlight = false
      return
    }
  }
  await api('/api/input', { line })
}

/** 裁决提案：0 批准所选 / 1 拒绝 / 2 免批 / 3 撤销 / 4 取消；成功即收起决策坞。 */
export async function decide(d, paths, msg) {
  const body = { decision: d }
  if (paths) body.paths = paths
  if (msg) body.msg = msg
  const r = await api('/api/decision', body)
  if (r.ok && d >= 0 && d <= 2 && S.pending) {   // 该提案已裁决 ⇒ 决策坞收起
    S.pending._decided = d
    S.pending = null
  } else if (r.ok && d === 4) {
    S.pending = null   // 取消 turn：挂起的提案随之作废（卡片保留展示）
    // NOTE: 取消后 provider 静默终止（无 done/error 事件），inFlight 无人清零
    // ⇒ 发送键永远停在停止态——这里自行解锁并收起流式块（踩过）
    S.inFlight = false
    S.live = null
  }
  return r
}

/**
 * 模型热切：provider 缺省 = 当前供应商内切换；带 provider = 跨供应商热切
 * （后端校验存在并启用，随 /api/model 一并持久化 + 凭据/窗口热切）。
 * 成功后回拉配置对齐（current/model/掩码），失败静默——后端会落盘通告。
 */
export async function setModel(model, provider) {
  const body = { model }
  if (provider) body.provider = provider
  const r = await api('/api/model', body)
  if (r.ok) await reloadConfig()
}
/** 重新拉取 /api/config 并对齐前端形状（setModel/saveConfig/savePluginVals 共用）。 */
async function reloadConfig() {
  cfgFromServer(await api('/api/config'))
}

/** 权限模式切换（通告由 agent 在值真正变化时发，这里不重复发）。 */
export async function setMode(mode) {
  await api('/api/permission-mode', { mode })
}
/** 保存供应商两层配置；成功即回拉对齐 + loadSessions（面板关闭即反馈）。 */
export async function saveConfig(providers, current) {
  const body = {
    providers: providers.map(p => ({
      name: (p.name || '').trim(), type: p.type, builtin: p.builtin,
      base_url: (p.base_url || '').trim(), api_key: p.api_key || '',
      enabled: p.enabled, models: (p.models || []).filter(m => m.id)
    })),
    current_provider: current
  }
  const r = await api('/api/config', body)
  if (!r.ok) return { ok: false, error: r.error }
  await reloadConfig()   // 与后端对齐（掩码/回落）
  loadSessions()   // 成功反馈 = 面板关闭；不向对话流插行
  return { ok: true }
}

/** 弹原生目录选择；结果经 SSE folder_picked 事件回页面。 */
export async function pickFolder() {
  // NOTE: 必须 POST——api() 无 body 会发 GET（踩过：hero 选目录按钮 404 静默无效）
  await api('/api/pick-folder', {})
}

// ---- 插件 UI 贡献----------------------------------------------------

/** 拉各插件的 UI 贡献清单（schema，"界面长什么样"）；设置面板打开时调用。 */
export async function loadPluginUi() {
  const r = await api('/api/ui/registry')
  if (r.ok) S.pluginUi = r.plugins || []
}

/** 保存各插件设置值（plugins 节整节回传；后端透传并触发对应插件 reload）。 */
export async function savePluginVals() {
  const r = await api('/api/config', { plugins: S.pluginVals })
  if (r.ok) await reloadConfig()   // 与磁盘对齐（防并发漂移）
  return r
}
