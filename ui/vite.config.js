import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// dev 模式经代理直连宿主（免 CORS）；构建产物 = 纯静态文件，交给宿主磁盘直出。
// base=/web/：宿主静态路由只发 /web/* 前缀，产物引用必须落在该前缀下。
const target = 'http://127.0.0.1:' + (process.env.ITHACA_WEBUI_PORT || '8790')
export default defineConfig({
  base: '/web/',
  plugins: [vue()],
  server: {
    proxy: {
      '/api': { target, changeOrigin: true },
      '/events': { target, changeOrigin: true },
    },
  },
  build: { outDir: 'dist' },
})
