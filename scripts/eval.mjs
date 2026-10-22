// 上下文压缩（transcript 折叠）的评测基准：确定性 mock 场景 × 压缩开/关（A/B 两臂），
// 采集指标对比。定位：测压缩指标不测对错——不进 check 链，改折叠逻辑后手动跑。
//   指标：任务成功率 / turns / prompt 与 total token / cache_tokens（缓存命中）/
//         折叠次数（transcript notice 解析）/ 锚点保真（host log 解析）/
//         recall 总数与未命中（transcript tool 记录）/ 墙钟。
//   两臂：baseline = 巨型窗口（折叠永不触发）；treated = 小窗口（发送前预检与
//         边界折叠确定性触发，mock 剧本逐轮注入 usage 精确控压）。
//   口径：TTFT 与摘要质量为主观项，以成功率保持 + 锚点保真 +
//   token 开销作为代理指标。报告落 tests/.tmp/eval/report.json。
// 用法：node scripts/eval.mjs [--preset debug]
import { spawn, spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const preset = process.argv.includes("--preset")
    ? process.argv[process.argv.indexOf("--preset") + 1]
    : "debug";
const bin = path.join(root, "build", preset, "bin");
const hostExe = path.join(bin, "ithaca_host.exe");
const outDir = path.join(root, "tests", ".tmp", "eval");
fs.rmSync(outDir, { recursive: true, force: true });
fs.mkdirSync(outDir, { recursive: true });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ---- 场景（确定性；剧本逐 chat 弹出：tool_calls 轮与收口轮各占一条）-------------
const scenarios = [
    {
        name: "三文件构建",
        prompts: [
            "创建 a.txt（内容：alpha 配置）",
            "创建 b.txt（内容：beta 路由）",
            "读取 a.txt 检查内容",
            "创建 c.txt 汇总 alpha 与 beta",
        ],
        turns: [
            { content: "", tool_calls: [{ id: "c1", type: "function", function: { name: "workspace.apply", arguments: JSON.stringify({ ops: [{ op: "write", path: "a.txt", content: "alpha=v1\n" + "配置参数\n".repeat(900) }] }) } }] },
            { content: "a.txt 已创建", usage: { prompt_tokens: 900, completion_tokens: 20, total_tokens: 920 } },
            { content: "", tool_calls: [{ id: "c2", type: "function", function: { name: "workspace.apply", arguments: JSON.stringify({ ops: [{ op: "write", path: "b.txt", content: "beta=v1\n" }] }) } }] },
            { content: "b.txt 已创建", usage: { prompt_tokens: 1400, completion_tokens: 20, total_tokens: 1420 } },
            { content: "", tool_calls: [{ id: "c3", type: "function", function: { name: "workspace.read", arguments: JSON.stringify({ path: "a.txt" }) } }] },
            { content: "a.txt 内容确认（头部 alpha=v1）", usage: { prompt_tokens: 1900, completion_tokens: 20, total_tokens: 1920 } },
            { content: "", tool_calls: [{ id: "c4", type: "function", function: { name: "workspace.apply", arguments: JSON.stringify({ ops: [{ op: "write", path: "c.txt", content: "alpha=v1\nbeta=v1\n" }] }) } }] },
            { content: "c.txt 已创建，汇总完成", usage: { prompt_tokens: 2400, completion_tokens: 20, total_tokens: 2420 } },
        ],
        verify: [
            { file: "a.txt", contains: "alpha=v1" },
            { file: "b.txt", contains: "beta=v1" },
            { file: "c.txt", contains: "beta=v1" },
        ],
    },
    {
        name: "长会话滚动",
        prompts: Array.from({ length: 6 }, (_, i) => `创建 chunk${i + 1}.txt（内容：第 ${i + 1} 段数据）`),
        turns: Array.from({ length: 6 }, (_, i) => [
            { content: "", tool_calls: [{ id: `w${i}`, type: "function", function: { name: "workspace.apply", arguments: JSON.stringify({ ops: [{ op: "write", path: `chunk${i + 1}.txt`, content: `第 ${i + 1} 段数据：${"数据".repeat(40)}` }] }) } }] },
            { content: `chunk${i + 1}.txt 已创建`, usage: { prompt_tokens: 1200 + i * 600, completion_tokens: 20, total_tokens: 1220 + i * 600 } },
        ]).flat(),
        verify: Array.from({ length: 6 }, (_, i) => ({ file: `chunk${i + 1}.txt`, contains: `第 ${i + 1} 段数据` })),
    },
];

// 两臂：baseline 巨型窗口（折叠永不触发）；treated 小窗口（预检/边界折叠确定性触发）
const arms = [
    { name: "baseline", window: "100000000" },
    { name: "treated", window: "6000" },
];

async function runOnce(scenario, arm, tag) {
    const home = path.join(outDir, `${tag}_home`);
    const ws = path.join(outDir, `${tag}_ws`);
    fs.rmSync(home, { recursive: true, force: true });
    fs.rmSync(ws, { recursive: true, force: true });
    fs.mkdirSync(path.join(home, "sessions"), { recursive: true });
    fs.mkdirSync(ws, { recursive: true });
    const scenarioFile = path.join(home, "scenario.json");
    fs.writeFileSync(scenarioFile, JSON.stringify(scenario.turns));
    fs.writeFileSync(path.join(home, "config.json"), JSON.stringify({ provider: "mock" }));

    const port = 8797;
    const plugins = ["llm_mock", "workspace", "agent", "webui"]
        .map((n) => path.join(bin, `${n}_plugin.dll`));
    const logLines = [];
    let proc = null;
    const startHost = () => {
        proc = spawn(hostExe, ["--seconds", "900", ...plugins], {
            cwd: ws,
            env: {
                ...process.env,
                ITHACA_WEBUI_HOME: home,
                ITHACA_WEBUI_WEB: path.join(root, "ui", "dist"),
                ITHACA_WEBUI_PORT: String(port),
                ITHACA_WORKSPACE_DIR: ws,
                ITHACA_LLM_ACTIVE: "mock",
                ITHACA_MOCK_TURNS: scenarioFile,
                ITHACA_MOCK_DELAY_MS: "2",
                ITHACA_CONTEXT_WINDOW: arm.window,   // A/B 开关
                ITHACA_SESSION_FILE: path.join(home, "sessions", "eval.state.json"),   // 启用 raw 外置
                ITHACA_API_KEY: "",
            },
            stdio: ["ignore", "pipe", "pipe"],
        });
        proc.stdout.setEncoding("utf8");
        proc.stdout.on("data", (d) => logLines.push(...d.toString().split("\n")));
        proc.stderr.setEncoding("utf8");
        proc.stderr.on("data", (d) => logLines.push(...d.toString().split("\n")));
    };
    startHost();

    const base = `http://127.0.0.1:${port}`;
    const api = async (p2, body) => {
        for (let i = 0; i < 3; ++i) {
            try {
                const r = await fetch(base + p2, {
                    method: body ? "POST" : "GET",
                    headers: body ? { "Content-Type": "application/json; charset=utf-8" } : undefined,
                    body: body ? JSON.stringify(body) : undefined,
                    signal: AbortSignal.timeout(8000),
                });
                return await r.json();
            } catch (e) {
                if (i === 2) throw e;
                await sleep(300);
            }
        }
    };
    const expectPoll = async (fn, timeoutMs = 60000, what = "") => {
        const deadline = Date.now() + timeoutMs;
        for (;;) {
            if (Date.now() > deadline) throw new Error("轮询超时: " + what);
            if (await fn()) return;
            await sleep(200);
        }
    };
    const waitTurnDone = () =>
        expectPoll(async () => (await api("/api/session")).in_flight === false, 60000, "turn 收口");

    const result = {
        scenario: scenario.name, arm: arm.name, success: true, error: "",
        turns: 0, prompt_tokens: 0, total_tokens: 0, cache_tokens: 0,
        folds: 0, anchor_kept: 0, anchor_total: 0, recall_total: 0, recall_misses: 0,
        wall_ms: 0,
    };
    const t0 = Date.now();
    try {
        for (let i = 0; i < 80; ++i) {
            try { if ((await api("/api/health")).ok) break; } catch (e) {}
            await sleep(250);
        }
        await api("/api/session/new", { message: scenario.prompts[0] });
        await waitTurnDone();
        for (let i = 1; i < scenario.prompts.length; ++i) {
            await api("/api/input", { line: scenario.prompts[i] });
            await waitTurnDone();
        }
        // 任务成功率：落盘产物逐项核验
        for (const v of scenario.verify) {
            const f = path.join(ws, v.file);
            if (!fs.existsSync(f) || !fs.readFileSync(f, "utf8").includes(v.contains)) {
                result.success = false;
                result.error = `产物不符: ${v.file}`;
            }
        }
        // 指标采集：transcript 记录 + host log + 会话元数据
        const meta = await api("/api/session");
        result.turns = meta.turns;
        result.prompt_tokens = meta.fTokens;   // total_tokens 口径（assistant 记录累计）
        const t = await api("/api/transcript?after=0");
        for (const rec of t.records) {
            if (rec.type === "assistant" && rec.usage) {
                result.prompt_tokens = rec.usage.prompt_tokens || result.prompt_tokens;  // 末轮 prompt
                result.total_tokens += rec.usage.total_tokens || 0;
                result.cache_tokens += rec.usage.cache_tokens || 0;
            }
            if (rec.type === "notice" && /上下文已压缩/.test(rec.text || "")) ++result.folds;
            if (rec.type === "tool" && /recall/.test(rec.tool || "")) {
                ++result.recall_total;
                if (rec.ok === false) ++result.recall_misses;
            }
        }
        for (const line of logLines) {
            const m = line.match(/折叠锚点保真 (\d+)\/(\d+)/);
            if (m) { result.anchor_kept += +m[1]; result.anchor_total += +m[2]; }
        }
        result.wall_ms = Date.now() - t0;
    } catch (e) {
        result.success = false;
        result.error = String(e).slice(0, 200);
    }
    spawnSync("taskkill", ["/F", "/T", "/PID", String(proc.pid)], { stdio: "ignore" });
    await sleep(300);
    fs.writeFileSync(path.join(outDir, `${tag}_host.log`), logLines.join("\n"));
    return result;
}

// ---- 主循环：场景 × 两臂，串行执行 --------------------------------------------
const report = [];
let allSuccess = true;
for (const sc of scenarios) {
    for (const arm of arms) {
        const tag = `${sc.name}_${arm.name}`.replace(/\s/g, "");
        const r = await runOnce(sc, arm, tag);
        report.push(r);
        if (!r.success) allSuccess = false;
        console.log(
            `[${r.arm.padEnd(8)}] ${r.scenario}  成功=${r.success ? "✓" : "✗"} ` +
            `turns=${r.turns} tokens=${r.total_tokens} (prompt ${r.prompt_tokens}, cache ${r.cache_tokens}) ` +
            `折叠=${r.folds} 锚点保真=${r.anchor_kept}/${r.anchor_total} ` +
            `recall=${r.recall_total}(未命中 ${r.recall_misses}) ${r.error}`);
    }
}

// A/B 对比（同场景 treated vs baseline）
console.log("\n== A/B 对比（treated vs baseline） ==");
for (const sc of scenarios) {
    const b = report.find((r) => r.scenario === sc.name && r.arm === "baseline");
    const t = report.find((r) => r.scenario === sc.name && r.arm === "treated");
    if (!b || !t) continue;
    const pct = (x, y) => (y ? Math.round((x / y) * 100) + "%" : "n/a");
    console.log(
        `${sc.name}: 成功 ${b.success}/${t.success}  tokens ${b.total_tokens}→${t.total_tokens} ` +
        `(${pct(t.total_tokens, b.total_tokens)})  折叠 0→${t.folds}  ` +
        `锚点保真 ${t.anchor_total ? t.anchor_kept + "/" + t.anchor_total : "n/a"}`);
}

fs.writeFileSync(path.join(outDir, "report.json"),
                 JSON.stringify({ generated: new Date().toISOString(), report }, null, 2));
const fidelityLines = report.flatMap((r) => []);   // 占位：logLines 在 runOnce 内
console.log(`\n报告: ${path.join(outDir, "report.json")}  总体: ${allSuccess ? "全部成功" : "存在失败"}`);
process.exit(allSuccess ? 0 : 1);
