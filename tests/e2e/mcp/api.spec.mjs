// MCP 插件接入链路用例：mock MCP server 经 mcp
// 插件接入 agent 工具链，mock LLM 场景驱动真实工具调用，断言 transcript 全链路。
import path from "node:path";
import { test, expect, getJson, postJson, root } from "../helpers/host.mjs";

const serverScript = path.join(root, "tests", "fixtures", "mock-mcp-server.mjs");

test.describe("MCP 接入", () => {
    test.use({
        hostCfg: {
            mcpServers: [{ name: "demo", command: "node", args: [serverScript] }],
            scenario: [
                { role: "assistant", content: "", tool_calls: [
                    { id: "m1", type: "function",
                      function: { name: "mcp.demo__add",
                                  arguments: JSON.stringify({ a: 2, b: 3 }) } },
                ]},
                { role: "assistant", content: "算完了" },
            ],
        },
    });

    test("MCP1 server 工具经统一工具表被 LLM 调用并回灌结果", async ({ host }) => {
        const healthWatch = (async () => {
            for (let i = 0; i < 60; i++) {
                try { const r = await fetch(host.base + "/api/health"); if (!r.ok) console.log("!! health 异常", r.status); }
                catch (e) { console.log("!! 宿主失联", Math.floor(i / 2) + "s"); }
                await new Promise((r) => setTimeout(r, 500));
            }
        })();
        healthWatch.catch(() => {});
        // 等 server 拉起完成（异步化：activate 立即返回，握手在后台）
        for (let i = 0; i < 60; i++) {
            const st = await getJson(host.base, "/api/mcp/status");
            const demo = (st.body.servers || []).find((x) => x.name === "demo");
            if (demo && demo.alive) break;
            await new Promise((r) => setTimeout(r, 500));
        }
        await postJson(host.base, "/api/session/new", { message: "算 2+3" });
        let t;
        for (let i = 0; i < 40; i++) {
            t = (await getJson(host.base, "/api/transcript?after=0")).body;
            if (t.records.some((x) => x.type === "assistant" && /算完了/.test(x.text || ""))) break;
            await new Promise((r) => setTimeout(r, 400));
        }
        // 工具调用行（agent 工具集含 MCP 工具 ⇒ LLM 可见可调）
        const call = t.records.find((x) => x.type === "tool_call");
        expect(call?.tool).toBe("mcp.demo__add");
        // 结果回灌（mock 返回 2+3=5）
        const res = t.records.find((x) => x.type === "tool" && x.tool === "mcp.demo__add");
        expect(res?.ok).toBe(1);   // webui 记录 ok 为 1/0（ToolResult.e->ok 直传）
        expect(res?.brief).toContain("5");
        // 收口
        expect(t.records.some((x) => x.type === "assistant" && /算完了/.test(x.text || ""))).toBe(true);
    });
});

test.describe("MCP 容错", () => {
    // 未配 mcpServers：mcp 插件空表发布，宿主与普通对话零影响
    test.use({
        hostCfg: {
            scenario: [{ role: "assistant", content: "普通对话收口" }],
        },
    });

    test("MCP2 server 未配置时宿主正常、普通对话不受影响", async ({ host }) => {
        const h = await getJson(host.base, "/api/health");
        expect(h.body.ok).toBe(true);
        await postJson(host.base, "/api/session/new", { message: "普通对话" });
        let t;
        for (let i = 0; i < 40; i++) {
            t = (await getJson(host.base, "/api/transcript?after=0")).body;
            if (t.records.some((x) => x.type === "assistant")) break;
            await new Promise((r) => setTimeout(r, 400));
        }
        expect(t.records.some((x) => x.type === "user")).toBe(true);
        expect(t.records.some((x) => x.type === "assistant")).toBe(true);
        // 无 MCP 工具记录
        expect(t.records.some((x) => /mcp\./.test(x.tool || ""))).toBe(false);
    });
});
