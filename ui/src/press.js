// 折叠行拖选豁免（FoldBlock 与 Stream 汇总卡编辑行共用一件）：在可折叠行上拖选
// 文字松开，click 仍会触发折叠切换——mousedown 记起点、click 比位移，>5px 视为
// 选择文字不放行。pos 由调用方持有（组件内 const 对象），读后即清。
export function trackPress(e, pos) { pos.x = e.clientX; pos.y = e.clientY }
export function pressed(e, pos) {
  const x = pos.x
  pos.x = null
  if (x === null) return true
  const dx = e.clientX - x, dy = e.clientY - pos.y
  return dx * dx + dy * dy <= 25
}
