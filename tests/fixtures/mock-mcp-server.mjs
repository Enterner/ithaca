// 测试夹具：最小 MCP stdio server（JSON-RPC 2.0，换行分帧）。
// 工具：add（两数相加，结果为文本）；method 未知 → JSON-RPC error。
// 用途：mcp.api.spec.mjs 验证 mcp 插件的接入链路（离线、确定性）。
let buf = "";
process.stdin.on("data", (d) => {
    buf += d;
    let i;
    while ((i = buf.indexOf("\n")) >= 0) {
        const line = buf.slice(0, i);
        buf = buf.slice(i + 1);
        if (line.trim()) handle(line);
    }
});
function send(msg) {
    process.stdout.write(JSON.stringify(msg) + "\n");
}
function handle(line) {
    let msg;
    try { msg = JSON.parse(line); } catch { return; }   // 噪音行丢弃
    if (msg.id === undefined) return;                   // 通知：忽略
    if (msg.method === "initialize")
        return send({ jsonrpc: "2.0", id: msg.id, result: {
            protocolVersion: "2024-11-05",
            capabilities: { tools: {} },
            serverInfo: { name: "mock-mcp", version: "0.1.0" },
        } });
    if (msg.method === "tools/list")
        return send({ jsonrpc: "2.0", id: msg.id, result: { tools: [
            { name: "add", description: "两数相加（mock）",
              inputSchema: { type: "object",
                             properties: { a: { type: "number" }, b: { type: "number" } },
                             required: ["a", "b"] } },
        ] } });
    if (msg.method === "tools/call") {
        const { name, arguments: a } = msg.params || {};
        if (name === "add")
            return send({ jsonrpc: "2.0", id: msg.id, result: {
                content: [{ type: "text", text: String((a?.a || 0) + (a?.b || 0)) }] } });
        return send({ jsonrpc: "2.0", id: msg.id, result: {
            content: [{ type: "text", text: "boom" }], isError: true } });
    }
    send({ jsonrpc: "2.0", id: msg.id,
           error: { code: -32601, message: "method not found: " + msg.method } });
}
