<script setup>
// 自绘下拉（替换原生 select 的统一形态）：8px 圆角方框面板、6px 圆角悬浮项、
// 选中琥珀字 + ✓、向上弹、点外关闭、↑↓/Enter/Esc 键盘导航。
import { ref, onMounted, onBeforeUnmount } from 'vue'
const props = defineProps({
  items: { type: Array, default: () => [] },   // [{value,label,sub}]
  modelValue: { type: String, default: '' },
  amber: { type: Boolean, default: false },
  alignRight: { type: Boolean, default: false },
})
const emit = defineEmits(['update:modelValue', 'select'])
const open = ref(false)
const hl = ref(0)
const host = ref(null)
function label() {
  const it = props.items.find(i => i.value === props.modelValue)
  return it ? it.label : props.modelValue
}
function renderHl() {
  hl.value = Math.max(0, props.items.findIndex(i => i.value === props.modelValue))
}
function toggle() { open.value = !open.value; if (open.value) renderHl() }
function pick(v) {
  open.value = false
  if (v !== props.modelValue) {
    emit('update:modelValue', v)
    emit('select', v)
  }
}
function onDoc(e) { if (open.value && host.value && !host.value.contains(e.target)) open.value = false }
function onKey(e) {
  if (!open.value) {
    if (['ArrowDown', 'Enter', ' '].includes(e.key)) { open.value = true; renderHl(); e.preventDefault() }
    return
  }
  const n = props.items.length
  if (!n) return
  if (e.key === 'ArrowDown') { hl.value = (hl.value + 1) % n; e.preventDefault() }
  else if (e.key === 'ArrowUp') { hl.value = (hl.value - 1 + n) % n; e.preventDefault() }
  else if (e.key === 'Enter') { if (props.items[hl.value]) pick(props.items[hl.value].value); e.preventDefault() }
  else if (e.key === 'Escape') { open.value = false; e.preventDefault() }
}
onMounted(() => document.addEventListener('click', onDoc))
onBeforeUnmount(() => document.removeEventListener('click', onDoc))
</script>

<template>
  <span ref="host" class="dd" :class="{ amber }">
    <button type="button" class="dd-btn" @click.stop="toggle" @keydown="onKey">
      <span>{{ label() }}</span><span class="chev">▾</span>
    </button>
    <div class="dd-menu" :class="{ open, right: alignRight }">
      <div v-for="(it, i) in items" :key="it.value" class="dd-item"
           :class="{ cur: it.value === modelValue, hl: i === hl }" @click.stop="pick(it.value)">
        <span>{{ it.label }}</span>
        <small v-if="it.sub">{{ it.sub }}</small>
        <span v-if="it.value === modelValue" class="tick">✓</span>
      </div>
    </div>
  </span>
</template>
