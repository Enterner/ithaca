// tests/e2e/config/api.spec.mjs —— 配置契约（API 层）：
// api_key 只写不读（has_key 掩码下发、明文仅落盘）/ 留空按名保留旧值 /
// 未知模型自动建档 / SSE 长连接存活（kit::http reap 豁免的回归护栏）。
// cloud 模式用例以 ITHACA_LLM_ACTIVE="" 覆盖 fixture 的 mock 缺省：providers
// 按模式单侧下发，mock 模式下 POST providers 被整体忽略，key 往返必须跑在
// cloud 侧（config 种子不动，只换模式 env）。
import fs from "node:fs";
import net from "node:net";
import path from "node:path";

import { test, expect, getJson, postJson } from "../helpers/host.mjs";

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// 模型条目校验要求 context ≥1024（webui 供应商配置校验），缺了整份配置被拒
const CLOUD = {
    name: "cfg-cloud", type: "cloud",
    base_url: "https://api.example.com/v1", enabled: true,
    models: [{ id: "cfg-model-a", context: 128000, enabled: true }],
};

// cloud 模式宿主 + 保存一份带 key 的供应商（key 往返用例的公共底座）。
// 注意 postJson 签名是 (base, path, body)——path 拼在 base 后。
async function cloudHostWithKey(host) {
    const saved = await postJson(host.base, "/api/config", {
        providers: [{ ...CLOUD, api_key: "sk-e2e-test-key" }],
        current_provider: CLOUD.name,
    });
    expect(saved.body.ok, `配置保存被拒：${JSON.stringify(saved.body)}`).toBe(true);
}

test.describe("配置契约（cloud 模式）", () => {
    test.use({ hostCfg: { extraEnv: { ITHACA_LLM_ACTIVE: "" } } });

    test("CFG1 配置往返：api_key 只写不读——has_key 掩码下发，明文只进 config.json", async ({ host }) => {
        await cloudHostWithKey(host);
        const { body: cfg } = await getJson(host.base, "/api/config");
        const cloud = cfg.providers.find((p) => p.name === CLOUD.name);
        expect(cloud, "POST 的 cloud 供应商在 GET 中可见").toBeTruthy();
        expect(cloud.has_key).toBe(true);
        expect(JSON.stringify(cfg)).not.toContain("sk-e2e-test-key");
        expect(JSON.stringify(cfg)).not.toContain('"api_key"');
        const cfgFile = fs.readFileSync(path.join(host.home, "config.json"), "utf8");
        expect(cfgFile).toContain("sk-e2e-test-key");
    });

    test("CFG2 api_key 留空 = 按名保留旧值（key 不出前端也能改配置）", async ({ host }) => {
        await cloudHostWithKey(host);
        const { body: cfg } = await getJson(host.base, "/api/config");
        await postJson(host.base, "/api/config", {
            providers: cfg.providers, current_provider: CLOUD.name,
        });
        const cfgFile = fs.readFileSync(path.join(host.home, "config.json"), "utf8");
        expect(cfgFile).toContain("sk-e2e-test-key");
    });

    test("CFG3 未知模型自动建档进当前供应商（去重）", async ({ host }) => {
        await cloudHostWithKey(host);
        await postJson(host.base, "/api/model", { model: "cfg-auto-model" });
        const { body: cfg } = await getJson(host.base, "/api/config");
        const cur = cfg.providers.find((p) => p.name === cfg.current_provider);
        expect(cur.models.filter((m) => m.id === "cfg-auto-model")).toHaveLength(1);
    });
});

test.describe("SSE 存活（mock 缺省模式）", () => {
    test("CFG4 SSE 长连接存活：静默跨过回收窗口后广播仍可达", async ({ host }) => {
        const created = await postJson(host.base, "/api/session/new", { message: "创建 hello.txt" });
        expect(created.body.ok).toBe(true);
        // 等 turn 收口（等事实不等时间）——in_flight 门卫会拒并发 input
        await expect.poll(async () => {
            const { body: t } = await getJson(host.base, "/api/transcript?after=0");
            return t.records.some((r) => r.type === "assistant");
        }, { timeout: 15_000 }).toBe(true);

        const port = Number(new URL(host.base).port);
        const chunks = [];
        const client = net.connect(port, "127.0.0.1");
        client.on("error", () => {});   // 服务端关连接（RST）不是本用例的失败面——广播可达性才是
        await new Promise((r) => client.on("connect", r));
        client.on("data", (d) => chunks.push(d));
        client.write("GET /events HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
        await expect.poll(() => Buffer.concat(chunks).toString("utf8"), { timeout: 5_000 })
            .toContain("text/event-stream");
        chunks.length = 0;
        await sleep(17_000);   // 静默跨过 15s reap 回收窗口——本用例的存在意义
        await postJson(host.base, "/api/input", { line: "SSE 存活探测" });
        await expect.poll(() => Buffer.concat(chunks).toString("utf8"), { timeout: 10_000 })
            .toContain("data:");
        client.destroy();
    });
});
