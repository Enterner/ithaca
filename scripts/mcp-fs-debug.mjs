// 真实开源 MCP server（@modelcontextprotocol/server-filesystem）的接入调试入口。
// TODO: 调试结束后删除。
import path from "node:path";
import fs from "node:fs";
import { spawn, spawnSync } from "node:child_process";

const bin = "D:/CODE/c++/nostos_code/build/debug/bin";
const root = "D:/CODE/c++/nostos_code";
const home = path.join(root, "tests", ".tmp", "mcp_fs_home");
const ws = "D:/CODE/c++/nostos_code_test";

// 以真实数据目录为底：保留 mcp.servers，LLM 切 mock + 注入驱动场景（确定性调试）
fs.rmSync(home, { recursive: true, force: true });
fs.mkdirSync(path.join(home, "sessions"), { recursive: true });
const cfg = JSON.parse(fs.readFileSync(path.join(root, ".ithaca-data", "config.json"), "utf8"));
cfg.provider = "mock";
cfg.current_provider = "Mock 本地回放";
cfg.mcp = { servers: [
    { name: "fs", command: "node",
      args: ["C:/Users/Enter/AppData/Local/npm-cache/_npx/a3241bba59c344f5/node_modules/@modelcontextprotocol/server-filesystem/dist/index.js", ws],
      enabled: true },
] };
fs.writeFileSync(path.join(home, "config.json"), JSON.stringify(cfg, null, 2));
fs.writeFileSync(path.join(ws, "scenario.json"), JSON.stringify([
    { role: "assistant", content: "", tool_calls: [
        { id: "f1", type: "function",
          function: { name: "mcp.fs__list_directory",
                      arguments: JSON.stringify({ path: "." }) } },
        { id: "f2", type: "function",
          function: { name: "mcp.fs__read_file",
                      arguments: JSON.stringify({ path: "hello.txt" }) } },
    ]},
    { role: "assistant", content: "目录与文件已读取。" },
]));

const plugins = ["llm_mock", "workspace", "runner", "agent", "webui", "mcp"]
    .map((n) => path.join(bin, `${n}_plugin.dll`));
let log = "";
const proc = spawn(path.join(bin, "ithaca_host.exe"),
    ["--seconds", "180", ...plugins], {
        cwd: ws,
        env: { ...process.env, ITHACA_WEBUI_HOME: home, ITHACA_WEBUI_WEB: path.join(root, "ui", "dist"),
               ITHACA_WEBUI_PORT: "8990", ITHACA_WORKSPACE_DIR: ws, ITHACA_API_KEY: "",
               ITHACA_MOCK_TURNS: path.join(ws, "scenario.json") },
    });
proc.stdout.on("data", (d) => { log += d; });
proc.stderr.on("data", (d) => { log += d; });
const j = async (p) => (await fetch("http://127.0.0.1:8990" + p)).json();

for (let i = 0; i < 40; i++) {
    try { if ((await j("/api/health")).ok) break; } catch {}
    await new Promise((r) => setTimeout(r, 250));
}
await new Promise((r) => setTimeout(r, 1000));   // 等 npx 冷启动 + 握手 + tools/list
console.log("=== mcp 相关宿主日志 ===");
console.log(log.split("\n").filter((l) => l.includes("mcp")).join("\n") || "(无)");

await fetch("http://127.0.0.1:8990/api/session/new", { method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ message: "列目录并读取 hello.txt", workspace: ws }) });
let t;
for (let i = 0; i < 60; i++) {
    t = await j("/api/transcript?after=0");
    if (t.records.some((x) => x.type === "assistant" && /已读取/.test(x.text || ""))) break;
    await new Promise((r) => setTimeout(r, 500));
}
console.log("=== transcript 工具记录 ===");
for (const r of t.records)
    if (r.type === "tool_call" || r.type === "tool")
        console.log(r.type, r.tool, "|", (r.brief || "").slice(0, 120).replace(/\n/g, " ⏎ "));
console.log("=== 宿主日志尾部 ===");
console.log(log);
proc.kill();
spawnSync("taskkill", ["/F", "/T", "/PID", String(proc.pid)], { stdio: "ignore" });
process.exit(0);
