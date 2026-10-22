// Naive UI 主题令牌（唯一出处）：App.vue 全局 Provider 与 confirm.js 离散对话框
// 共用同一组 overrides——调主题只改这里，两处同步生效。
export const themeOverrides = {
  common: {
    primaryColor: '#6366F1', primaryColorHover: '#7B83F5', primaryColorPressed: '#5257E0',
    bodyColor: '#0B0E15', cardColor: '#12171F', modalColor: '#12171F',
    borderColor: '#222B3B', textColorBase: '#E6EAF2',
  },
  Card: { borderRadius: '14px' },
  Dialog: { borderRadius: '14px' },
}
