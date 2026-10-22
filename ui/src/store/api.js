// 与宿主 HTTP/SSE 契约的交界原语：fetch 助手、/api/config 两层供应商的
// 前端映射与防御（旧启动器 ConvertTo-Json 低 Depth 压坏 models）、
// 供应商模型契约助手、壳（WebView2）消息通道。
import { S } from './state'

/** 当前选中供应商对象（S.current 按名匹配；找不到返回 null）。 */
export function curProv() {
  return S.providers.find(p => p.name === S.current) || null
}
/** 当前供应商的模型列表（无供应商时为空数组）。 */
export function curProvModels() {
  return (curProv() && curProv().models) || []
}
/** 模型的上下文窗口大小（未启用/未列出回落 128000）。 */
export function ctxSizeOf(m) {
  const e = curProvModels().find(x => x.id === m && x.enabled !== false)
  return (e && e.context) || 128000
}
/** token 数格式化：128000 → "128K"，1000000 → "1M"。 */
export function fmtCtx(n) {
  if (n >= 1e6 && n % 1e6 === 0) return (n / 1e6) + 'M'
  if (n >= 1e3 && n % 1e3 === 0) return (n / 1e3) + 'K'
  return String(n)
}
/** "1M"/"256K"/"131072" → tokens（十进制）；非法或 <1024 ⇒ null。 */
export function parseCtx(s) {
  s = (s || '').trim().toUpperCase()
  const mul = s.endsWith('M') ? 1e6 : s.endsWith('K') ? 1e3 : 1
  const n = mul > 1 ? parseFloat(s.slice(0, -1)) : parseFloat(s)
  if (!isFinite(n) || n <= 0) return null
  const t = Math.round(n * mul)
  return t >= 1024 ? t : null
}

/**
 * /api/config 的服务端形状 → S 的前端形状。防御旧启动器压坏的 models（字符串化
 * 的数组按空处理）；当前模型不在启用列表时回落第一个启用模型。
 */
export function cfgFromServer(cfg) {
  S.providers = (cfg.providers || []).map(p => ({
    name: p.name || '', type: p.type || 'cloud', builtin: !!p.builtin,
    base_url: p.base_url || '', enabled: p.enabled !== false, has_key: !!p.has_key,
    // Array.isArray 防御：旧启动器（ConvertTo-Json 低 Depth）会把 models 压成字符串
    models: (Array.isArray(p.models) ? p.models : []).map(m => ({
      id: m.id, context: m.context || 128000, enabled: m.enabled !== false
    }))
  }))
  S.current = cfg.current_provider || (S.providers[0] && S.providers[0].name) || ''
  S.model = cfg.model || 'deepseek-flash'
  S.pmode = cfg.permission_mode || 'auto-edit'
  S.pluginVals = cfg.plugins || {}   // 各插件 schema 的值域（编辑在设置面板）
  // 当前模型不在当前供应商启用列表 ⇒ 回落第一个启用模型
  const en = curProvModels().filter(m => m.enabled !== false)
  if (en.length && !en.some(m => m.id === S.model)) S.model = en[0].id
}

/**
 * fetch JSON 助手：带 body ⇒ POST JSON，无 body ⇒ GET。
 * NOTE: POST 端点必须显式传 {}——无 body 会发 GET（踩过：pickFolder 404 静默无效）。
 */
export async function api(path, body) {
  const r = await fetch(path, body ? { method: 'POST', body: JSON.stringify(body) } : undefined)
  return r.json()
}

/** 壳消息通道（WebView2 宿主）；浏览器/单实例模式下为空操作。 */
export function shellMsg(obj) {
  try { window.chrome?.webview?.postMessage(obj) } catch (e) {}
}
