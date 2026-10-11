// 便携包打包（原 scripts/package.ps1）：release 产物 + 前端 dist 组装为解压即用的 zip。
// zip 由系统自带 bsdtar（C:\Windows\System32\tar.exe，-a 按后缀出 zip）生成，零外部依赖。
// 用法：node scripts/pack.mjs [--version 0.1.0] [--preset release] [--with-redist]
import { createHash } from "node:crypto";
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
function flag(name) {
    const i = process.argv.indexOf(name);
    return i >= 0 ? process.argv[i + 1] : undefined;
}
const preset = flag("--preset") ?? "release";
const withRedist = process.argv.includes("--with-redist");
const bin = path.join(root, "build", preset, "bin");

let version = flag("--version") ?? "";
if (!version) {
    // 从顶层 CMakeLists 读 project(ithaca VERSION x.y.z ...)
    const m = fs.readFileSync(path.join(root, "CMakeLists.txt"), "utf8")
        .match(/project\(ithaca VERSION ([0-9.]+)/);
    if (!m) { console.error("读不到版本号（顶层 CMakeLists project VERSION）"); process.exit(1); }
    version = m[1];
}
const pack = path.join(root, "pack");
const stage = path.join(pack, `Ithaca-${version}-x64`);
fs.rmSync(stage, { recursive: true, force: true });

// ---- 组装 ----
const need = ["ithaca.exe", "ithaca_host.exe", "WebView2Loader.dll",
              "llm_mock_plugin.dll", "llm_cloud_plugin.dll",
              "workspace_plugin.dll", "runner_plugin.dll", "agent_plugin.dll", "mcp_plugin.dll",
              "webui_plugin.dll"];
for (const d of ["plugins", "web", "data/sessions", "data/workspace"]) {
    fs.mkdirSync(path.join(stage, d), { recursive: true });
}
for (const f of need) {
    if (!fs.existsSync(path.join(bin, f))) {
        console.error(`✗ 缺产物 ${f} —— 先 npm run build -- --preset ${preset}`);
        process.exit(1);
    }
    fs.copyFileSync(path.join(bin, f), path.join(stage, f.endsWith("_plugin.dll") ? "plugins" : "", f));
}
fs.cpSync(path.join(root, "ui", "dist"), path.join(stage, "web", "dist"), { recursive: true });

// 启动宿主.cmd：受信入口（cmd 链）先拉起宿主，ithaca.exe 再附着
// （企业管控软件按父进程信任链拦截未知程序的子进程——cmd 签名父进程可放行）
fs.copyFileSync(path.join(root, "scripts", "启动宿主.cmd"), path.join(stage, "启动宿主.cmd"));

const readme = [
    "Ithaca —— 代码的归途", "",
    "运行（两步）：",
    "  1. 双击「启动宿主.cmd」（黑窗口一闪而过属正常）",
    "  2. 双击 ithaca.exe", "",
    "首次需系统装有 Microsoft Edge WebView2 Runtime（Windows 10/11 一般自带）；",
    "缺 VC++ 运行库请先运行 vc_redist.x64.exe。数据保存在 data\\ 目录。",
].join("\r\n");
fs.writeFileSync(path.join(stage, "README.txt"), readme);

if (withRedist) {
    const res = await fetch("https://aka.ms/vs/17/release/vc_redist.x64.exe", { redirect: "follow" });
    fs.writeFileSync(path.join(stage, "vc_redist.x64.exe"), Buffer.from(await res.arrayBuffer()));
    console.log("已附带 vc_redist.x64.exe");
}

// ---- 压缩 + 校验和 ----
const zip = path.join(pack, `Ithaca-${version}-x64.zip`);
fs.rmSync(zip, { force: true });
const r = spawnSync(process.env.WINDIR
        ? path.join(process.env.WINDIR, "System32", "tar.exe")
        : "C:\\Windows\\System32\\tar.exe",
    // bsdtar（系统自带）按 .zip 后缀出 zip；PATH 里的 tar 可能是 Git Bash 的 GNU tar（不能写 zip）。
    // 相对路径 + cwd：bsdtar 会把 "D:\..." 的冒号当远程主机语法（rsh 写法）。
    ["-a", "-c", "-f", path.basename(zip), "-C", path.basename(stage), "."],
    { stdio: "inherit", shell: false, cwd: pack });
if (r.status !== 0) { console.error(`✗ 打 zip 失败（tar 退出码 ${r.status}）`); process.exit(r.status ?? 1); }
const sha = createHash("sha256").update(fs.readFileSync(zip)).digest("hex").toUpperCase();
console.log(`✓ ${zip}`);
console.log(`  SHA256 ${sha}`);

// ---- 安装包（二期）：Inno Setup 编译 ----
if (process.argv.includes("--no-installer")) {
    console.log("（跳过安装包）");
} else if (await buildInstaller()) {
    console.log(`✓ ${path.join(pack, `Ithaca-${version}-x64-setup.exe`)}`);
}

// WebView2 引导器（约 2MB，aka.ms=微软 CDN）打进安装包：目标机缺运行时时静默装上。
// 返回 false = ISCC 缺失或编译失败（zip 已出，不算致命）。
async function buildInstaller() {
    try {
        const res = await fetch("https://aka.ms/MicrosoftEdgeWebView2Setup.exe", { redirect: "follow" });
        fs.writeFileSync(path.join(stage, "MicrosoftEdgeWebview2Setup.exe"), Buffer.from(await res.arrayBuffer()));
    } catch (e) {
        console.warn(`⚠ WebView2 引导器下载失败（${e.message}）——目标机缺 WebView2 时安装包将无法自动补装`);
    }
    const iscc = ["ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA"]
        .map((k) => (process.env[k] ? path.join(process.env[k], "Inno Setup 6", "ISCC.exe") : null))
        .find((p) => p && fs.existsSync(p));
    if (!iscc) {
        console.warn("⚠ 未找到 Inno Setup 6（ISCC.exe）——只出了便携 zip。安装：winget install JRSoftware.InnoSetup");
        return false;
    }
    const isccArgs = [`/DAppVersion=${version}`, `/DStageDir=${stage}`, `/DPackDir=${pack}`];
    if (withRedist) isccArgs.push("/DWithRedist");
    console.log(`== 编译安装包（${iscc}）==`);
    const r2 = spawnSync(iscc, [...isccArgs, path.join(root, "scripts", "installer.iss")],
        { stdio: "inherit", shell: false, cwd: root });
    if (r2.status !== 0) { console.error(`✗ ISCC 编译失败（退出码 ${r2.status}）`); process.exit(r2.status ?? 1); }
    return true;
}
