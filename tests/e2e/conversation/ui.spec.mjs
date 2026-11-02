// 会话区域交互 · 与 agent 对话 UI 用例（对话闭环 / 决策坞 / 停止生成 / 命令）。
// mock 剧本（fixture 默认）每轮重放
// workspace.apply 写 hello.txt → assistant 收口。
import fs from "node:fs";
import path from "node:path";
import { test, expect, postJson } from "../helpers/host.mjs";

test.use({ hostCfg: { pickStub: true } });
const hello = (host) => path.join(host.ws, "picked-ws", "hello.txt");

// 「打开草稿对话流并发送」共用五步（subagent-ui 同款 helper——spec 间禁止互引，
// 故各留一份）：goto → SSE 就绪 → hero 选工作区（pickStub 钩子）→ 草稿流可见 →
// 填入发送。
async function openFlow(page, host, message) {
    await page.goto(host.base);
    await expect(page.locator("#conn", { hasText: "已连接" })).toBeVisible();   // 等 SSE 就绪
    await page.locator(".hero-ws").click();   // 选工作区（pickStub 测试钩子）
    await expect(page.locator(".stream")).toBeVisible();   // 等 folder_picked 记入草稿
    await page.locator("textarea.in").fill(message);
    await page.locator(".send-btn").click();
}


test("对话闭环：发送 → 落盘 → 本轮修改卡 → 收口 → 再对话", async ({ host, page }) => {
    await openFlow(page, host, "创建 hello.txt");

    // 用户消息渲染
    await expect(page.locator(".msg.u", { hasText: "创建 hello.txt" })).toBeVisible();
    // 落盘 = 「本轮修改」汇总卡（applied 并入卡内、不单独成行）+ 文件行 + 新文件徽章
    const edits = page.locator(".card.edits");
    await expect(edits).toBeVisible({ timeout: 15000 });
    await expect(edits.locator(".erow .p", { hasText: "hello.txt" })).toBeVisible();
    await expect(edits.locator(".ntag", { hasText: "新文件" }).first()).toBeVisible();
    // assistant 收口 + 工作区真实产物（跨层断言）
    await expect(page.locator(".msg.a", { hasText: "任务完成" }).first())
        .toBeVisible({ timeout: 15000 });
    expect(fs.existsSync(hello(host))).toBe(true);

    // 再对话：新 turn 走通（剧本耗尽的透传文本也照常渲染），对话连续性保持
    await page.locator("textarea.in").fill("继续");
    await page.locator(".send-btn").click();
    await expect(page.locator(".msg.u", { hasText: "继续" })).toBeVisible();
    await expect(page.locator(".msg.a", { hasText: "MOCK_EXHAUSTED" }).first())
        .toBeVisible({ timeout: 15000 });
    expect(fs.existsSync(hello(host))).toBe(true);
});

test("决策坞：suggest 模式提案 → 批准 → 落盘 → 坞收起", async ({ host, page }) => {
    await postJson(host.base, "/api/permission-mode", { mode: "suggest" });   // 前置：切逐条批准
    await openFlow(page, host, "创建 hello.txt");

    // 提案挂进决策坞（工具名 + 待你裁决）
    const dock = page.locator(".ddock");
    await expect(dock).toBeVisible({ timeout: 15000 });
    await expect(dock.locator("b", { hasText: "workspace.apply" })).toBeVisible();

    // 批准（决策坞 Enter 键同款按钮）→ 落盘（本轮修改汇总卡收口，applied 并
    // 入卡内、不单独成行）+ 坞收起 + 磁盘产物
    await dock.locator("button.y").click();
    await expect(page.locator(".card.edits", { hasText: "本轮修改" })).toBeVisible({ timeout: 15000 });
    await expect(page.locator(".ddock")).toHaveCount(0);
    await expect(page.locator(".msg.a", { hasText: "任务完成" }).first())
        .toBeVisible({ timeout: 15000 });
    expect(fs.existsSync(hello(host))).toBe(true);
});

test.describe("停止生成", () => {
    // 长内容 + 放慢流式节流，留出点停止的时间窗
    test.use({
        hostCfg: {
            pickStub: true,
            scenario: [{ role: "assistant", content: "长\n".repeat(120) }],
        },
    });
    test("生成中发送键变停止方框，点击取消 turn", async ({ host, page }) => {
        await openFlow(page, host, "写一篇长文");

        // 等流式真的开始（agent 已进 Running，取消才不会空转），此时键已变停止方框
        await expect(page.locator(".msg.a").first()).toBeVisible({ timeout: 15000 });
        const stop = page.locator(".send-btn.stop");
        await expect(stop).toBeVisible();
        await stop.click();

        // 取消通告渲染（llm_error ⇒ error 记录 → 错误卡：当前会话中断 + ↩ 撤回按钮）
        // + 按钮回到发送态
        await expect(page.locator(".errcard", { hasText: "当前会话中断" })).toBeVisible({ timeout: 15000 });
        await expect(page.locator(".send-btn.stop")).toHaveCount(0);
        await expect(page.locator(".send-btn")).toBeVisible();
    });
});

test("命令：/help 本地应答、/cancel 空转无害", async ({ host, page }) => {
    await openFlow(page, host, "创建 hello.txt");
    await expect(page.locator(".msg.a", { hasText: "任务完成" }).first())
        .toBeVisible({ timeout: 15000 });

    // /help：本地伪记录应答（不走 agent）
    await page.locator("textarea.in").fill("/help");
    await page.locator(".send-btn").click();
    await expect(page.locator(".msg", { hasText: "/undo 撤销上一 turn" })).toBeVisible({ timeout: 15000 });

    // /cancel：无待取消 turn 时空转无害（不报错、对话流不受污染）
    await page.locator("textarea.in").fill("/cancel");
    await page.locator(".send-btn").click();
    await expect(page.locator(".errcard")).toHaveCount(0);
});

test("/undo 撤销上一轮：undone 行 + 汇总卡标已撤销 + 文件回退删除", async ({ host, page }) => {
    // 挂起：带工作区会话的 undo 不回退文件（独立产品问题），修复后本用例转正。
    // fixme 一层即够——fixme+skip 叠用会让两处跳过原因各自为政（改一处漏一处）
    test.fixme(true, "undo 带工作区会话不回退文件（挂账产品问题）");
    await openFlow(page, host, "创建 hello.txt");
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

test.describe("工具行人性化", () => {
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
        await openFlow(page, host, "看看目录");

        // 调用参数摘要（两行制：调用行只说跑了什么——depth 等请求参数细节只在
        // 展开区；缺省 path = 工作区根，直接显示真实目录）
        await expect(page.locator(".fblock", { hasText: "列目录 " + host.ws }))
            .toBeVisible({ timeout: 15000 });
        await expect(page.locator(".fblock", { hasText: "搜索「hello」" })).toBeVisible();
        await expect(page.locator(".fblock", { hasText: "读取 api.txt" })).toBeVisible();
        // read 结果（字节数随内容变化——断言稳定前缀而非具体数字）
        await expect(page.locator(".procres", { hasText: "api.txt · " })).toBeVisible();
        // 结果摘要（独立结果行 └）
        await expect(page.locator(".procres", { hasText: "列出 1 项" })).toBeVisible();
        await expect(page.locator(".procres", { hasText: "命中 1 处 · 扫描 1 个文件" })).toBeVisible();
        // 收口
        await expect(page.locator(".msg.a", { hasText: "看完了" }).first())
            .toBeVisible({ timeout: 15000 });
        // 裸 JSON 不应出现
        await expect(page.locator(".fblock", { hasText: '{"entries"' })).toHaveCount(0);
    });
});
