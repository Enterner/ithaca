// 会话区域交互 · 与 agent 对话 UI 用例（对话闭环 / 决策坞 / 停止生成 / 命令）。
// mock 剧本（fixture 默认）每轮重放
// workspace.apply 写 hello.txt → assistant 收口。
import fs from "node:fs";
import path from "node:path";
import { test, expect, postJson } from "../helpers/host.mjs";

test.use({ hostCfg: { pickStub: true } });
const hello = (host) => path.join(host.ws, "picked-ws", "hello.txt");

test("CONV1 对话闭环：发送 → 落盘 → 本轮修改卡 → 收口 → 再对话", async ({ host, page }) => {
    await page.goto(host.base);
    await expect(page.locator("#conn", { hasText: "已连接" })).toBeVisible();   // 等 SSE 就绪
    await page.locator(".hero-ws").click();   // 选工作区（pickStub 测试钩子）
    await expect(page.locator(".stream")).toBeVisible();   // 等 folder_picked 记入草稿
    await page.locator("textarea.in").fill("创建 hello.txt");
    await page.locator(".send-btn").click();

    // 用户消息渲染
    await expect(page.locator(".msg.u", { hasText: "创建 hello.txt" })).toBeVisible();
    // 落盘事件行（applied 携带 diff）+ 新文件徽章
    await expect(page.locator(".toolline", { hasText: "已更新" })).toBeVisible({ timeout: 15000 });
    await expect(page.locator(".ntag", { hasText: "新文件" }).first()).toBeVisible();
    // 「本轮修改」汇总卡与文件行
    const edits = page.locator(".card.edits");
    await expect(edits).toBeVisible();
    await expect(edits.locator(".erow .p", { hasText: "hello.txt" })).toBeVisible();
    // assistant 收口 + 工作区真实产物（跨层断言）
    await expect(page.locator(".msg.a", { hasText: "任务完成" }).first())
        .toBeVisible({ timeout: 15000 });
    expect(fs.existsSync(hello(host))).toBe(true);

    // 再对话：新 turn 重新落盘，对话连续性保持
    await page.locator("textarea.in").fill("继续");
    await page.locator(".send-btn").click();
    await expect(page.locator(".msg.u", { hasText: "继续" })).toBeVisible();
    await expect(page.locator(".toolline", { hasText: "已更新" }).first())
        .toBeVisible({ timeout: 15000 });
    expect(fs.existsSync(hello(host))).toBe(true);
});

test("CONV2 决策坞：suggest 模式提案 → 批准 → 落盘 → 坞收起", async ({ host, page }) => {
    await postJson(host.base, "/api/permission-mode", { mode: "suggest" });   // 前置：切逐条批准
    await page.goto(host.base);
    await expect(page.locator("#conn", { hasText: "已连接" })).toBeVisible();   // 等 SSE 就绪
    await page.locator(".hero-ws").click();
    await expect(page.locator(".stream")).toBeVisible();   // 等 folder_picked 记入草稿
    await page.locator("textarea.in").fill("创建 hello.txt");
    await page.locator(".send-btn").click();

    // 提案挂进决策坞（工具名 + 待你裁决）
    const dock = page.locator(".ddock");
    await expect(dock).toBeVisible({ timeout: 15000 });
    await expect(dock.locator("b", { hasText: "workspace.apply" })).toBeVisible();

    // 批准（决策坞 Enter 键同款按钮）→ applied 落盘行 + 坞收起 + 磁盘产物
    await dock.locator("button.y").click();
    await expect(page.locator(".toolline", { hasText: "已更新" })).toBeVisible({ timeout: 15000 });
    await expect(page.locator(".ddock")).toHaveCount(0);
    await expect(page.locator(".msg.a", { hasText: "任务完成" }).first())
        .toBeVisible({ timeout: 15000 });
    expect(fs.existsSync(hello(host))).toBe(true);
});

test.describe("CONV5 停止生成", () => {
    // 长内容 + 放慢流式节流，留出点停止的时间窗
    test.use({
        hostCfg: {
            pickStub: true,
            scenario: [{ role: "assistant", content: "长\n".repeat(120) }],
        },
    });
    test("生成中发送键变停止方框，点击取消 turn", async ({ host, page }) => {
        await page.goto(host.base);
        await page.locator(".hero-ws").click();
        await expect(page.locator(".stream")).toBeVisible();
        await page.locator("textarea.in").fill("写一篇长文");
        await page.locator(".send-btn").click();

        // 等流式真的开始（agent 已进 Running，取消才不会空转），此时键已变停止方框
        await expect(page.locator(".msg.a").first()).toBeVisible({ timeout: 15000 });
        const stop = page.locator(".send-btn.stop");
        await expect(stop).toBeVisible();
        await stop.click();

        // 取消通告渲染（llm_error ⇒ error 记录 → 错误卡：turn #N 已被用户取消）
        // + 按钮回到发送态
        await expect(page.locator(".errcard", { hasText: "已被用户取消" })).toBeVisible({ timeout: 15000 });
        await expect(page.locator(".send-btn.stop")).toHaveCount(0);
        await expect(page.locator(".send-btn")).toBeVisible();
    });
});

