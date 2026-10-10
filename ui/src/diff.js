// diff 文本 → 着色 HTML（提案卡与汇总卡共用；格式见 docs/edit-diff-方案.md §5）。
// 逐字移植旧 diffSpan/diffChangeRow/diffBodyHtml：行号 gutter、不输出 +/- 记号、
// 连续变更区按位置配对做行内高亮（UTF-16 代理对对齐防劈字）、单 hunk 隐藏 @@ 头。
import { esc } from './md'

function diffSpan(a, b) {
  let p = 0
  while (p < a.length && p < b.length && a[p] === b[p]) p++
  while (p > 0 && a.charCodeAt(p) >= 0xDC00 && a.charCodeAt(p) <= 0xDFFF) p--
  let s = 0
  while (s < a.length - p && s < b.length - p && a[a.length - 1 - s] === b[b.length - 1 - s]) s++
  while (s > 0 && a.charCodeAt(a.length - s) >= 0xDC00 && a.charCodeAt(a.length - s) <= 0xDFFF) s--
  return { pre: a.slice(0, p), midA: a.slice(p, a.length - s),
           midB: b.slice(p, b.length - s), suf: s > 0 ? a.slice(a.length - s) : '' }
}
function diffChangeRow(l, marker, pair) {
  const m = l.match(/^(\s*\d+) ([+-]) ?(.*)$/)
  if (!m) return '<div>' + esc(l) + '</div>'
  let inner = esc(m[3])
  if (pair) {
    const pm = pair.match(/^(\s*\d+) ([+-]) ?(.*)$/)
    if (pm) {
      const sp = diffSpan(m[3], pm[3])
      // 本行内容是 diffSpan 第一参 ⇒ 变化子串恒为 midA
      inner = esc(sp.pre) + '<span class="mark">' + esc(sp.midA) + '</span>' + esc(sp.suf)
    }
  }
  return '<div class="' + (marker === '+' ? 'add' : 'del') + '"><span class="lnum">' +
         esc(m[1]) + '</span>' + inner + '</div>'
}
export function diffFiles(diffText) {   // "── path（…）" 行 → 路径数组（逐文件批准用）
  const files = []
  diffText.split('\n').forEach((l) => {
    if (l.startsWith('── '))
      files.push(l.substring(3).replace(/（[^）]*）\s*$/, '').trim())
  })
  return files
}
export function stripHeads(diffText) {   // 多文件提案：头部行改由 Vue 复选框渲染
  return diffText.split('\n').filter(l => !l.startsWith('──')).join('\n')
}
export function diffHtml(diffText) {
  const lines = diffText.split('\n').filter((l) => l !== '')
  const classify = (l) => {
    if (l.startsWith('──')) return 'head'
    if (l.startsWith('@@') || l.startsWith('（')) return 'meta'
    if (/^ {0,3}\d+ [+-] /.test(l)) return l.match(/^ {0,3}\d+ ([+-])/)[1] === '+' ? 'add' : 'del'
    if (/^ {0,3}\d+ {2}[^ ]/.test(l) || /^ {0,3}\d+ {3}/.test(l)) return 'ctx'
    return 'other'
  }
  const atCount = lines.filter((l) => l.startsWith('@@')).length
  const hideMeta = atCount <= 1
  let html = ''
  let i = 0
  while (i < lines.length) {
    const l = lines[i], k = classify(l)
    if (k !== 'add' && k !== 'del') {
      if (k === 'head') html += '<div class="fhead">' + esc(l) + '</div>'
      else if (k === 'meta') {
        if (!hideMeta) html += '<div class="hunk">' + esc(l) + '</div>'
      } else if (k === 'ctx') {
        const c = l.match(/^(\s*\d+) {3}(.*)$/)
        html += '<div class="ctx"><span class="lnum">' + esc(c[1]) + '</span>' + esc(c[2]) + '</div>'
      } else html += '<div>' + esc(l) + '</div>'
      i++
      continue
    }
    // 连续变更区：del/add 收集成组后按位置配对
    let j = i; const dels = [], adds = []
    while (j < lines.length) {
      const kk = classify(lines[j])
      if (kk !== 'add' && kk !== 'del') break
      ;(kk === 'del' ? dels : adds).push(lines[j]); j++
    }
    const n = Math.min(dels.length, adds.length)
    dels.forEach((d, t) => { html += diffChangeRow(d, '-', t < n ? adds[t] : null) })
    adds.forEach((a, t) => { html += diffChangeRow(a, '+', t < n ? dels[t] : null) })
    i = j
  }
  return html
}
