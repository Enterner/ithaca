// 会话创建生命周期 · UI 点击层用例。
// 浏览器直达 webui 的 HTTP 页面（mock LLM 剧本驱动回复，pick-folder 走测试钩子），
// 断言 API 层给不了的东西：真实渲染、点击链、草稿期零创建请求（网络监听）。
import fs from "node:fs";
import path from "node:path";
import { spawn, spawnSync } from "node:child_process";
import { test, expect, getJson, postJson, root } from "../helpers/host.mjs";

test.use({ hostCfg: { pickStub: true } });

function transcriptIds(host) {
    const dir = path.join(host.home, "sessions");
    return fs.readdirSync(dir).filter((f) => f.endsWith(".transcript.jsonl"));
}

test("SC1 UI 全点击链：＋新建 → 空态引导 → 选工作区 → 发送 → 归组 → 重启重放", async ({ host, page }) => {
    await page.goto(host.base);

    // 首装无会话 ⇒ 前端自动进草稿态：hero 引导可见
    const heroWs = page.locator(".hero-ws");
    await expect(heroWs).toBeVisible();

    // 选工作区（pick-folder 测试钩子 ⇒ folder_picked 事件 ⇒ 草稿记下工作区）
    await heroWs.click();
    await expect(page.locator(".stream")).toBeVisible();   // 有工作区：对话流 + dock

    // 发送首条消息 = 创建会话
    await page.locator("textarea.in").fill("你好伊萨卡");
    await page.locator(".send-btn").click();

    // 侧栏条目出现（标题 = 首条消息，归入 picked-ws 分组）、active 高亮
    const entry = page.locator(".sess .t", { hasText: "你好伊萨卡" });
    await expect(entry).toBeVisible();
    await expect(page.locator(".sess.active")).toHaveCount(1);

    // 回复渲染（mock 剧本收口）
    await expect(page.locator(".msg.a", { hasText: "任务完成" }).first()).toBeVisible({ timeout: 30000 });

    // 注册表：1 条、turns=1、workspace 归组；transcript 落盘
    const sessions = (await getJson(host.base, "/api/sessions")).body;
    expect(sessions).toHaveLength(1);
    expect(sessions[0].turns).toBe(1);
    expect(sessions[0].workspace).toContain("picked-ws");
    expect(transcriptIds(host)).toHaveLength(1);

    // 重启宿主 + reload：会话仍在、记录完整重放
    host.stop();
    host.start();
    await host.waitReady();
    await page.reload();
    await expect(page.locator(".sess .t", { hasText: "你好伊萨卡" })).toBeVisible();
    await expect(page.locator(".msg.u", { hasText: "你好伊萨卡" }).first()).toBeVisible();
    await expect(page.locator(".msg.a", { hasText: "任务完成" }).first()).toBeVisible({ timeout: 30000 });
});

