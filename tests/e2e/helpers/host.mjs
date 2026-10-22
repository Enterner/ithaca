// tests/e2e/helpers/host.mjs —— 宿主启停 + 隔离环境（共用基建），
// 叠加 @playwright/test fixture：每个用例独立的宿主生命周期与隔离 home。
// 端口按 workerIndex 分配（worker 内串行、worker 间错开），不用全局 taskkill。
import { spawn, spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { test as base } from "@playwright/test";
import { MOCK_FULL } from "../../../scripts/plugins.mjs";

export const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..", "..");

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export function makeHost({ name, port, preset = "debug" }) {
    const bin = path.join(root, "build", preset, "bin");
    const hostExe = path.join(bin, "ithaca_host.exe");
    if (!fs.existsSync(hostExe)) throw new Error(`找不到 ${hostExe} —— 先构建：npm run build`);
    const tmp = path.join(root, "tests", ".tmp");
    const home = path.join(tmp, `${name}_home`);
    const ws = path.join(tmp, `${name}_ws`);
    let proc = null;
    let extraEnv = {};
    return {
        home, ws, base: `http://127.0.0.1:${port}`,
        fresh() {
            fs.rmSync(home, { recursive: true, force: true });
            fs.rmSync(ws, { recursive: true, force: true });
            fs.mkdirSync(path.join(home, "sessions"), { recursive: true });
            fs.mkdirSync(ws, { recursive: true });
        },
        setEnv(map) { extraEnv = map; },
        writeScenario(ops) {
            const f = path.join(ws, "scenario.json");
            fs.writeFileSync(f, JSON.stringify(ops));
            return f;
        },
        start() {
            const plugins = MOCK_FULL
                .map((n) => path.join(bin, `${n}_plugin.dll`));
            proc = spawn(hostExe, ["--seconds", "300", ...plugins], {
                cwd: ws,
                env: {
                    ...process.env,
                    ITHACA_WEBUI_HOME: home,
                    ITHACA_WEBUI_WEB: path.join(root, "ui", "dist"),
                    ITHACA_WEBUI_PORT: String(port),
                    ITHACA_WORKSPACE_DIR: ws,
                    ITHACA_LLM_ACTIVE: "mock",   // e2e = 测试模式：mock 发布 + 面板单侧
                    ITHACA_API_KEY: "",
                    ...extraEnv,
                },
                stdio: "ignore",
            });
        },
        stop() {
            // NOTE: 树杀先行（/T）：先 kill cmd 会断树 → 宿主孤儿化
            if (proc)
                spawnSync("taskkill", ["/F", "/T", "/PID", String(proc.pid)], { stdio: "ignore" });
            if (proc && proc.exitCode === null) {
                try { proc.kill(); } catch {}
            }
            proc = null;
        },
        async waitReady(timeoutMs = 15000) {
            const deadline = Date.now() + timeoutMs;
            while (Date.now() < deadline) {
                try {
                    const r = await fetch(`${this.base}/api/health`, { signal: AbortSignal.timeout(1500) });
                    if (r.ok) return;
                } catch {}
                await sleep(250);
            }
            throw new Error("宿主未就绪（health 超时）");
        },
    };
}

// 默认 mock 剧本：提案（apply 写文件）→ 收口
const defaultScenario = [
    { role: "assistant", content: "", tool_calls: [{ id: "c1", type: "function",
        function: { name: "workspace.apply",
            arguments: JSON.stringify({ ops: [{ op: "write", path: "hello.txt", content: "你好世界" }] }) } }] },
    { role: "assistant", content: "已创建 hello.txt，任务完成。" },
];

export const test = base.extend({
    // hostCfg: { scenario?, seed?(host), extraEnv? } —— 在宿主启动前定制环境
    hostCfg: [{}, { option: true }],
    host: async ({ hostCfg }, use, testInfo) => {
        const h = makeHost({
            name: `${testInfo.title.replace(/[^A-Za-z0-9]+/g, "_").slice(0, 32)}_${testInfo.workerIndex}_${testInfo.retry}`,
            port: 8950 + testInfo.workerIndex * 4,
        });
        h.fresh();
        // 默认种 Mock 供应商配置：全新 home 的配置迁移会把 Mock 置为 disabled、
        // current_provider 指向无 key 的云端 ⇒ turn 必然「LLM 不可用」。
        // 会话生命周期用例不测配置迁移，一律直用 mock 回放。
        // config 种子：mock 供应商 + 可选的 MCP server 清单（mcp 插件读取）
        const seedCfg = { provider: "mock" };
        if (hostCfg.mcpServers) seedCfg.mcp = { servers: hostCfg.mcpServers };
        if (!hostCfg.noSeedConfig)
            fs.writeFileSync(path.join(h.home, "config.json"), JSON.stringify(seedCfg));
        h.writeScenario(hostCfg.scenario ?? defaultScenario);
        if (hostCfg.seed) hostCfg.seed(h);
        // 环境注入：mock 剧本路径 + pick-folder 测试钩子（宿主侧跳过原生对话框，
        // 固定返回 ws 下的 picked-ws）
        const envMap = { ITHACA_MOCK_TURNS: path.join(h.ws, "scenario.json") };
        if (hostCfg.pickStub) envMap.ITHACA_TEST_PICK_PATH = path.join(h.ws, "picked-ws");
        Object.assign(envMap, hostCfg.extraEnv ?? {});
        h.setEnv(envMap);
        h.start();
        await h.waitReady();
        await use(h);
        h.stop();
        // 用例目录用完即清——仅成功/跳过时删：失败现场留给排查（重试以
        // <title>_<worker>_<retry> 另起新目录，残留不影响重跑）；一律不删会让
        // tests/.tmp 每轮 e2e 净增一批 home/ws，无限累积。
        if (testInfo.status === "passed" || testInfo.status === "skipped") {
            fs.rmSync(h.home, { recursive: true, force: true });
            fs.rmSync(h.ws, { recursive: true, force: true });
        }
    },
});
export const expect = base.expect;

// 常用小助手
export async function getJson(base, p) {
    const r = await fetch(`${base}${p}`, { signal: AbortSignal.timeout(8000) });
    return { status: r.status, body: await r.json() };
}
export async function postJson(base, p, body) {
    const r = await fetch(`${base}${p}`, {
        method: "POST",
        headers: { "Content-Type": "application/json; charset=utf-8" },
        body: JSON.stringify(body),
        signal: AbortSignal.timeout(8000),
    });
    return { status: r.status, body: await r.json() };
}
