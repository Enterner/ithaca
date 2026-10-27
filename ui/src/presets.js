// 主流平台快速接入模板（设置面「模型接入」的数据源）：选平台 → 粘贴 API Key →
// 保存即用（Base URL 与模型表预置，模型清单可再增删改）。纯数据模块——视图只做
// 选择与展示，平台目录的维护点在此。
// 注意：火山 Coding Plan 必须用 coding 专用 base_url 才走套餐额度（实测）；
// 其余平台的旗舰模型 ID 随平台迭代可能过期，以官方模型列表为准。
export const PRESETS = [
  { key: 'ark-coding', name: '火山方舟 Coding Plan',
    base_url: 'https://ark.cn-beijing.volces.com/api/coding/v3',
    models: [
      { id: 'doubao-seed-2.1-pro', context: 256000 },
      { id: 'doubao-seed-2.1-lite', context: 256000 },
      { id: 'doubao-seed-2.0-mini', context: 256000 },
      { id: 'glm-5.3', context: 200000 },
      { id: 'glm-5.3-flash', context: 200000 },
      { id: 'deepseek-v4.1-flash', context: 128000 },
      { id: 'kimi-k3', context: 256000 },
    ] },
  { key: 'ark', name: '火山方舟 按量付费',
    base_url: 'https://ark.cn-beijing.volces.com/api/v3',
    models: [
      { id: 'doubao-seed-2-1-pro-260915', context: 256000 },
      { id: 'doubao-seed-2-1-lite-260915', context: 256000 },
      { id: 'deepseek-v4-1-flash-261017', context: 128000 },
      { id: 'glm-5-3-flash-260828', context: 200000 },
    ] },
  { key: 'deepseek', name: 'DeepSeek 官方', base_url: 'https://api.deepseek.com',
    models: [
      { id: 'deepseek-flash', context: 128000 },
      { id: 'deepseek-v4-pro', context: 128000 },
      { id: 'deepseek-reasoner', context: 128000 },
    ] },
  { key: 'kimi', name: 'Kimi（月之暗面）', base_url: 'https://api.moonshot.cn/v1',
    models: [
      { id: 'kimi-k3.1-pro', context: 256000 },
      { id: 'kimi-k2.5', context: 256000 },
    ] },
  { key: 'glm', name: '智谱 GLM', base_url: 'https://open.bigmodel.cn/api/paas/v4',
    models: [
      { id: 'glm-5.3', context: 200000 },
      { id: 'glm-5.2', context: 200000 },
    ] },
  { key: 'qwen', name: '阿里云百炼 Qwen',
    base_url: 'https://dashscope.aliyuncs.com/compatible-mode/v1',
    models: [
      { id: 'qwen3.8-max', context: 256000 },
      { id: 'qwen3.8-flash', context: 256000 },
      { id: 'qwen3.7-max', context: 256000 },
    ] },
  { key: 'openai', name: 'OpenAI', base_url: 'https://api.openai.com/v1',
    models: [
      { id: 'gpt-5.2', context: 400000 },
      { id: 'gpt-5.2-thinking', context: 400000 },
    ] },
]
