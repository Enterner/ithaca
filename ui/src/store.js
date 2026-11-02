// 桶文件：store 领域拆分（src/store/ 下 state/api/records/sessions/actions/boot）
// 后的统一出口——组件的 `from '../store'` 导入路径保持不变。依赖方向：
// state ← api ← records/sessions ← actions/boot（无环；X 转态容器在 state）。
// 各 export 的职责/失败行为 JSDoc 在源模块，不在此复制（改口径去源头改）。
export { S, X } from './store/state'   // 全局响应式状态 + 非响应式转态容器
export { api, fetchConfig, fetchFsList, fetchFile, fetchMcpStatus, fetchAgentLog,
         ctxSizeOf, fmtCtx, parseCtx, fetchProviderModels,
         fetchCtxBreakdown, fmtK, DEFAULT_CONTEXT, isImageAtt, attUrl, previewUrl } from './store/api'   // HTTP 契约原语与配置映射
export { liveStart, pushLocal } from './store/records'   // 记录状态机的发送侧入口
export { openSession, newSession, delSession, renameSession, loadSessions } from './store/sessions'   // 会话域：切换/新建/删除/重命名/注册表装载
export { send, decide, setModel, setMode, saveConfig, pickFolder,
         loadPluginUi, savePluginVals, saveMcpVals } from './store/actions'   // 用户动作：发送/裁决/热切/保存
export { boot } from './store/boot'   // 启动编排（health→配置→SSE→重放→会话）
