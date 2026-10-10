// 暗色主题的确认对话框（替代原生 confirm——白底蓝按钮与主题割裂）。
// 用法：await confirmDialog({ title, content }) → true（确认）/ false（取消）。
import { createDiscreteApi, darkTheme } from 'naive-ui'

const { dialog } = createDiscreteApi(['dialog'], {
  configProviderProps: {
    theme: darkTheme,
    themeOverrides: {
      common: {
        primaryColor: '#6366F1', primaryColorHover: '#7B83F5', primaryColorPressed: '#5257E0',
        bodyColor: '#0B0E15', cardColor: '#12171F', modalColor: '#12171F',
        borderColor: '#222B3B', textColorBase: '#E6EAF2',
      },
      Card: { borderRadius: '14px' },
      Dialog: { borderRadius: '14px' },
    },
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
