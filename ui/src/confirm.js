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

export function confirmDialog({ title, content, positive = '删除', negative = '取消',
                                type = 'error' }) {
  // type：error（删除类，红叉红钮）/ warning（危险但非破坏，琥珀警示）——
  // full-auto 开启这类「授权放大」用 error 的红叉语义错位（不是出了错）
  const show = type === 'warning' ? dialog.warning : dialog.error
  return new Promise((resolve) => {
    show({
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
            // IME 组合期 Enter（候选词上屏）不得当作确认——中文会话名会以
            // 未上屏的拼音提交
            if (e.isComposing || e.keyCode === 229) return
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
