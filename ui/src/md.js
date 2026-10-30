// markdown 渲染门面：对话流的唯一渲染入口。
// 引擎可随时替换——实现见 render/engine-marked.js（接口 = render(text) → html；
// 安全契约在引擎内：叶子 esc、raw HTML 丢弃、链接 scheme 白名单）。
// esc 亦经此处导出（Stream.vue/diff.js 的工具摘要转义直接用）。
import { render, esc } from './render/engine-marked.js'

export function mdHtml(text) {
  return render(text)
}
export { esc }

// 工具名 → 中文显示（对话流 ● 卡与右栏明细行共用）。read/list/search/run/
// recall/task 的摘要已是自含动词的中文短句——显示时不再叠英文原名（英文函数名
// 是给模型的，不进对话流）；旧数据 brief 还是原始 JSON（以 { 开头）时回退原名。
// apply/undo 及未知工具（mcp 等）保留「中文名/原名 + 摘要」。
const TOOL_BRIEF_SELF = new Set(['workspace.read', 'workspace.list', 'workspace.search',
                                 'runner.run', 'context.recall', 'task'])
const TOOL_LABELS = { 'workspace.apply': '编辑', 'workspace.undo_turn': '撤销' }
// 家族人话前缀：office.read 这类 raw 全名对人不可读——「使用office工具读取 x」
//（brief 自含动词短语，前缀只补「谁在干活」；brief 回退原 JSON 时不加前缀，
// 保住可诊断性）
const TOOL_FAMILY_PREFIX = { office: '使用office工具' }

// 行内规整（新旧记录同一显示规则——历史 transcript 的旧格式 brief 也走这里）：
// list 的深度 / recall 的分页 offset 是请求参数细节，行内只说动作与对象，参数
// 全量在调用卡的展开区（args_json）；路径人话与后端 human_path 同一规则——根的
// 两种旧形态（"."、占位名词）落到真实工作区路径（ws 缺省退占位），"./" 前缀剥除
function normalizeBrief(tool, brief, ws) {
  if (tool === 'workspace.list') brief = brief.replace(/（深度 \d+）\s*$/, '')
  if (tool === 'workspace.list' || tool === 'workspace.read' || tool === 'office.read') {
    const root = ws || '工作区根目录'
    brief = brief.replace(/^(列目录|读取) (?:\.\/?|工作区根目录)$/, (m, verb) => verb + ' ' + root)
    brief = brief.replace(/^(列目录|读取) \.\//, '$1 ')
  }
  return brief
}

function toolLabel(tool, brief, ws) {
  brief = normalizeBrief(tool, brief, ws)
  if (TOOL_BRIEF_SELF.has(tool) && brief && !brief.startsWith('{')) return ''
  if (brief && !brief.startsWith('{')) {
    const fam = tool.slice(0, tool.indexOf('.'))
    if (TOOL_FAMILY_PREFIX[fam]) return TOOL_FAMILY_PREFIX[fam]
  }
  return TOOL_LABELS[tool] || tool || ''
}

/** 工具名 → 显示名（无摘要可依的场合：提案卡头部等）——仅查中文标签表，无原文回退。 */
export function toolName(tool) {
  return TOOL_LABELS[tool] || tool || ''
}

// label 与 brief 的接缝：中文接中文不加空格（中文排版无词间空格——
// 「使用office工具」+「读取 x.docx」⇒「使用office工具读取 x.docx」），其余留空格
function labelGap(label, brief) {
  if (!label || !brief) return ''
  const cjk = (c) => /[\u2e80-\u9fff\uf900-\ufaff\ufe30-\ufe4f]/.test(c)
  return cjk(label.slice(-1)) && cjk(brief[0]) ? '' : ' '
}

function toolParts(tool, rawBrief, ws) {
  const brief = normalizeBrief(tool, String(rawBrief || ''), ws)
  return { label: toolLabel(tool, brief, ws), brief }
}

/** 工具行 → 展示 HTML（对话流 ● 卡；内容全部经 esc——信任级别与正文一致）。
 *  ws = 当前工作区根（根路径落真实目录用；旧记录重放同样受益）。 */
export function toolLineHtml(tool, rawBrief, ws) {
  const { label, brief } = toolParts(tool, rawBrief, ws)
  return label ? '<b>' + esc(label) + '</b>' + labelGap(label, brief) + esc(brief) : esc(brief)
}

/** 工具行 → 纯文本（右栏子代理明细行；与对话流同一套规整与接缝规则）。 */
export function toolLineText(tool, rawBrief, ws) {
  const { label, brief } = toolParts(tool, rawBrief, ws)
  return label ? label + labelGap(label, brief) + brief : brief
}
