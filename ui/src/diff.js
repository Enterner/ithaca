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

// ---- 查看器对照模式（docs/查看器diff模式-方案.md §2/§3/§7）------------------------
// 自绘管线 = 现盘全文 + hunk 标记，经 diffHtml 渲染（与会话卡同管线同样式）：
// 未修改行即纯源码（仅行号 gutter），修改行红绿底 + 词级加重。CodeViewer.vue 按
// 新鲜度路由（§7.1）：新鲜态走 Monaco diff editor（语法高亮），这里服务陈旧降级
// 分支（文件已再次变更 / read 截断）。

/** diff 文本 → 结构化 hunk。rows 保留 hunk 内原始行序（ctx/del/add 交错），
 *  fullTextDiff 展开全文用；oldLines/newLines 是按侧视图，reverseApply 校验用。 */
export function parseHunks(diffText) {
  const hunks = []; let cur = null
  for (const l of diffText.split('\n')) {
    const h = l.match(/^@@ -(\d+),(\d+) \+(\d+),(\d+) @@/)
    if (h) {
      cur = { a: +h[1], n: +h[2], b: +h[3], m: +h[4], oldLines: [], newLines: [], rows: [] }
      hunks.push(cur)
      continue
    }
    if (!cur || !l || l.startsWith('──') || l.startsWith('（')) continue
    // 行格式：行号 + 3 空格 = ctx；行号 + " - " / " + " = del/add（regex 首个 \d++空格
    // 吃掉一个分隔空格后，ctx 还剩 2 空格、del/add 剩标记）
    const r = l.match(/^\s*(\d+) (- |\+ |  )(.*)$/)
    if (!r) continue
    if (r[2] === '- ') {
      cur.oldLines.push(r[3]); cur.rows.push({ t: 'del', text: r[3], oldNo: +r[1] })
    } else if (r[2] === '+ ') {
      cur.newLines.push(r[3]); cur.rows.push({ t: 'add', text: r[3], newNo: +r[1] })
    } else {
      cur.oldLines.push(r[3]); cur.newLines.push(r[3])
      cur.rows.push({ t: 'ctx', text: r[3], newNo: +r[1] })
    }
  }
  return hunks
}

/** 现盘内容 + hunk（逆序）→ 编辑前全文。任一 hunk 的 modified 侧对不上现盘 ⇒
 *  {error}（文件在 diff 之后又被改过 / read 截断）。查看器对照的新鲜度裁决：
 *  成功 ⇒ original 全文喂 Monaco diff editor；失败 ⇒ 降级自绘管线。 */
export function reverseApply(modified, hunks) {
  const lines = modified.split('\n')
  for (let k = hunks.length - 1; k >= 0; k--) {
    const h = hunks[k]
    const seg = lines.slice(h.b - 1, h.b - 1 + h.m)
    if (seg.length !== h.m || h.newLines.length !== h.m ||
        seg.some((l, t) => l !== h.newLines[t]))
      return { error: 'diff 与现盘不一致（文件可能已再次变更）' }
    lines.splice(h.b - 1, h.m, ...h.oldLines)
  }
  return { original: lines.join('\n') }
}

const p4 = (n) => String(n).padStart(4, ' ')

/** hunk 展开 → 全文 Ithaca diff 行序列（喂 diffHtml 渲染对照）。
 *  hunk 外 = 现盘源码行（ctx，仅行号 gutter 无底色）；hunk 内 = rows 原序
 *  （del/add/ctx，内容是 diff 记录的历史事实）。hunk 间现盘行按行号游标补齐；
 *  旧 diff 的行号与现盘错位时（文件已再次变更）渲染可能重复或偏移——调用方
 *  以 reverseApply 的校验结果提示用户。 */
export function fullTextDiff(modified, hunks) {
  const modLines = modified.split('\n')
  const out = []
  let cur = 1   // 现盘 1-based 行游标
  for (const h of hunks) {
    const b = Math.max(cur, Math.min(h.b, modLines.length + 1))
    for (; cur < b && cur <= modLines.length; cur++)
      out.push(p4(cur) + '   ' + modLines[cur - 1])
    for (const row of h.rows)
      out.push(row.t === 'del' ? p4(row.oldNo ?? cur) + ' - ' + row.text
             : row.t === 'add' ? p4(row.newNo ?? cur) + ' + ' + row.text
             : p4(row.newNo ?? cur) + '   ' + row.text)
    cur = h.b + h.m
  }
  for (; cur <= modLines.length; cur++) out.push(p4(cur) + '   ' + modLines[cur - 1])
  return out.join('\n')
}
