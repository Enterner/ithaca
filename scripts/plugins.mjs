// 插件 dll 清单唯一出处：新增/改名插件只改本文件。C++ 侧两处无法 import
// （shell/ithaca.cpp、启动宿主.cmd——各有指针注释），改清单时需同步。
// 子集按语义划分：llm 两插件互斥装载（产品决策），各宿主形态只带其一。
export const ALL = ["llm_mock", "llm_cloud", "workspace", "runner", "agent", "mcp", "webui"];

// mock 最小闭环：无 HTTP 面（selftest / regress 的 --selftest 链）
export const MOCK_CHAIN = ["llm_mock", "workspace", "runner", "agent"];

// mock 全链：+ HTTP/SSE + MCP（dev --provider mock、e2e host fixture）
export const MOCK_FULL = ["llm_mock", "workspace", "runner", "agent", "webui", "mcp"];

// cloud 全链（dev 默认、便携包启动宿主）
export const CLOUD_FULL = ["llm_cloud", "workspace", "runner", "agent", "mcp", "webui"];

// 评测链：纯 workspace 闭环不跑命令（无 runner），无 MCP
export const MOCK_EVAL = ["llm_mock", "workspace", "agent", "webui"];
