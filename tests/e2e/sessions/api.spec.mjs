// 会话创建生命周期 · API 层用例（方案 docs/会话生命周期-测试方案.md）。
// SC1（新口径）+ SC2a–d 的服务端镜像不变式：注册表只含已初始化会话。
import fs from "node:fs";
import path from "node:path";
import { test, expect, getJson, postJson } from "../helpers/host.mjs";

function transcriptLines(host, id) {
    const tf = path.join(host.home, "sessions", `${id}.transcript.jsonl`);
    if (!fs.existsSync(tf)) return null;
    return fs.readFileSync(tf, "utf8").split("\n").filter((l) => l.trim() !== "");
}

test("SC1 组合端点创建即初始化：注册表/transcript/回复/重启重放", async ({ host }) => {
    // 创建（带工作区 + 首条消息）
    const wsPath = path.join(host.ws, "proj");
    const r = await postJson(host.base, "/api/session/new", {
        message: "第一课：你好伊萨卡", workspace: wsPath,
    });
    expect(r.status).toBe(200);
    expect(r.body.ok).toBe(true);
    const sid = r.body.id;

    // 注册表 +1，turns=1（创建即初始化），workspace 归组
    const sessions = (await getJson(host.base, "/api/sessions")).body;
    expect(sessions).toHaveLength(1);
    expect(sessions[0].id).toBe(sid);
    expect(sessions[0].turns).toBe(1);
    expect(sessions[0].workspace).toContain("proj");

    // transcript 首条用户记录已落盘；工作区目录已创建；active 指向它
    const lines = transcriptLines(host, sid);
    expect(lines).not.toBeNull();
    expect(JSON.parse(lines[0]).type).toBe("user");
    expect(fs.existsSync(wsPath)).toBe(true);
    const meta = (await getJson(host.base, "/api/session")).body;
    expect(meta.id).toBe(sid);

    // 回复到达（mock 剧本）
    let assistant = null;
    for (let i = 0; i < 30 && !assistant; i++) {
        await new Promise((res) => setTimeout(res, 400));
        const t = (await getJson(host.base, "/api/transcript?after=0")).body;
        assistant = t.records.find((x) => x.type === "assistant" && /任务完成/.test(x.text ?? ""));
    }
    expect(assistant).not.toBeNull();

    // 重启 → 会话仍在、active 指向它、记录完整重放
    const before = transcriptLines(host, sid).length;
    host.stop();
    host.start();
    await host.waitReady();
    const meta2 = (await getJson(host.base, "/api/session")).body;
    expect(meta2.id).toBe(sid);
    expect(transcriptLines(host, sid).length).toBe(before);
    const sessions2 = (await getJson(host.base, "/api/sessions")).body;
    expect(sessions2).toHaveLength(1);
    expect(sessions2[0].turns).toBeGreaterThanOrEqual(1);
});

test("SC2a 镜像：裸 session/new 400，注册表不变", async ({ host }) => {
    // 先正常创建一个会话，等 turn 收口（否则裸调用会先撞上 in_flight 门卫）
    const created = await postJson(host.base, "/api/session/new", { message: "原会话" });
    expect(created.body.ok).toBe(true);
    for (let i = 0; i < 30; i++) {
        const t = (await getJson(host.base, "/api/transcript?after=0")).body;
        if (t.records.some((x) => x.type === "assistant")) break;
        await new Promise((res) => setTimeout(res, 400));
    }
    const before = (await getJson(host.base, "/api/sessions")).body;

    // 裸调用（无 message）⇒ 400 + 注册表分毫不动 + 无孤儿条目 + 无 transcript 文件
    const bare = await postJson(host.base, "/api/session/new", {});
    expect(bare.status).toBe(400);
    expect(bare.body.ok).toBe(false);
    const after = (await getJson(host.base, "/api/sessions")).body;
    expect(after).toHaveLength(before.length);
    for (const s of after) expect(s.turns).toBeGreaterThanOrEqual(1);
    const files = fs.readdirSync(path.join(host.home, "sessions")).filter((f) => f.endsWith(".transcript.jsonl"));
    expect(files).toHaveLength(1);   // 只有原会话的 transcript
});

test("SC2b 镜像：缺 message 的工作区创建 400，目录不落盘", async ({ host }) => {
    const wsPath = path.join(host.ws, "never-created");
    const r = await postJson(host.base, "/api/session/new", { workspace: wsPath });
    expect(r.status).toBe(400);
    expect(fs.existsSync(wsPath)).toBe(false);   // 目录创建延迟到合法创建
    expect((await getJson(host.base, "/api/sessions")).body).toHaveLength(0);
});

test("SC2c 启动清扫：孤儿条目与空 transcript 被兜底清除", async ({ host }) => {
    // 预置老版本遗留：turns=0 条目 + 空 transcript 文件（首装种子形态），再启动
    const orphanId = "sOrphan";
    const indexJson = {
        active: orphanId,
        sessions: [{ id: orphanId, title: "(新会话)", workspace: "", created: 0, updated: 0, turns: 0, tokens: 0 }],
    };
    fs.writeFileSync(path.join(host.home, "index.json"), JSON.stringify(indexJson));
    fs.writeFileSync(path.join(host.home, "sessions", `${orphanId}.transcript.jsonl`), "");
    host.stop();
    host.start();
    await host.waitReady();

    // 注册表空、无会话态（/api/session 空 id）、空 transcript 文件已删
    expect((await getJson(host.base, "/api/sessions")).body).toHaveLength(0);
    expect((await getJson(host.base, "/api/session")).body.id).toBe("");
    expect(fs.existsSync(path.join(host.home, "sessions", `${orphanId}.transcript.jsonl`))).toBe(false);
    // 无会话态下 /api/input 拒绝
    const inp = await postJson(host.base, "/api/input", { line: "不应该被接受" });
    expect(inp.status).toBe(400);
});

test("SC2d 镜像：失败创建后原会话与 active 分毫未动", async ({ host }) => {
    const created = await postJson(host.base, "/api/session/new", { message: "原会话" });
    const sid = created.body.id;
    for (let i = 0; i < 30; i++) {   // 等 turn 收口（记录数稳定）
        const t = (await getJson(host.base, "/api/transcript?after=0")).body;
        if (t.records.some((x) => x.type === "assistant")) break;
        await new Promise((res) => setTimeout(res, 400));
    }
    const beforeRecords = (await getJson(host.base, "/api/transcript?after=0")).body.records.length;

    await postJson(host.base, "/api/session/new", {});   // 裸调用 400（SC2a 已断言状态码）
    expect((await getJson(host.base, "/api/session")).body.id).toBe(sid);
    expect((await getJson(host.base, "/api/transcript?after=0")).body.records.length).toBe(beforeRecords);
    const s = (await getJson(host.base, "/api/sessions")).body.find((x) => x.id === sid);
    expect(s.turns).toBe(1);
});