test("SC2a/b/c UI：草稿期零创建请求，切走/关窗即丢弃", async ({ host, page }) => {
    // 前置：经 API 造一个已初始化会话
    const wsPath = path.join(host.ws, "real-ws");
    const created = await postJson(host.base, "/api/session/new", { message: "原会话", workspace: wsPath });
    expect(created.body.ok).toBe(true);
    // 等创建 turn 收口（in_flight 原子真值）——不收口时点＋新建会被前端 inFlight
    // 门卫吞掉（newSession 直接 return），hero 永不出现
    await expect
        .poll(async () => (await getJson(host.base, "/api/session")).body.in_flight, { timeout: 20000 })
        .toBe(false);

    // 创建类请求监听（SC2 的正面断言：草稿期一个都不许发）
    const creating = [];
    page.on("request", (r) => {
        if (r.method() === "POST" && /\/api\/session\/(new|assign)/.test(new URL(r.url()).pathname))
            creating.push(new URL(r.url()).pathname);
    });

    await page.goto(host.base);
    const entry = page.locator(".sess .t", { hasText: "原会话" });
    await expect(entry).toBeVisible();

    // SC2a：＋新建 → 不选工作区 → hero 出现，零创建请求
    await page.locator("button.new-chat").click();
    await expect(page.locator(".hero-ws")).toBeVisible();
    expect(creating).toHaveLength(0);

    // SC2b：选了工作区未发言 → 仍零创建请求、目录未建
    await page.locator(".hero-ws").click();
    await expect(page.locator(".stream")).toBeVisible();
    expect(creating).toHaveLength(0);
    expect(fs.existsSync(path.join(host.ws, "picked-ws"))).toBe(false);   // 目录创建延迟到发送

    // 切走（点侧栏原会话）→ 草稿丢弃、回到原会话
    await entry.click();
    await expect(page.locator(".sess.active")).toHaveCount(1);
    expect(creating).toHaveLength(0);

    // SC2c：再进草稿 → 关窗（reload）→ 无残留
    await page.locator("button.new-chat").click();
    await expect(page.locator(".hero-ws")).toBeVisible();
    await page.reload();
    await expect(page.locator(".sess .t", { hasText: "原会话" })).toBeVisible();
    expect(creating).toHaveLength(0);

    // 磁盘零残留：注册表 1 条、transcript 只有原会话、草稿目录未创建
    expect((await getJson(host.base, "/api/sessions")).body).toHaveLength(1);
    expect(transcriptIds(host)).toHaveLength(1);
    expect(fs.existsSync(path.join(host.ws, "picked-ws"))).toBe(false);
});

test("SC2d UI：草稿被丢弃后原会话记录与 active 分毫未动", async ({ host, page }) => {
    const created = await postJson(host.base, "/api/session/new", { message: "原会话" });
    expect(created.body.ok).toBe(true);
    const sid = created.body.id;
    // 等 turn 真收口：多轮 mock 剧本的中间 llm_done 也落一条空 assistant
    //（turn_done=false）——以带 turn_done 标记的收口记录为准（踩过：只认
    // assistant/旗子会在中间轮误判，记录数差一条）
    await expect
        .poll(async () => {
            const t = (await getJson(host.base, "/api/transcript?after=0")).body;
            return t.records.some((x) => x.turn_done === true);
        }, { timeout: 20000 })
        .toBe(true);
    const before = (await getJson(host.base, "/api/transcript?after=0")).body.records.length;

    await page.goto(host.base);
    await page.locator("button.new-chat").click();
    await expect(page.locator(".hero-ws")).toBeVisible();
    await page.locator(".sess .t", { hasText: "原会话" }).click();   // 切走 = 丢弃草稿

    // 原会话记录数、active 指向分毫未动
    await expect(page.locator(".sess.active")).toHaveCount(1);
    const meta = (await getJson(host.base, "/api/session")).body;
    expect(meta.id).toBe(sid);
    const after = (await getJson(host.base, "/api/transcript?after=0")).body.records.length;
    expect(after).toBe(before);
});

