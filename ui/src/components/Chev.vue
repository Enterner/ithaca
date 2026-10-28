<script setup>
// 折叠指示箭头（SVG，全仓统一出处）：closed 朝右、open 朝下，旋转过渡。
// 会话流的思考块/工具卡/编辑行等可折叠块共用，替代散落的 ▸▾ 文本三角——
// 文本三角无过渡动画且基线随字体漂移。
defineProps({ open: { type: Boolean, default: false } })
</script>

<template>
  <svg class="chev" :class="{ open }" width="12" height="12" viewBox="0 0 24 24" fill="none"
       stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round"
       aria-hidden="true">
    <path d="M9 6l6 6-6 6" />
  </svg>
</template>

<style scoped>
/* 颜色随语境（继承）：折叠块的箭头着所在头行的语义色（琥珀/紫），不再钉灰 */
.chev { flex: none; color: inherit; transition: transform .2s ease; }
.chev.open { transform: rotate(90deg); }
</style>
