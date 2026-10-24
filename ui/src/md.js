// 受限 markdown：转义优先，只认 加粗/行内代码/代码块/列表（禁 raw HTML）。
// 返回 HTML 串，配合 v-html（内容全部经 esc）。
function esc(s) {
  return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
          .replace(/"/g, '&quot;').replace(/'/g, '&#39;')
}
export function mdHtml(text) {
  let s = esc(text)
  const blocks = []
  s = s.replace(/```(\w*)\n([\s\S]*?)```/g, (m, lang, code) => {
    const i = blocks.push('<div class="codeblk"><button class="cpy" data-c="' +
      encodeURIComponent(code) + '">复制</button><pre><code>' +
      code.replace(/\n$/, '') + '</code></pre></div>') - 1
    return '\uE000MD' + i + '\uE001'
  })
  s = s.replace(/`([^`\n]+)`/g, '<code class="ic">$1</code>')
  s = s.replace(/\*\*([^\n*]+)\*\*/g, '<strong>$1</strong>')
  s = s.replace(/^(\s*)([-*]|\d+\.) (.+)$/gm, (m, ind, mk, item) =>
    '<div class="li" style="margin-left:' + Math.min(4, ind.length) * 14 + 'px">' +
    (mk === '-' || mk === '*' ? '•' : mk) + ' ' + item + '</div>')
  s = s.replace(/\uE000MD(\d+)\uE001/g, (m, i) => blocks[+i])
  return s
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
