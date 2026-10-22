// 暗色主题的确认对话框（替代原生 confirm——白底蓝按钮与主题割裂）。
// 用法：await confirmDialog({ title, content }) → true（确认）/ false（取消）。
import { createDiscreteApi, darkTheme } from 'naive-ui'
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
