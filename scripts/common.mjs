// scripts 公共件：任务入口薄壳共享的样板——root 定位、命名参数解析、宿主产物
// 校验、插件 dll 路径、mock 隔离工作区与环境组装。各入口只留自己的编排逻辑。
import fs from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

export const ROOT = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

/** --name value 形态的命名参数（缺省 undefined）。 */
export function flag(name) {
    const i = process.argv.indexOf(name);
    return i >= 0 ? process.argv[i + 1] : undefined;
}

/** --preset 的统一解析（缺省 debug）。 */
export function presetArg() {
    return flag("--preset") ?? "debug";
}

/** 宿主 exe 路径；不存在即报错退出——所有入口的前置，spawn 失败再报就晚了。 */
export function hostExe(preset) {
    const exe = path.join(ROOT, "build", preset, "bin", "ithaca_host.exe");
    if (!fs.existsSync(exe)) {
        console.error(`✗ 找不到宿主 ${exe} —— 先构建：npm run build`);
        process.exit(1);
    }
    return exe;
}

/** 插件清单 → bin 下 dll 绝对路径；任一缺失即报错退出。统一严格口径——
 * 静默 filter/魔法数会把「新增插件没构建」吃掉（selftest 的 length<4 踩过）。 */
export function pluginDlls(names, preset) {
    const bin = path.join(ROOT, "build", preset, "bin");
    const dlls = names.map((n) => path.join(bin, `${n}_plugin.dll`));
    const missing = dlls.filter((p) => !fs.existsSync(p));
    if (missing.length) {
        console.error(`✗ 插件产物不全：${missing.map((p) => path.basename(p)).join(", ")}`);
        process.exit(1);
    }
    return dlls;
}

/** 隔离工作区：清掉重建（残留文件会让 apply 的 is_new/diff 漂移，踩过），返回路径。 */
export function freshWs(dir) {
    fs.rmSync(dir, { recursive: true, force: true });
    fs.mkdirSync(dir, { recursive: true });
    return dir;
}

/** mock 链路公共 env（加速回放节奏；workspace/session 等由调用方在其上补）。 */
export function mockEnv(ws, delayMs = 5) {
    return {
        ...process.env,
        ITHACA_WORKSPACE_DIR: ws,
        ITHACA_LLM_ACTIVE: "mock",   // mock 缺省休眠（非测试模式不发布），必须显式开启
        ITHACA_MOCK_DELAY_MS: String(delayMs),
    };
}
