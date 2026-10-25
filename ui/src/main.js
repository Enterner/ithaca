// 应用入口：挂载根组件 + 全局样式，无业务逻辑（状态与行为见 src/store/）。
import { createApp } from 'vue'
import App from './App.vue'
import './global.css'
import { S, X } from './store'

createApp(App).mount('#app')
// NOTE: 控制台调试口——生产构建保留（体积忽略）；排查记录流/配对状态用
window.__ithaca = { S, X }
