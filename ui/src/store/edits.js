// 编辑记录查询（records 域的只读面）：查看器对照模式按 path 找最新折叠 diff。
// 只读不写——记录的生成/聚合在 records.js，这里只暴露按 path 的查询。
import { S } from './state'
import { diffFiles } from '../diff'

/**
 * 返回该 path 最新一条「首 before → 末 after」折叠 diff；无编辑记录则 null。
 * turn_edits 汇总卡优先（后端已把同文件多次编辑折叠为一条多 hunk diff，
 * 其 after = 现盘，反向重建必然自洽）；散条记录兜底——**只认 applied**：
 * proposal 也带 diff 字段但那是「将要」的编辑，混进
 * 兜底会让未应用/旧提案的内容冒充已落盘证据（踩过：批准过的旧提案 diff
 * 命中对照，显示的修改与本轮无关）。早期 applied 可能没带 paths，从 diff
 * 头解析（"── path（…）" 行）。
 * @param {string} path 工作区相对路径（与文件树 entry.path 同形态）
 */
export function diffForPath(path) {
  for (let i = S.records.length - 1; i >= 0; i--) {
    const rec = S.records[i]
    if (rec.type === 'turn_edits') {
      const f = rec.files.find((f) => f.path === path)
      if (f && f.applies.length) return f.applies[f.applies.length - 1]
    } else if (rec.type === 'applied' && rec.diff &&
               (rec.paths || diffFiles(rec.diff)).includes(path)) {
      return rec.diff
    }
  }
  return null
}
