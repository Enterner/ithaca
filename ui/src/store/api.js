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
      id: m.id, context: m.context || 128000, enabled: m.enabled !== false,
      // 子 agent 模型引用（{provider,model}；缺省 = 自己）——形状不齐时丢弃，
      // 让后端校验兜底而不是把坏值带回配置
      ...(m.sub_model && m.sub_model.provider && m.sub_model.model
        ? { sub_model: { provider: m.sub_model.provider, model: m.sub_model.model } } : {})
    }))
  }))
  S.current = cfg.current_provider || (S.providers[0] && S.providers[0].name) || ''
  // 当前供应商停用/无启用模型 ⇒ 回落第一个「启用且带启用模型」的供应商（与后端
  // 保存路径同口径——磁盘遗留的停用当前项也在此纠正显示）；全都不可用 = 空当前
  // （选择器显示「未选择模型」）
  const usable = p => p && p.enabled !== false &&
                      (p.models || []).some(m => m.enabled !== false)
  if (!usable(S.providers.find(p => p.name === S.current))) {
    const first = S.providers.find(usable)
    S.current = first ? first.name : ''
  }
  S.model = cfg.model || 'deepseek-flash'
  S.pmode = cfg.permission_mode || 'auto-edit'
  S.pluginVals = cfg.plugins || {}   // 各插件 schema 的值域（编辑在设置面板）
  // 当前模型不在当前供应商启用列表 ⇒ 回落第一个启用模型
  const en = curProvModels().filter(m => m.enabled !== false)
  if (en.length && !en.some(m => m.id === S.model)) S.model = en[0].id
}

/**
 * 粗略 k 化（全项目 token 值统一口径）：≥1000 → 一位小数截尾 k（整数去 .0）；
 * ≥1e6 → M。例：9355→9.3k、12000→12k、1817969→1.8M；999 以内原样。
 */
export function fmtK(n) {
  n = n || 0
  if (n >= 1e6) {
    const m = Math.floor(n / 1e5) / 10
    return (Number.isInteger(m) ? String(m) : m.toFixed(1)) + 'M'
  }
  if (n >= 1000) {
    const k = Math.floor(n / 100) / 10
    return (Number.isInteger(k) ? String(k) : k.toFixed(1)) + 'k'
  }
  return String(n)
}

/**
 * fetch JSON 助手：带 body ⇒ POST JSON，无 body ⇒ GET。
 * NOTE: POST 端点必须显式传 {}——无 body 会发 GET（踩过：pickFolder 404 静默无效）。
 */
export async function api(path, body) {
  const r = await fetch(path, body ? { method: 'POST', body: JSON.stringify(body) } : undefined)
  return r.json()
}

/**
 * 设置面「从 API 获取模型」：请后端代理请求平台 GET /models（CORS 挡浏览器
 * 直连）。结果经 SSE provider_models 事件回设置面，不在本调用的返回值里。
 */
export function fetchProviderModels(name, baseUrl, apiKey) {
  return api('/api/provider/models', { name, base_url: baseUrl, api_key: apiKey })
}

/** 上下文构成分解（悬停指示器拉取；后端读活跃会话 state.json 估算+实测修正）。 */
export function fetchCtxBreakdown() {
  return api('/api/context/breakdown')
}

/** 壳消息通道（WebView2 宿主）；浏览器/单实例模式下为空操作。 */
export function shellMsg(obj) {
  try { window.chrome?.webview?.postMessage(obj) } catch (e) {}
}
