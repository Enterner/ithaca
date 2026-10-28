// 子代理全流程 e2e（mock 剧本驱动）：派发→子代理跑命令→报告→父收口的单卡
// 显示与终态翻转，以及取消场景的终态卡。mock 剧本按请求到达序消费（父/子共用
// 一个序列——子请求在 task 派发后到达，父回合在收口后继续）。
import fs from "node:fs";
import path from "node:path";
import { test, expect } from "../helpers/host.mjs";

test.use({ hostCfg: { pickStub: true } });

async function openFlow(page, host) {
    await page.goto(host.base);
    await expect(page.locator("#conn", { hasText: "已连接" })).toBeVisible();
    await page.locator(".hero-ws").click();
    await expect(page.locator(".stream")).toBeVisible();
    await page.locator("textarea.in").fill("派子代理跑命令");
    await page.locator(".send-btn").click();
}

test.describe("SUB1 完整周期：派发→执行→报告→收口", () => {
    test.use({
        hostCfg: {
            pickStub: true,
            scenario: [
                { role: "assistant", content: "", tool_calls: [
                    { id: "t1", type: "function", function: { name: "task", arguments: JSON.stringify({
                        description: "命令测试",
                        prompt: "运行 echo a && echo b 并报告结果",
                        subagent_type: "general",
                    }) } },
                ]},
                // 子代理第 1 轮：跑命令（异步 ack → 等 run_exit 归流）
                { role: "assistant", content: "", tool_calls: [
                    { id: "c1", type: "function", function: { name: "runner.run", arguments: JSON.stringify({ command_line: "echo a && echo b" }) } },
                ]},
                // 子代理第 2 轮：收口报告（无 tool_calls ⇒ 子代理完成回灌）
                { role: "assistant", content: "命令测试完成：a 与 b 均已输出" },
                // 父收口
                { role: "assistant", content: "子代理报告：命令测试完成（a、b 均正常输出）" },
            ],
        },
    });
    test("一次派发单卡显示；终态翻转；明细落独立存档", async ({ host, page }) => {
        await openFlow(page, host);

        // 一次派发只有一张卡：spawn 卡唯一，task 调用卡（派出子代理 ● 行）不出现
        await expect(page.locator(".subcard")).toHaveCount(1);
        await expect(page.locator(".fblock", { hasText: "派出子代理" })).toHaveCount(0);
        // 命令在真实 runner 里执行（cmd 包装），子代理收口 ⇒ 卡片终态翻转
        const stat = page.locator(".subcard .stat");
        await expect(stat).toHaveText(/已完成 · 点击查看/, { timeout: 20000 });
        // 父收口
        await expect(page.locator(".msg.a", { hasText: "子代理报告" }).first())
            .toBeVisible({ timeout: 15000 });
        // 跨层：明细已落独立存档（sessions/agents/<会话>/agent_*.jsonl）
        await expect(async () => {
            const agentsDir = path.join(host.home, "sessions", "agents");
            const sessions = fs.existsSync(agentsDir) ? fs.readdirSync(agentsDir) : [];
            expect(sessions.length).toBeGreaterThan(0);
            const files = fs.readdirSync(path.join(agentsDir, sessions[0]));
            expect(files.some((f) => f.startsWith("agent_"))).toBe(true);
        }).toPass({ timeout: 10000 });
    });
});

test.describe("SUB2 取消：级联终态卡 + 状态翻转为已取消", () => {
    test.use({
        hostCfg: {
            pickStub: true,
            scenario: [
                { role: "assistant", content: "", tool_calls: [
                    { id: "t1", type: "function", function: { name: "task", arguments: JSON.stringify({
                        description: "长命令测试",
                        prompt: "运行 ping -n 30 127.0.0.1 并报告",
                        subagent_type: "general",
                    }) } },
                ]},
                // 子代理：长命令（异步在途，等待期内可取消）
                { role: "assistant", content: "", tool_calls: [
                    { id: "c1", type: "function", function: { name: "runner.run", arguments: JSON.stringify({ command_line: "ping -n 30 127.0.0.1 > nul" }) } },
                ]},
                // 后续不会到达（取消）
                { role: "assistant", content: "不应到达" },
            ],
        },
    });
    test("取消后 spawn 卡翻转为已取消（不再悬挂进行中）", async ({ host, page }) => {
        await openFlow(page, host);
        await expect(page.locator(".subcard")).toHaveCount(1);
        await expect(page.locator(".subcard .stat", { hasText: "进行中" }))
            .toBeVisible({ timeout: 15000 });

        // 打开右栏过程视图 → 点取消（decide 4 级联取消整棵 turn）
        await page.locator(".subcard").click();
        await page.locator(".wcancel").click();

        await expect(page.locator(".errcard", { hasText: "已被用户取消" }).first())
            .toBeVisible({ timeout: 15000 });
        await expect(page.locator(".subcard .stat", { hasText: "已取消 · 点击查看" }))
            .toBeVisible({ timeout: 15000 });
    });
});
