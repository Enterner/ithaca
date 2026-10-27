// 回归编排：确定性验收（mock 剧本驱动全链路，离线可复现）。
//   · 连续两次 selftest 输出完全一致（timing 归一化后）
//   · 输出与 golden 文件一致（--update-golden 重录）
//   · session 持久化断言（turns 计数落盘）
// 用法：node scripts/regress.mjs [--preset debug] [--update-golden]
import { spawnSync } from "node:child_process";
import fs from "node:fs";
import path from "node:path";
import { MOCK_CHAIN } from "./plugins.mjs";
import { ROOT, presetArg, hostExe, pluginDlls, freshWs, mockEnv } from "./common.mjs";

const preset = presetArg();
const updateGolden = process.argv.includes("--update-golden");
const exe = hostExe(preset);
const plugins = pluginDlls(MOCK_CHAIN, preset);

// 隔离工作区 + 会话文件 + 加速回放
const ws = freshWs(path.join(ROOT, "tests", "regress", ".ws"));
const env = {
    ...mockEnv(ws),   // 回归 = 测试模式（mock 缺省休眠后必须显式开启）
    ITHACA_SESSION_FILE: path.join(ws, "session.json"),
};

function invokeSelftest() {
    // 两次运行必须从相同起点出发：清整个工作区（文件残留会让 apply 的
    // is_new/diff 变化——踩过：第二轮 replace 变成 is_new:false）
    fs.rmSync(ws, { recursive: true, force: true });
    fs.mkdirSync(ws, { recursive: true });
    // 输出重定向到文件再读：①stdout+stderr 按真实写入顺序合并（两条管道分别收集
    // 顺序不定，会污染"两遍一致"比较）；②宿主管道输出是 UTF-8 字节，按 utf8 读即可
    const outFile = path.join(ws, "selftest_out.txt");
    const cmd = `"${exe}" --selftest ${plugins.map((p) => `"${p}"`).join(" ")} > "${outFile}" 2>&1`;
    const r = spawnSync(cmd, { shell: true, env, cwd: ws });
    if (r.status !== 0) {
        console.error(`✗ selftest 退出码 ${r.status}`);
        process.exit(r.status ?? 1);
    }
    return fs.readFileSync(outFile, "utf8")
        .replaceAll("\r\n", "\n")
        .replaceAll(/全程 \d+ms/g, "全程 <T>ms")
        // 取消场景（turn 9）的截断行数 = 取消传播与 5ms 流节奏的赛跑结果，是
        // 时序噪音——锁进 golden 只会偶发假失败。断言价值在 m4_ok（流即刻终止
        // + 通告到达），不在截断在第几行；整段归一移出比对（delta 文本以 \n 结尾
        // ⇒ 每条后随一个空行，一并移除）。
        .replaceAll(/^    delta#9: 输出行 \d+\n\n/gm, "")
        .replaceAll(ROOT, "<ROOT>")                        // 绝对路径归一化（反斜杠形态）——
        .replaceAll(ROOT.replaceAll("\\", "/"), "<ROOT>"); // 正斜杠形态。golden 才能跨机器比对
}

console.log("== 回归：两次连续 selftest ==");
const r1 = invokeSelftest();
const r2 = invokeSelftest();
if (r1 !== r2) {
    console.error("✗ 连续两次运行不一致（存在非确定性输出）");
    process.exit(1);
}
console.log("✓ 连续两次运行一致（timing 归一化后）");

const golden = path.join(ROOT, "tests", "regress", "golden", "selftest_mock.txt");
if (updateGolden || !fs.existsSync(golden)) {
    fs.mkdirSync(path.dirname(golden), { recursive: true });
    fs.writeFileSync(golden, r1);
    console.log(`✓ golden 已录制（${golden}）`);
} else {
    const expected = fs.readFileSync(golden, "utf8").replaceAll("\r\n", "\n").trim();
    if (r1.trim() !== expected) {
        console.error("✗ 与 golden 不一致——行为回归！");
        console.error(r1.slice(0, 600));
        process.exit(1);
    }
    console.log("✓ 与 golden 一致");
}

console.log("== session 持久化断言 ==");
const session = JSON.parse(fs.readFileSync(path.join(ws, "session.json"), "utf8"));
if (session.turns !== 3) fail(`✗ turns=${session.turns} ≠ 3`);
if (!(session.usage.total_tokens > 0)) fail("✗ usage 未落盘");
if (!(session.messages.length >= 10)) fail("✗ messages 过少");
console.log(`✓ 会话落盘：turns=${session.turns} messages=${session.messages.length} total_tokens=${session.usage.total_tokens}`);

console.log("\n回归全绿");

function fail(msg) {
    console.error(msg);
    process.exit(1);
}
