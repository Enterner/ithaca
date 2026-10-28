// markdown 渲染引擎（marked 实现）——可替换模块：
//   对外唯一接口 render(text) → html；调用方只走 ../md 门面，不感知引擎。
//   安全契约：叶子文本一律 esc；原始 HTML token 丢弃（禁 raw HTML 入流）；
//   链接 scheme 白名单（http/https/mailto + 无 scheme 相对地址），危险 scheme
//   降级为纯文本。样式沿用 .mdh/.codeblk/.ic 等既有类——换引擎不动样式与调用点。
import { marked } from 'marked'

const esc = (s) =>
  String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
           .replace(/>/g, '&gt;').replace(/"/g, '&quot;').replace(/'/g, '&#39;')

// 链接 scheme 白名单：javascript:/vbscript:/data: 等危险形态降级为纯文本
function safeHref(href) {
  const h = String(href || '').trim()
  if (/^(https?:|mailto:)/i.test(h)) return h
  if (!/^[a-z][a-z0-9+.-]*:/i.test(h)) return h   // 无 scheme = 站内相对地址
  return ''
}

marked.use({
  gfm: true,      // 表格/删除线/任务列表/自动链接
  breaks: true,   // 单换行断行（聊天习惯——旧 pre-wrap 渲染的等价形态）
  renderer: {
    // 原始 HTML：转义显示（禁 raw HTML 入流，但内容可见——与旧渲染器一致）
    html({ text }) { return esc(text) },
    text({ text, tokens }) { return tokens ? this.parser.parseInline(tokens) : esc(text) },
    codespan({ text }) { return `<code class="ic">${esc(text)}</code>` },
    code({ text }) {
      return `<div class="codeblk"><button class="cpy" data-c="${encodeURIComponent(text)}">复制</button>` +
             `<pre><code>${esc(text)}</code></pre></div>`
    },
    heading({ tokens, depth }) {
      return `<div class="mdh mdh-${depth}">${this.parser.parseInline(tokens)}</div>`
    },
    paragraph({ tokens }) { return `<p>${this.parser.parseInline(tokens)}</p>` },
    link({ href, tokens }) {
      const safe = safeHref(href)
      return safe ? `<a href="${esc(safe)}" target="_blank" rel="noreferrer">${this.parser.parseInline(tokens)}</a>`
                  : this.parser.parseInline(tokens)
    },
    image({ text, href }) {
      const safe = /^(https?:|data:image\/)/i.test(String(href || '').trim())
      return safe ? `<img src="${esc(href)}" alt="${esc(text)}">` : esc(text)
    },
  },
})

export function render(text) {
  return marked.parse(String(text ?? ''))
}
export { esc }
