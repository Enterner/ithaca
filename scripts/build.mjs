// 构建 + 单元测试编排。
// 代理防御：MSBuild 遇到仅大小写不同的重名代理变量会抛 MSB6001，
// 构建前把成对的代理环境变量整组清掉（cmd 层防御的 Node 等价物）。
// 用法：node scripts/build.mjs [--preset debug|release|asan] [--test-only]
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const preset = flag("--preset") ?? "debug";
const testOnly = process.argv.includes("--test-only");

function flag(name) {
    const i = process.argv.indexOf(name);
    return i >= 0 ? process.argv[i + 1] : undefined;
}
function run(cmd, args) {
    const env = { ...process.env };
    for (const k of ["HTTP_PROXY", "HTTPS_PROXY", "ALL_PROXY", "NO_PROXY"]) {
        delete env[k];
        delete env[k.toLowerCase()];
    }
    const r = spawnSync(cmd, args, { stdio: "inherit", shell: false, env, cwd: root });
    if (r.status !== 0) {
        console.error(`✗ ${cmd} ${args.join(" ")} 退出码 ${r.status}`);
        process.exit(r.status ?? 1);
    }
}

if (!testOnly) {
    // 内核子模块没初始化（普通 git clone 而非 --recurse-submodules）就先补上
    if (!fs.existsSync(path.join(root, "external", "nostos", "CMakeLists.txt")) &&
        fs.existsSync(path.join(root, ".git"))) {
        run("git", ["submodule", "update", "--init"]);
    }
    run("cmake", ["--preset", preset]);
    run("cmake", ["--build", "--preset", preset]);
}

const tests = path.join(root, "build", preset, "bin", "ithaca_unit_tests.exe");
if (!fs.existsSync(tests)) {
    console.error(`✗ 找不到 ${tests} —— 先构建：npm run build`);
    process.exit(1);
}
run(tests, []);
console.log("\n全部完成。运行：npm run dev（浏览器）或桌面壳 build/<preset>/bin/ithaca.exe；回归 npm run regress");
