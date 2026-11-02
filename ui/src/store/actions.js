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
 * 失败行为：网络/创建失败时回滚草稿与输入框、复位 inFlight 并 pushLocal 提示，
 * 附件保留待重试。
 */
export async function send(text) {
  const line = (text || '').trim()
  if (!line) return
  // 附件只走结构化数组：图片经 data_url 进 LLM 视觉输入；文档类的 read 指引由
  // agent 组模型语境时按未内联附件补注记——消息文本保持用户原文（对话流直接
  // 渲染 transcript 文本，管道注记混进用户消息就是展示噪音）。创建失败回退时
  // 不清附件，重试仍带上
  const attPaths = S.attachments.map(a => a.path)
  S.inputText = ''
  // 本地命令（/undo /cancel /help）不是 turn：不置 inFlight——否则空闲会话输入
  // /undo 后发送键恒为停止态（inFlight 无人复位：/undo 没有 error/assistant 收口
  // 记录，undone 分支也不复位；/cancel 与 /help 各有出口，唯独它漏）
  if (!(line.startsWith('/') && !S.draft)) S.inFlight = true
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
      S.attachments = []
      shellMsg({ type: 'new-session', ws })
      return
    }
    S.draft = null
    let r
    try {
      r = await api('/api/session/new', ws ? { workspace: ws, message: line, attachments: attPaths } : { message: line, attachments: attPaths })
    } catch (e) {
      // 网络失败（宿主重启瞬间/断连）：回滚草稿与输入，等同创建失败——否则
      // 未捕获 rejection + inFlight 永不复位
      S.draft = { workspace: ws }
      S.inputText = line
      S.inFlight = false
      pushLocal('创建会话失败：网络错误（文字已放回输入框）')
      return
    }
    if (!r.ok) {   // 创建失败（如目录不可建）：回到草稿态，消息留在输入框
      S.draft = { workspace: ws }
      S.inputText = line
      S.inFlight = false
      pushLocal('创建会话失败：' + (r.error || '未知错误'))
    } else {
      S.attachments = []
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
  try {
    await api('/api/input', { line, attachments: attPaths })
    S.attachments = []
  } catch (e) {
    // 发送失败回滚现场：输入框先清后发，网络失败时文字必须放回、inFlight 必须
    // 复位（turn 未发出去，永远等不到收口记录——发送键卡死为停止态）
    S.inputText = line
    S.inFlight = false
    pushLocal('发送失败：网络错误（文字已放回输入框）')
  }
}

/** 裁决提案：0 批准所选 / 1 拒绝 / 2 免批 / 3 撤销 / 4 取消；成功即收起决策坞。
 *  失败行为：r.ok=false 时决策坞与提案原状不动，错误由调用方提示。 */
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
 * 成功后回拉配置对齐（current/model），失败静默——后端会落盘通告。
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

/** 权限模式切换（通告由 agent 在值真正变化时发，这里不重复发）。
 *  失败行为：请求失败返回 {ok:false,error}，本地模式值不动（下次拉配置对齐）。 */
export async function setMode(mode) {
  await api('/api/permission-mode', { mode })
}
/** 保存供应商两层配置；成功即回拉对齐 + loadSessions（面板关闭即反馈）。
 *  失败行为：返回 {ok:false,error}，面板不关、本地编辑态不动。 */
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
  await reloadConfig()   // 与后端对齐（回落）
  loadSessions()   // 成功反馈 = 面板关闭；不向对话流插行
  return { ok: true }
}

/** 弹原生目录选择；结果经 SSE folder_picked 事件回页面。
 *  失败行为：请求本身失败仅得 {ok:false}，无 SSE 回执；不弹本地错误（用户重试即可）。 */
export async function pickFolder() {
  // NOTE: 必须 POST——api() 无 body 会发 GET（踩过: hero 选目录按钮 404 静默无效）
  await api('/api/pick-folder', {})
}

// ---- 插件 UI 贡献----------------------------------------------------

/** 保存 MCP server 配置（mcp 节整节回传；后端透传并热重载 mcp 插件）。
 *  失败行为：透传 api() 结果 {ok,error}，配置不落盘不重载。 */
export async function saveMcpVals(servers) {
  return api('/api/config', { mcp: { servers } })
}

/** 拉各插件的 UI 贡献清单（schema，"界面长什么样"）；设置面板打开时调用。
 *  失败行为：仅 r.ok 时更新 S.pluginUi，失败静默保留旧清单。 */
export async function loadPluginUi() {
  const r = await api('/api/ui/registry')
  if (r.ok) S.pluginUi = r.plugins || []
}

/** 保存各插件设置值（plugins 节整节回传；后端透传并触发对应插件 reload）。
 *  失败行为：返回 r；仅成功时回拉 /api/config 对齐（防并发漂移）。 */
export async function savePluginVals() {
  const r = await api('/api/config', { plugins: S.pluginVals })
  if (r.ok) await reloadConfig()   // 与磁盘对齐（防并发漂移）
  return r
}
