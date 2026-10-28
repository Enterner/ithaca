// 暗色主题的确认对话框（替代原生 confirm——白底蓝按钮与主题割裂）。
// 用法：await confirmDialog({ title, content }) → true（确认）/ false（取消）。
// promptDialog：带单行输入的询问框（替代原生 prompt）→ 输入串 / null（取消）。
import { createDiscreteApi, darkTheme } from 'naive-ui'
import { h } from 'vue'
import { themeOverrides } from './theme'

const { dialog } = createDiscreteApi(['dialog'], {
  configProviderProps: {
    theme: darkTheme,
    themeOverrides,
  },
})

export function confirmDialog({ title, content, positive = '删除', negative = '取消' }) {
  return new Promise((resolve) => {
    dialog.error({
      title, content, positiveText: positive, negativeText: negative,
      onPositiveClick: () => resolve(true),
      onNegativeClick: () => resolve(false),
      onMaskClick: () => resolve(false),
      onClose: () => resolve(false),
    })
  })
}

export function promptDialog({ title, initial = '', placeholder = '', positive = '确定' }) {
  return new Promise((resolve) => {
    let value = initial
    let done = false
    const settle = (v) => { if (!done) { done = true; resolve(v) } }
    dialog.info({
      title,
      positiveText: positive,
      negativeText: '取消',
      onPositiveClick: () => settle(value.trim()),
      onNegativeClick: () => settle(null),
      onMaskClick: () => settle(null),
      onClose: () => settle(null),
      onEsc: () => settle(null),
      content: () => h('div', { style: 'padding-top:4px' }, [
        h('input', {
          class: 'promptdlg-in',
          value: initial,
          placeholder,
          spellcheck: false,
          // h() 的原生输入：v-model 不可用，onInput 手动回写
          onInput: (e) => { value = e.target.value },
          onKeydown: (e) => {
            if (e.key === 'Enter') { e.preventDefault(); settle(value.trim()) }
          },
          style: 'width:100%;box-sizing:border-box;background:var(--ink);border:1px solid var(--line);' +
                 'border-radius:8px;padding:7px 10px;color:var(--text);font-size:13px;outline:none;' +
                 'font-family:inherit',
        }),
      ]),
      // 关闭统一走 settle（naive 的 onClose 覆盖 esc/mask 缺省路径，防 resolve 悬挂）
      style: 'width:min(420px, 92vw)',
    })
  })
}
