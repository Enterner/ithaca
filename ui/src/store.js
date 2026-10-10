// 桶文件：store 领域拆分（src/store/ 下 state/api/records/sessions/actions/boot）
// 后的统一出口——组件的 `from '../store'` 导入路径保持不变。依赖方向：
// state ← api ← records/sessions ← actions/boot（无环；X 转态容器在 state）。
export { S, X } from './store/state'
export { api, curProv, curProvModels, ctxSizeOf, fmtCtx, parseCtx, cfgFromServer,
         shellMsg } from './store/api'
export { render, pollOnce, flushTurnEdits, pushLocal, liveStart } from './store/records'
export { loadSessions, openSession, newSession, delSession, renameSession,
         sessionMeta, startShellPoll, resetView, enterDraft, discardDraft } from './store/sessions'
export { send, decide, setModel, setMode, saveConfig, pickFolder,
         loadPluginUi, savePluginVals } from './store/actions'
export { boot } from './store/boot'
export { mdHtml } from './md'
