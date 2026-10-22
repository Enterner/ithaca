// selftest 冒烟：mock 全链路单次运行（回归见 npm run regress）。
// 用法：node scripts/selftest.mjs [--preset debug]
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import { MOCK_CHAIN } from "./plugins.mjs";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const preset = process.argv.includes("--preset") ? process.argv[process.argv.indexOf("--preset") + 1] : "debug";
const bin = path.join(root, "build", preset, "bin");
const hostExe = path.join(bin, "ithaca_host.exe");
if (!fs.existsSync(hostExe)) {
    console.error(`✗ 找不到 ${hostExe} —— 先构建：npm run build`);
    process.exit(1);
}

// 隔离工作区（selftest 会写文件，不能落在启动目录）
const ws = path.join(root, "tests", ".tmp", "selftest_ws");
fs.rmSync(ws, { recursive: true, force: true });
fs.mkdirSync(ws, { recursive: true });

const env = { ...process.env };
env.ITHACA_WORKSPACE_DIR = ws;
env.ITHACA_SESSION_FILE = path.join(ws, "session.json");
env.ITHACA_LLM_ACTIVE = "mock";   // mock 缺省休眠（非测试模式不发布），必须显式开启
env.ITHACA_MOCK_DELAY_MS = "5";

const plugins = MOCK_CHAIN
    .map((n) => path.join(bin, `${n}_plugin.dll`))
    .filter((p) => fs.existsSync(p));
if (plugins.length < 4) {
    console.error("✗ 插件产物不全");
    process.exit(1);
}

const r = spawnSync(hostExe, ["--selftest", ...plugins], { stdio: "inherit", env, cwd: ws });
fs.rmSync(ws, { recursive: true, force: true });
if (r.status !== 0) {
    console.error(`✗ selftest 退出码 ${r.status}`);
    process.exit(r.status ?? 1);
}
console.log("\n✓ selftest 通过");