// 多实例会话切换（供应商模式 · 壳聚合视图的最小现场）：双宿主共享 shell
// manifest，侧栏 = webui 聚合清单；alive 实例点击 = 整页导航到它的端口
//（真实切换），停止态实例点击 = 发激活意图（浏览器内以 postMessage 桩捕获）。
test("SW1 UI 会话切换：聚合侧栏点击存活实例 = 整页导航切换；停止态 = 激活意图", async ({ page }) => {
    test.setTimeout(90000);
    // 释放固定端口：上次运行的残留宿主会占住 8794/8795（webui 回退随机端口 ⇒
    // baseA 失配）。测试规范禁 taskkill /IM 全局杀——会误杀并行 worker 的
    // fixture 宿主（踩过：并发跑时对端用例的 turn 被腰斩）——按端口找
    // LISTENING 的 PID 再按 PID 树杀。
    const freePort = (port) => {
        const out = spawnSync("netstat", ["-ano", "-p", "TCP"], { encoding: "utf8" }).stdout ?? "";
        const pids = new Set();
        for (const line of out.split("\n"))
            if (line.includes(`:${port} `) && line.includes("LISTENING")) {
                const pid = line.trim().split(/\s+/).pop();
                if (pid && pid !== "0") pids.add(pid);
            }
        for (const pid of pids)
            spawnSync("taskkill", ["/F", "/T", "/PID", pid], { stdio: "ignore" });
    };
    freePort(8794);
    freePort(8795);
    await new Promise((r) => setTimeout(r, 800));

    // 现场双实例共享 manifest（聚合端点按 manifest.parent/hosts/<key> 解析实例 home）
    const dir = path.join(root, "tests", ".tmp", "switch");
    fs.rmSync(dir, { recursive: true, force: true });
    const hostsRoot = path.join(dir, "hosts");
    const wsA = path.join(dir, "wsA");
    const wsB = path.join(dir, "wsB");
    for (const d of [path.join(hostsRoot, "hA", "sessions"), path.join(hostsRoot, "hB", "sessions"), wsA, wsB])
        fs.mkdirSync(d, { recursive: true });
    const now = Math.floor(Date.now() / 1000);
    const idx = (sid, title, ws) => JSON.stringify({
        active: sid, sessions: [{ id: sid, title, workspace: ws, updated: now, turns: 1 }],
    });
    fs.writeFileSync(path.join(hostsRoot, "hA", "index.json"), idx("sSWA", "会话甲", wsA));
    fs.writeFileSync(path.join(hostsRoot, "hB", "index.json"), idx("sSWB", "会话乙", wsB));
    fs.writeFileSync(path.join(hostsRoot, "hA", "port"), "8794");
    fs.writeFileSync(path.join(hostsRoot, "hB", "port"), "8795");
    const manifest = path.join(dir, "shell-manifest.txt");
    fs.writeFileSync(manifest, ["hA\t" + wsA, "hB\t" + wsB].join("\r\n"));

    const bin = path.join(root, "build", "debug", "bin");
    const procs = [];
    const spawnHost = (home, port) => {
        const fd = fs.openSync(path.join(home, "host.log"), "a");   // 宿主日志落盘（排障必需）
        const pr = spawn(path.join(bin, "ithaca_host.exe"),
            ["--seconds", "180",
             ...["llm_mock", "workspace", "runner", "agent", "webui"].map((n) => path.join(bin, `${n}_plugin.dll`))],
            { cwd: wsA,
              env: { ...process.env,
                  ITHACA_WEBUI_HOME: home,
                  ITHACA_WEBUI_WEB: path.join(root, "ui", "dist"),
                  ITHACA_WEBUI_PORT: String(port),
                  ITHACA_WORKSPACE_DIR: wsA,
                  ITHACA_LLM_ACTIVE: "mock",
                  ITHACA_SHELL_MANIFEST: manifest,
                  ITHACA_API_KEY: "" },
              stdio: ["ignore", "pipe", "pipe"] });
        pr.stdout.on("data", (d) => process.stdout.write("[host " + port + "] " + d.toString()));
        pr.stderr.on("data", (d) => process.stdout.write("[host " + port + "!ERR] " + d.toString()));
        pr.on("exit", (c) => console.log("[host " + port + "] EXIT code=" + c));
        pr.on("exit", (c) => console.log(`[host ${port}] EXIT code=${c} at=${new Date().toISOString().slice(11, 19)}`));
        procs.push(pr);
        return pr;
    };
    spawnHost(path.join(hostsRoot, "hA"), 8794);
    spawnHost(path.join(hostsRoot, "hB"), 8795);
    const baseA = "http://127.0.0.1:8794";
    const baseB = "http://127.0.0.1:8795";
    const waitReady = async (base) => {
        for (const d = Date.now() + 20000; Date.now() < d;) {
            try { if ((await fetch(base + "/api/health", { signal: AbortSignal.timeout(1200) })).ok) return; } catch {}
            await new Promise((r) => setTimeout(r, 250));
        }
        throw new Error("宿主未就绪：" + base);
    };
    try {
        await waitReady(baseA);
        await waitReady(baseB);

        // 捕获激活意图（停止态路径在无壳浏览器里只能验证意图）。桩面 = boot.js
        // 依赖的最小 webview 外观：postMessage（激活意图出向）+ addEventListener/
        // removeEventListener（boot 注册 navigate 监听用）——缺任一 boot 即抛
        // TypeError，SSE 永不连接（踩过：只桩 postMessage ⇒ 页面停在「连接中」）
        await page.addInitScript(() => {
            window.__shellMsgs = [];
            window.chrome = window.chrome || {};
            window.chrome.webview = {
                postMessage: (o) => window.__shellMsgs.push(o),
                addEventListener: () => {},
                removeEventListener: () => {},
            };
        });
        page.on("console", (m) => {
            if (m.type() === "error" || m.type() === "warning")
                console.log("[page console]", m.type(), m.text().slice(0, 120));
        });
        page.on("response", (r) => {
            if (r.status() >= 400) console.log("[PAGE404]", r.status(), r.url());
        });
        page.on("requestfailed", (r) => {
            console.log("[PAGE REQFAIL]", r.url().slice(0, 90), r.failure()?.errorText?.slice(0, 60));
        });
        page.on("request", (r) => {
            const u = r.url();
            if (u.includes("/events") || u.includes("favicon")) console.log("[PAGE REQ]", u);
        });
        await page.goto(baseA);
        // 等 boot 完成（已连接 = SSE 上线 = 事件通道就绪）
        await expect(page.locator("#conn", { hasText: "已连接" })).toBeVisible({ timeout: 20000 });
        await expect(page.locator(".sess .t", { hasText: "会话甲" })).toBeVisible();
        await expect(page.locator(".sess .t", { hasText: "会话乙" })).toBeVisible();

        // 真实切换（核心）：点击乙行（存活）→ 整页导航到 8795 → 乙成为活动会话
        await page.locator(".sess .t", { hasText: "会话乙" }).click();
        await page.waitForURL(/8795/, { timeout: 15000 });
        await expect(page.locator(".sess.active .t", { hasText: "会话乙" })).toBeVisible({ timeout: 15000 });

        // 切回甲
        await page.locator(".sess .t", { hasText: "会话甲" }).click();
        await page.waitForURL(/8794/, { timeout: 15000 });
        await expect(page.locator(".sess.active .t", { hasText: "会话甲" })).toBeVisible({ timeout: 15000 });

        // 停止态：树杀乙宿主（只杀乙——甲继续活着供聚合查询）→ 聚合探测判停止
        // → 点击乙 = 激活意图（壳内由壳拉起；此处以 postMessage 桩捕获）
        spawnSync("taskkill", ["/F", "/T", "/PID", String(procs[1].pid)], { stdio: "ignore" });
        await expect
            .poll(async () => {
                try {
                    const r = await fetch(baseA + "/api/shell/sessions", { signal: AbortSignal.timeout(1500) });
                    const b = await r.json();
                    return b.sessions?.find((x) => x.key === "hB")?.alive;
                } catch { return "probe-failed"; }   // 探活失败 ≠ 已停止
            }, { timeout: 15000 })
            .toBe(false);
        // 页面侧数据只能靠它自己的 3s 轮询刷新（store 不暴露给测试进程）——等事实：
        // 乙行渲染出「已停止」角标后，点击才会走激活意图而非直航死端口（踩过：
        // 轮询间隔内旧 alive=true 会被整页导航到已杀实例）
        await expect(
            page.locator(".sess", { has: page.locator(".t", { hasText: "会话乙" }) })
                .locator(".m", { hasText: "已停止" }),
        ).toBeVisible({ timeout: 15000 });
        await page.locator(".sess .t", { hasText: "会话乙" }).click();
        const msgs = await page.evaluate(() => window.__shellMsgs);
        expect(msgs.some((m) => m.type === "activate" && m.key === "hB")).toBe(true);
    } finally {
        for (const pr of procs)
            spawnSync("taskkill", ["/F", "/T", "/PID", String(pr.pid)], { stdio: "ignore" });
    }
});
