<script setup>
// 自绘下拉（替换原生 select 的统一形态）：8px 圆角方框面板、6px 圆角悬浮项、
// 选中琥珀字 + ✓、向上弹、点外关闭、↑↓/Enter/Esc 键盘导航。
// 分组：项带 group 字段 ⇒ 同名连续项归一组，组名渲染为不可点节头（键盘导航
// 只在项上走）。
import { ref, computed, onMounted, onBeforeUnmount } from 'vue'
const props = defineProps({
  items: { type: Array, default: () => [] },   // [{value,label,sub,group?,icon?,desc?}]
  modelValue: { type: String, default: '' },
  amber: { type: Boolean, default: false },
  alignRight: { type: Boolean, default: false },
  leadIcon: { type: String, default: '' },   // 触发器按钮内的前导图标（与文字同一悬浮框）
  down: { type: Boolean, default: false },   // 菜单向下弹（触发器靠上时用；默认向上）
  block: { type: Boolean, default: false },  // 通栏：按钮与菜单吃满父宽（设置面表单用）
  bordered: { type: Boolean, default: false },   // 表单形态：触发器常态带边框（设置面用）
  placeholder: { type: String, default: '' },   // 当前值不在清单时的占位文案
})
const emit = defineEmits(['update:modelValue', 'select'])
const open = ref(false)
const hl = ref(0)
const host = ref(null)
// 当前选中项：找不到 = 占位态（placeholder 有值显示占位，否则裸显 value）
const cur = computed(() => props.items.find(i => i.value === props.modelValue))
const lbl = computed(() => cur.value ? cur.value.label : (props.placeholder || props.modelValue))
const showPh = computed(() => !cur.value && !!props.placeholder)
// 渲染行 = 分组头 + 项；idx = 项在 items 里的序号（hl 的坐标系）
const rows = computed(() => {
  const out = []
  let last, n = 0
  for (const it of props.items) {
    if (it.group !== undefined) {
      if (it.group !== last) { out.push({ head: it.group }); last = it.group }
    } else {
      last = undefined
    }
    out.push({ ...it, idx: n++ })
  }
  return out
})
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
  <span ref="host" class="dd" :class="{ amber, 'dd-block': block, 'dd-bordered': bordered }">
    <button type="button" class="dd-btn" @click.stop="toggle" @keydown="onKey">
      <span v-if="leadIcon" class="dd-lead" v-html="leadIcon"></span>
      <span :class="{ ph: showPh }">{{ lbl }}</span><span class="chev">▾</span>
    </button>
    <div class="dd-menu" :class="{ open, right: alignRight, down, block }">
      <template v-for="(r, i) in rows" :key="r.head !== undefined ? 'h' + i : r.value">
        <div v-if="r.head !== undefined" class="dd-ghead">{{ r.head }}</div>
        <div v-else class="dd-item"
             :class="{ cur: r.value === modelValue, hl: r.idx === hl }" @click.stop="pick(r.value)">
          <span v-if="r.icon" class="dd-ico" v-html="r.icon"></span>
          <span class="dd-main" :class="{ col: r.desc }">
            <span>{{ r.label }}</span>
            <small v-if="r.desc" class="dd-desc">{{ r.desc }}</small>
          </span>
          <small v-if="r.sub">{{ r.sub }}</small>
          <span v-if="r.value === modelValue" class="tick">✓</span>
        </div>
      </template>
    </div>
  </span>
</template>
