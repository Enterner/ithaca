<script setup>
// 对话流统一折叠块（一个轮子）：工具卡 / 深度思考 / run 兜底块等一切
// 「头行 + 可折叠体」行型共用同一渲染与几何——灰白=过程层、正文纯白无框，
// 头行与折叠体同 12px 文字起线、同 0fr⇄1fr 网格动画（.fold）。
// 拖选豁免（按位移 >5px 判定为选择文字，不触发开合）收编在组件内，调用方只给
// open / 内容。clickable=false 时纯展示头行。
defineProps({
  open: { type: Boolean, default: false },
  clickable: { type: Boolean, default: false },
  warn: { type: Boolean, default: false },
})
const emit = defineEmits(['toggle'])
let pressPos = null
function trackPress(e) { pressPos = { x: e.clientX, y: e.clientY } }
function onClick(e) {
  const p = pressPos
  pressPos = null
  if (p) {
    const dx = e.clientX - p.x, dy = e.clientY - p.y
    if (dx * dx + dy * dy > 25) return   // 拖选文字松开：不是点击
  }
  emit('toggle')
}
</script>

<template>
  <div class="fblock" :class="{ open, click: clickable, warn }"
       @mousedown="trackPress" @click="onClick">
    <div class="fb-head"><slot name="head" /></div>
    <div class="fold" :class="{ open }"><div class="fold-in"><slot /></div></div>
  </div>
</template>
