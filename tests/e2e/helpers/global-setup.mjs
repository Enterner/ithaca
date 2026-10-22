// e2e 跑前清扫（playwright globalSetup）：tests/.tmp 的保留上界 = 最近一轮运行。
// 钩在「跑」上而非各产出方收尾——teardown 只覆盖 fixture 用例，手工调试脚本、
// 单测残留、上一轮失败现场都接不到清理；跑前全清让任何产出方的残留都活不过
// 下一轮 e2e，失败现场语义 = 留到下次运行为止。
// NOTE: 假设单实例运行（check 链串行）——并发两轮 e2e 会互清对方在途目录。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..", "..", "..");

// eval/ 白名单：压缩评测报告要跨轮比对，不随 e2e 现场清掉
const KEEP = new Set(["eval"]);

export default function globalSetup() {
    const tmp = path.join(root, "tests", ".tmp");
    if (!fs.existsSync(tmp)) return;
    for (const entry of fs.readdirSync(tmp, { withFileTypes: true })) {
        if (KEEP.has(entry.name)) continue;
        fs.rmSync(path.join(tmp, entry.name), { recursive: true, force: true });
    }
}
