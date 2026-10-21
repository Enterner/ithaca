// 应用入口：挂载根组件 + 全局样式，无业务逻辑（状态与行为见 src/store/）。
import { createApp } from 'vue'
import App from './App.vue'
import './global.css'

createApp(App).mount('#app')
