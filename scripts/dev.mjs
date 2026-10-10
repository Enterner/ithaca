// Web UI 启动器（原 ithacaweb.ps1 + ithacaweb.bat）：
// 清理残留宿主 → 数据目录准备（含旧位置一次性迁移）→ 配置引导 → 设 env → 宿主前台运行 → 开浏览器。
// 会话/配置落在 <仓库>/.ithaca-data/——宿主/浏览器开关均不影响数据（方案 §3）。
// 用法：npm run dev [-- --provider mock] [-- --data-home D:\path] [-- --no-browser] [-- --preset release]
//       默认 cloud；mock 仅供开发测试（显式指定）。
import { spawn, spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
function flag(name) {
    const i = process.argv.indexOf(name);
    return i >= 0 ? process.argv[i + 1] : undefined;
}
const has = (name) => process.argv.includes(name);

// 旧版启动器（隐藏窗口后台跑）会残留不可见宿主：抢 8790 端口、并发写同一 state 文件
spawnSync("taskkill", ["/F", "/IM", "ithaca_host.exe"], { stdio: "ignore" });

const preset = flag("--preset") ?? "debug";
const provider = flag("--provider") === "mock" ? "mock" : "cloud"; // 只由启动参数决定，config.json 的旧 provider 值不作依据
const dataHome = flag("--data-home") ?? path.join(root, ".ithaca-data"); // 便携式默认跟仓库走（仓库目录被安全软件放行）
const webDir = flag("--web-dir") ?? path.join(root, "ui", "dist");
const openBrowser = !has("--no-browser");

// 数据目录 + 一次性迁移：旧位置（%USERPROFILE%\.ithaca）的配置与会话搬到新家
try {
    fs.mkdirSync(path.join(dataHome, "sessions"), { recursive: true });
} catch {
    console.error(`⚠ 数据目录不可写：${dataHome} —— 用 --data-home 换位置`);
}
const legacyHome = path.join(process.env.USERPROFILE ?? "", ".ithaca");
try {
    if (fs.existsSync(path.join(legacyHome, "config.json")) && !fs.existsSync(path.join(dataHome, "config.json"))) {
        fs.copyFileSync(path.join(legacyHome, "config.json"), path.join(dataHome, "config.json"));
    }
    if (fs.existsSync(path.join(legacyHome, "sessions")) && !fs.existsSync(path.join(dataHome, "sessions"))) {
        fs.cpSync(path.join(legacyHome, "sessions"), path.join(dataHome, "sessions"), { recursive: true });
    }
    fs.mkdirSync(path.join(dataHome, "sessions"), { recursive: true });
} catch {}

// 配置引导：首跑生成默认值；之后一切以文件为准（UI 保存也写这里）
const cfgPath = path.join(dataHome, "config.json");
// 数据目录里的 json 多数是 PowerShell 时代写的，带 UTF-8 BOM——JSON.parse 前剥掉
const readJson = (p) => JSON.parse(fs.readFileSync(p, "utf8").replace(/^\uFEFF/, ""));
let cfg;
if (fs.existsSync(cfgPath)) {
    cfg = readJson(cfgPath);
} else {
    cfg = { api_key: "", provider: "cloud", model: "deepseek-flash", base_url: "",
            workspace: "", port: 8790, active_session: "s1" };
    fs.writeFileSync(cfgPath, JSON.stringify(cfg, null, 2));
    console.log(`已创建默认配置 ${cfgPath} —— 首次使用请在网页 ⚙ 设置里填写 DeepSeek Key`);
}

// 宿主级默认工作区：必须可写。空/不存在 ⇒ 数据目录下的 workspace（踩过：
// 从 system32 启动 ⇒ jail 回退到启动目录 ⇒ agent 写文件被 Windows 拒绝、整批回滚）
if (!cfg.workspace || !fs.existsSync(cfg.workspace)) {
    cfg.workspace = path.join(dataHome, "workspace");
    try { fs.mkdirSync(cfg.workspace, { recursive: true }); } catch {}
}
// 活跃会话以 index.json 的 active 为准（webui 是唯一写者）；缺失时回退 config → s1
let active = cfg.active_session;
try {
    const idx = readJson(path.join(dataHome, "index.json"));
    if (idx.active) active = idx.active;
} catch {}
if (!active) active = "s1";

const env = process.env;
env.ITHACA_API_KEY = `${cfg.api_key ?? ""}`;
env.ITHACA_MODEL = `${cfg.model ?? ""}`;
env.ITHACA_WORKSPACE_DIR = cfg.workspace;
env.ITHACA_WEBUI_HOME = dataHome;
env.ITHACA_WEBUI_SESSION = active;
env.ITHACA_WEBUI_WEB = webDir;
env.ITHACA_WEBUI_PORT = `${cfg.port ?? 8790}`;
env.ITHACA_SESSION_FILE = path.join(dataHome, "sessions", `${active}.state.json`);
env.ITHACA_LLM_ACTIVE = provider;
// 回写 config（自愈历史遗留的 mock 值，保持文件与实际一致）；Node 原生 JSON 无 Depth 问题
cfg.provider = provider;
fs.writeFileSync(cfgPath, JSON.stringify(cfg, null, 2));

// 两个 provider 同时装载：热切 = 撤旧发布 + 发新发布（ithaca.llm.set_active，webui 设置里即时切换）
const bin = path.join(root, "build", preset, "bin");
const hostExe = path.join(bin, "ithaca_host.exe");
if (!fs.existsSync(hostExe)) {
    console.error(`✗ 找不到宿主 ${hostExe} —— 先构建：npm run build`);
    process.exit(1);
}
// 供应商模式决定装载（产品拍板：mock 仅测试，两 llm 插件互斥——热切竞态随之消失）
const plugins = [
    provider === "mock" ? "llm_mock" : "llm_cloud",
    "workspace", "runner", "agent", "mcp", "webui",
]
    .map((n) => path.join(bin, `${n}_plugin.dll`))
    .filter((p) => fs.existsSync(p));

const port = env.ITHACA_WEBUI_PORT;
console.log(`Ithaca Web UI：http://127.0.0.1:${port}`);
console.log(`工作区：${cfg.workspace}  Provider：${provider}  模型：${cfg.model}`);
if (openBrowser) {
    spawn("cmd", ["/c", "start", "", `http://127.0.0.1:${port}`], { detached: true, stdio: "ignore" });
}
console.log("宿主前台运行中——Ctrl+C 停止。日志：");

// 前台运行：日志持续输出到本控制台，Ctrl+C 触发宿主的干净停用链（子进程同控制台组，自动收到事件）。
const child = spawn(hostExe, plugins, { stdio: "inherit" });
child.on("exit", (code) => {
    if (code) console.log(`宿主退出码 ${code}`);
});
