// selftest 冒烟：mock 全链路单次运行（回归见 npm run regress）。
// 用法：node scripts/selftest.mjs [--preset debug]
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { MOCK_CHAIN } from "./plugins.mjs";
import { ROOT, presetArg, hostExe, pluginDlls, freshWs, mockEnv } from "./common.mjs";

const preset = presetArg();
const exe = hostExe(preset);

// 隔离工作区（selftest 会写文件，不能落在启动目录）
const ws = freshWs(path.join(ROOT, "tests", ".tmp", "selftest_ws"));

const env = { ...mockEnv(ws), ITHACA_SESSION_FILE: path.join(ws, "session.json") };

const plugins = pluginDlls(MOCK_CHAIN, preset);

const r = spawnSync(exe, ["--selftest", ...plugins], { stdio: "inherit", env, cwd: ws });
fs.rmSync(ws, { recursive: true, force: true });
if (r.status !== 0) {
    console.error(`✗ selftest 退出码 ${r.status}`);
    process.exit(r.status ?? 1);
}
console.log("\n✓ selftest 通过");