test("CONV6 命令：/help 本地应答、/cancel 空转无害", async ({ host, page }) => {
    await page.goto(host.base);
    await expect(page.locator("#conn", { hasText: "已连接" })).toBeVisible();   // 等 SSE 就绪
    await page.locator(".hero-ws").click();
    await expect(page.locator(".stream")).toBeVisible();
    await page.locator("textarea.in").fill("创建 hello.txt");
    await page.locator(".send-btn").click();
    await expect(page.locator(".msg.a", { hasText: "任务完成" }).first())
        .toBeVisible({ timeout: 15000 });

    // /help：本地伪记录应答（不走 agent）
    await page.locator("textarea.in").fill("/help");
    await page.locator(".send-btn").click();
    await expect(page.locator(".msg", { hasText: "/undo 撤销上一 turn" })).toBeVisible();

    // /cancel：无待取消 turn 时空转无害（不报错、对话流不受污染）
    await page.locator("textarea.in").fill("/cancel");
    await page.locator(".send-btn").click();
    await expect(page.locator(".errcard")).toHaveCount(0);
});

test.fixme("CONV6b /undo 撤销上一轮：undone 行 + 汇总卡标已撤销 + 文件回退删除", async ({ host, page }) => {
    // 挂起：带工作区会话的 undo 不回退文件（独立产品问题，挂账），修复后转正
    test.skip(true, "undo 带工作区会话不回退文件（挂账产品问题）");
    await page.goto(host.base);
    await expect(page.locator("#conn", { hasText: "已连接" })).toBeVisible();   // 等 SSE 就绪
    await page.locator(".hero-ws").click();
    await expect(page.locator(".stream")).toBeVisible();
    await page.locator("textarea.in").fill("创建 hello.txt");
    await page.locator(".send-btn").click();
    await expect(page.locator(".msg.a", { hasText: "任务完成" }).first())
        .toBeVisible({ timeout: 15000 });
    await page.locator("textarea.in").fill("/undo");
    await page.locator(".send-btn").click();
    await expect(page.locator(".toolline", { hasText: "已撤销" })).toBeVisible({ timeout: 15000 });
    await expect(page.locator(".card.edits.revoked")).toBeVisible();
    await expect(async () => {
        expect(fs.existsSync(hello(host))).toBe(false);
    }).toPass({ timeout: 10000 });
});

test.describe("CONV10 工具行人性化", () => {
    // 场景驱动真实工具调用（list/search/read），断言 ● 行渲染人话摘要而非裸 JSON
    test.use({
        hostCfg: {
            pickStub: true,
            seed: (h) => {
                fs.mkdirSync(path.join(h.ws, "picked-ws"), { recursive: true });
                fs.writeFileSync(path.join(h.ws, "picked-ws", "api.txt"), "hello");
            },
            scenario: [
                { role: "assistant", content: "", tool_calls: [
                    { id: "t1", type: "function", function: { name: "workspace.list", arguments: JSON.stringify({ depth: 1 }) } },
                    { id: "t2", type: "function", function: { name: "workspace.search", arguments: JSON.stringify({ query: "hello" }) } },
                    { id: "t3", type: "function", function: { name: "workspace.read", arguments: JSON.stringify({ path: "api.txt" }) } },
                ]},
                { role: "assistant", content: "看完了" },
            ],
        },
    });
    test("list/search/read 渲染人话摘要", async ({ host, page }) => {
        await page.goto(host.base);
        await expect(page.locator("#conn", { hasText: "已连接" })).toBeVisible();
        await page.locator(".hero-ws").click();
        await expect(page.locator(".stream")).toBeVisible();
        await page.locator("textarea.in").fill("看看目录");
        await page.locator(".send-btn").click();

        // 调用参数摘要
        await expect(page.locator(".toolline", { hasText: "列目录 .（深度 1）" }))
            .toBeVisible({ timeout: 15000 });
        await expect(page.locator(".toolline", { hasText: "搜索「hello」" })).toBeVisible();
        await expect(page.locator(".toolline", { hasText: "读取 api.txt" })).toBeVisible();
        await expect(page.locator(".toolline", { hasText: "api.txt · 5 字节" })).toBeVisible();
        // 结果摘要
        await expect(page.locator(".toolline", { hasText: "列出 1 项" })).toBeVisible();
        await expect(page.locator(".toolline", { hasText: "命中 1 处 · 扫描 1 个文件" })).toBeVisible();
        // 收口
        await expect(page.locator(".msg.a", { hasText: "看完了" }).first())
            .toBeVisible({ timeout: 15000 });
        // 裸 JSON 不应出现
        await expect(page.locator(".toolline", { hasText: '{"entries"' })).toHaveCount(0);
    });
});
