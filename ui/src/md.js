// 受限 markdown：转义优先，只认 加粗/行内代码/代码块/列表（禁 raw HTML）。
// 逐字移植旧 index.html md()——返回 HTML 串，配合 v-html（内容全部经 esc）。
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
