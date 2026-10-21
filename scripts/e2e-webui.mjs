// webui 插件的端到端验收。
// 覆盖：health / 配置往返（key 掩码）/ 输入→直接落盘→applied 携带 diff→撤销回退 /
//       transcript 持久化 / 重启重放一致 / 会话注册表 / 权限模式 API / SSE 长连接存活。
// 用法：node scripts/e2e-webui.mjs [--preset debug]
import { spawn, spawnSync } from "node:child_process";
import fs from "node:fs";
import net from "node:net";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const preset = process.argv.includes("--preset") ? process.argv[process.argv.indexOf("--preset") + 1] : "debug";
const bin = path.join(root, "build", preset, "bin");
const hostExe = path.join(bin, "ithaca_host.exe");
if (!fs.existsSync(hostExe)) {
    console.error("✗ 先构建");
    process.exit(1);
}

const failures = [];
function assert(cond, name) {
    if (cond) console.log(`✓ ${name}`);
    else { console.error(`✗ ${name}`); failures.push(name); }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function j(url, opts = {}) {
    const res = await fetch(url, { ...opts, signal: AbortSignal.timeout(8000) });
    if (!res.ok) throw new Error(`${url} → HTTP ${res.status}`);
    return res.json();
}
const postJson = (url, body) => j(url, {
    method: "POST",
    headers: { "Content-Type": "application/json; charset=utf-8" },
    body: JSON.stringify(body),
});

// ---- 环境隔离 -----------------------------------------------------------------
const tmp = path.join(root, "tests", ".tmp");
const home2 = path.join(tmp, "e2e_webui_home");
const ws = path.join(tmp, "e2e_webui_ws");
fs.rmSync(home2, { recursive: true, force: true });
fs.rmSync(ws, { recursive: true, force: true });
fs.mkdirSync(path.join(home2, "sessions"), { recursive: true });
fs.mkdirSync(ws, { recursive: true });

// mock 剧本：提案（apply 写文件）→ 收尾（覆盖提案/裁决/落盘/收口全链路）
const scenario = path.join(ws, "scenario.json");
const scenarioOps = [
    { role: "assistant", content: "", tool_calls: [{ id: "c1", type: "function",
        function: { name: "workspace.apply",
            arguments: JSON.stringify({ ops: [{ op: "write", path: "hello.txt", content: "你好世界" }] }) } }] },
    { role: "assistant", content: "已创建 hello.txt，任务完成。" },
];
fs.writeFileSync(scenario, JSON.stringify(scenarioOps));

const baseEnv = {
    ...process.env,
    ITHACA_WEBUI_HOME: home2,
    ITHACA_WEBUI_WEB: path.join(root, "ui", "dist"),
    ITHACA_WEBUI_PORT: "8794",
    ITHACA_WORKSPACE_DIR: ws,
    ITHACA_MOCK_TURNS: scenario,
    ITHACA_API_KEY: "",
};
const plugins = ["llm_mock", "workspace", "runner", "agent", "webui"]
    .map((n) => path.join(bin, `${n}_plugin.dll`));
const base = "http://127.0.0.1:8794";

let proc = null;
function startHost(mode) {
    // 供应商模式（产品拍板：mock 仅测试）：cloud 段测配置往返，mock 段测对话闭环
    const env = { ...baseEnv, ITHACA_LLM_ACTIVE: mode };
    // 宿主日志落盘（诊断必需——老壳 STARTF_USESTDHANDLES 的等价物；stdio 传 fd
    // 由 node 自动继承）
    const hostLog = path.join(home2, `host-${mode}.log`);
    const hostFd = fs.openSync(hostLog, "a");
    fs.writeFileSync(path.join(home2, `env-${mode}.txt`),
                     Object.entries(env).map(([k, v]) => k + "=" + v).join(String.fromCharCode(10)));
    proc = spawn(hostExe, ["--seconds", "120", ...plugins],
                 { cwd: ws, env, stdio: ["ignore", hostFd, hostFd] });
    fs.closeSync(hostFd);
}
function stopHost() {
    // NOTE: 树杀必须在 proc.kill() 之前——先杀 cmd 会让树断裂，宿主孤儿化存活
    // （占着端口与 vc.llm.v1 发布位 ⇒ 后续实例 publish 失败 ⇒ LLM 不可用，踩过）
    if (proc) spawnSync("taskkill", ["/F", "/T", "/PID", String(proc.pid)], { stdio: "ignore" });
    if (proc && proc.exitCode === null) proc.kill();
    proc = null;
}

async function transcript() {
    return j(`${base}/api/transcript?after=0`);
}

// ---- 0. 清场（e2e 独占端口与宿主；残留宿主会占 8794/抢 vc.llm.v1 发布位）------
spawnSync("taskkill", ["/F", "/IM", "ithaca_host.exe"], { stdio: "ignore" });
await sleep(800);

// ---- 1. 启动与基础 ------------------------------------------------------------
startHost("cloud");
await sleep(3000);
const health = await j(`${base}/api/health`);
assert(health.ok === true, "health 就绪");
const page = await (await fetch(`${base}/`, { signal: AbortSignal.timeout(8000) })).text();
assert(page.includes("ithaca 测试台") || page.includes("Ithaca"), "SPA 页面可达");

// ---- 2. 配置往返（两层供应商 api_key 只写不读——掩码验证）------------------
const cfg0 = await j(`${base}/api/config`);
assert(cfg0.providers.length >= 1 && cfg0.providers.every((p) => p.type === "cloud"),
       "供应商单侧下发（cloud 模式只显示 cloud 项——产品拍板）");
assert(cfg0.current_provider !== "", "current_provider 已设置（钳在模式内）");
const cloudName = cfg0.providers[0].name;
// 保存：云供应商带 key（api_key 只写不读）+ current_provider 指向云端
const provs = cfg0.providers.map((p) => (p.type === "cloud" ? { ...p, api_key: "sk-e2e-test-key" } : p));
await postJson(`${base}/api/config`, { providers: provs, current_provider: cloudName });
const cfg = await j(`${base}/api/config`);
assert(cfg.current_provider === cloudName, "current_provider 往返");
const cloud = cfg.providers.find((p) => p.name === cloudName);
assert(cloud.has_key === true, "key 已保存（per-provider has_key 掩码）");
assert(!JSON.stringify(cfg).includes("sk-e2e-test-key"), "key 不出现在 /api/config 响应中");
assert(!JSON.stringify(cfg).includes('"api_key"'), "api_key 字段完全不下发");
const cfgFile = fs.readFileSync(path.join(home2, "config.json"), "utf8");
assert(cfgFile.includes("sk-e2e-test-key"), "key 已持久化到 config.json");
// api_key 留空 = 按名保留旧值（carry-over，key 不出前端）
await postJson(`${base}/api/config`, { providers: cfg.providers, current_provider: cloudName });
const cfgFile2 = fs.readFileSync(path.join(home2, "config.json"), "utf8");
assert(cfgFile2.includes("sk-e2e-test-key"), "api_key 留空 = 按名保留旧值");
// 切回 mock 作为当前供应商（后续对话闭环走本地回放）+ 未知模型自动建档进当前供应商
await postJson(`${base}/api/model`, { model: "e2e-auto-model" });
const cfg2b = await j(`${base}/api/config`);
const curP = cfg2b.providers.find((p) => p.name === cfg2b.current_provider);
assert(curP.models.filter((m) => m.id === "e2e-auto-model").length === 1, "未知模型自动建档进当前供应商");

// mock 段：切测试模式宿主（同 home；mock 模式发布 mock 服务，对话闭环走本地回放）
stopHost();
startHost("mock");
await sleep(1500);
for (const deadline = Date.now() + 15000; Date.now() < deadline;) {
    try { if ((await fetch(`${base}/api/health`, { signal: AbortSignal.timeout(1000) })).ok) break; }
    catch {}
    await sleep(300);
}

// ---- 3. 对话闭环（edit-diff v1.1 默认流）：创建即初始化 → 直接落盘 → applied 携带 diff → 撤销回退
// 默认确认清单已无 workspace.apply：agent 直接执行，diff 随 applied 事件到达（修改即事实）
// 方案 B：/api/session/new 必须带首条消息（创建即初始化）；裸调用 400
{
    const bare = await fetch(`${base}/api/session/new`, {
        method: "POST",
        headers: { "Content-Type": "application/json; charset=utf-8" },
        body: "{}",
    });
    assert(bare.status === 400, "裸 session/new 400（服务端不可能产生未初始化会话）");
}
const created = await postJson(`${base}/api/session/new`, { message: "创建 hello.txt" });
assert(created.ok === true && !!created.id, "组合端点创建即初始化（返回 id）");
const sid = created.id;
let t = null;
let applied = null;
for (const deadline = Date.now() + 15000; Date.now() < deadline;) {
    await sleep(400);
    t = await transcript();
    applied = t.records.find((r) => r.type === "applied");
    if (applied) break;
}
if (!applied) {   // 现场诊断（mock 段切换时序排查）
    console.error("转储：records =", JSON.stringify(t.records.map((r) => ({ id: r.id, type: r.type, msg: (r.msg || r.text || "").slice(0, 60) }))));
    const dbg = await j(`${base}/api/session`);
    console.error("session =", JSON.stringify({ id: dbg.id, ws: dbg.workspace, turns: dbg.turns, inflight: dbg.in_flight }));
    const cfgd = await j(`${base}/api/config`);
    console.error("config =", JSON.stringify({ provider: cfgd.provider, current: cfgd.current_provider, provs: (cfgd.providers || []).map((p) => p.name + ":" + p.type) }));
}
assert(applied != null, "落盘事件到达（默认免批流）");
assert(applied?.diff?.includes("── hello.txt"), "applied 携带 hunk diff（文件头）");
assert(applied?.diff?.includes("@@ "), "applied diff 含 hunk 头");
assert(applied?.diff?.includes("+ "), "applied diff 含新增行");
assert(t.records.filter((r) => r.type === "assistant" && /任务完成/.test(r.text ?? "")).length >= 1, "assistant 收口");
assert(fs.existsSync(path.join(ws, "hello.txt")), "工作区产物 hello.txt 存在");
const usageRec = t.records.find((r) => r.type === "assistant" && r.usage);
assert(usageRec != null && usageRec.usage.total_tokens > 0, "usage 记录（token 统计）");

// 撤销回退：decision 3 ⇒ undo_turn ⇒ undone 事件 + 文件删除（新建文件撤销 = 删除）
await postJson(`${base}/api/decision`, { decision: 3 });
let undone = false;
for (const deadline = Date.now() + 10000; Date.now() < deadline;) {
    await sleep(400);
    t = await transcript();
    if (t.records.filter((r) => r.type === "undone").length >= 1) { undone = true; break; }
}
assert(undone, "撤销事件到达（ithaca.edit.undone 有生产者）");
assert(!fs.existsSync(path.join(ws, "hello.txt")), "撤销后 hello.txt 已回退（删除）");

// ---- 3b. 权限模式 API：GET 回读 + POST 切换 + 配置持久化 ----------------
const pm = await j(`${base}/api/permission-mode`);
assert(pm.mode === "auto-edit", "权限模式默认 auto-edit");
await postJson(`${base}/api/permission-mode`, { mode: "suggest" });
const pm2 = await j(`${base}/api/permission-mode`);
assert(pm2.mode === "suggest", "权限模式切换 suggest 生效");
const cfgFile3 = fs.readFileSync(path.join(home2, "config.json"), "utf8");
assert(/"permission_mode"\s*:\s*"suggest"/.test(cfgFile3), "权限模式已持久化到 config.json");
await postJson(`${base}/api/permission-mode`, { mode: "auto-edit" });

// 等记录流稳定（undo 通告/权限模式通告经 poster 异步落盘——固定 sleep 会 race）
const tf = path.join(home2, "sessions", `${sid}.transcript.jsonl`);
let prev = -1;
let cnt = 0;
for (let i = 0; i < 12; i++) {
    await sleep(400);
    cnt = fs.readFileSync(tf, "utf8").split("\n").filter((l) => l.trim() !== "").length;
    if (cnt === prev) break;
    prev = cnt;
}
const before = cnt;

// ---- 4. 持久化与重启重放 --------------------------------------------------------
assert(fs.existsSync(tf), "transcript.jsonl 已落盘");
stopHost();
await sleep(800);
startHost("mock");
await sleep(3000);
const t2 = await transcript();
assert(t2.records.length === before, `重启重放一致（${t2.records.length} = ${before}）`);
const after = fs.readFileSync(tf, "utf8").split("\n").filter((l) => l.trim() !== "").length;
assert(after === before, `重启后文件未变（${after} = ${before}）`);

// ---- 5. 会话注册表（工作区分组数据源）+ 工作区随会话----------------------
// 方案 B：工作区归属 = 组合端点显式带 workspace（无独立 assign 端点）
const wsSession = await postJson(`${base}/api/session/new`, {
    message: "第二个会话",
    workspace: ws.replaceAll("\\", "/"),
});
const sessions = await j(`${base}/api/sessions`);
assert(sessions.length >= 2, "会话注册表有条目");
const active = sessions.find((s) => s.id === wsSession.id);
assert(active != null && active.turns >= 1, "活跃会话 turns 计数（创建即初始化）");
assert(`${active.workspace}`.includes("e2e_webui_ws"), "会话归属工作区记录（组合端点）");

// ---- 6. SSE 长连接存活（内核 kit::http reap 豁免的回归护栏；F14）----------------
// 原始套接字连 /events，静默 >15s 后广播仍应可达（回归护栏：连接不得被回收）
const chunks = [];
const client = net.connect(8794, "127.0.0.1");
client.on("data", (d) => chunks.push(d));
await new Promise((r) => client.on("connect", r));
client.write("GET /events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
await sleep(800);
const head = Buffer.concat(chunks).toString("utf8");
assert(head.includes("text/event-stream"), "SSE 响应头正确");
chunks.length = 0;
await sleep(17000);   // 静默跨过 15s 回收窗口
await postJson(`${base}/api/input`, { line: "SSE 存活探测" });
await sleep(1500);
const live = Buffer.concat(chunks).toString("utf8");
assert(live.includes("data:"), "静默超时后 SSE 仍推送（连接未被回收）");
client.destroy();

stopHost();
if (failures.length > 0) {
    console.error(`\ne2e 失败 ${failures.length} 项`);
    process.exit(1);
}
console.log("\nwebui e2e 全绿");
