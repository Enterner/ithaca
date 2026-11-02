// include 纪律检查：跨插件相对 include 机械拦截，无注释白名单豁免。
// 规则（冲突情形按更严口径判违规；全文见 AGENTS.md「目录与 include 纪律」）：
//   1. plugins/<名>/ 内文件的相对 include（./ ../ 形态）落点跨出本插件目录 = 违规
//      ——跨插件取头只许 contract/ 与 shared/（经 include 路径的裸名，不经相对路径）；
//   2. host/、shell/、shared/、contract/ 禁止相对 include 进 plugins/**；
//   3. tests/ 白盒放行（对齐 LLVM：单测可 include 插件私有头）。
// 诚实边界：经 include 路径解析的裸名 include 不建模（需完整 include 路径仿真）；
// 该形态的滥用必须先改 CMakeLists（review 可见），检查器盯的是相对路径形态。
// 用法：node scripts/check_includes.mjs（npm run check 首步，构建前快速失败）。
import fs from "node:fs";
import path from "node:path";
import { execSync } from "node:child_process";
import { ROOT } from "./common.mjs";

// 只查 git 跟踪的 C++ 源——构建产物/临时目录天然出局。
const files = execSync("git ls-files -- *.cpp *.hpp *.h", { cwd: ROOT, encoding: "utf8" })
    .split("\n").filter(Boolean);
const rootFwd = ROOT.replaceAll("\\", "/");   // 与 path.resolve 产物的正斜杠归一对齐

const violations = [];
for (const rel of files) {
    const file = path.join(ROOT, rel);
    const norm = rel.replaceAll("\\", "/");
    const lines = fs.readFileSync(file, "utf8").split("\n");
    lines.forEach((line, i) => {
        const m = /^\s*#\s*include\s*"(\.\/[^"]*|\.\.\/[^"]*)"/.exec(line);
        if (!m) return;   // 只盯相对形态；裸名经 include 路径解析、本检查不建模（该形态滥用必须先改 CMakeLists，review 可见）
        const target = path.resolve(path.dirname(file), m[1]).replaceAll("\\", "/");
        const hit = (why) => violations.push(`${norm}:${i + 1}  #include "${m[1]}"  ${why}`);
        if (norm.startsWith("plugins/")) {
            const pluginDir = norm.slice(0, norm.indexOf("/", "plugins/".length));
            if (!target.startsWith(`${rootFwd}/${pluginDir}/`))
                hit("相对 include 跨出本插件目录（跨插件取头走 contract/ 或 shared/ 裸名）");
            return;
        }
        // 插件域之外：进 plugins/ 即违规（tests 豁免见上方规则 3）
        if (!norm.startsWith("tests/") && target.startsWith(`${rootFwd}/plugins/`))
            hit("非插件域文件不得相对 include plugins/**");
    });
}

if (violations.length) {
    console.error(`✗ include 纪律违规 ${violations.length} 条：`);
    for (const v of violations) console.error(`  ${v}`);
    process.exit(1);
}
console.log(`✓ include 纪律：${files.length} 个源文件，0 违规`);
