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
export function toolLabel(tool, brief) {
  if (TOOL_BRIEF_SELF.has(tool) && brief && !String(brief).startsWith('{')) return ''
  return TOOL_LABELS[tool] || tool || ''
}
