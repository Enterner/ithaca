// tests/e2e/attachments/api.spec.mjs —— 附件管线（API 层）：
// 上传落盘（文件名净化/撞名避让）/ 附件图片字节出口（uploads 白名单归界）/
// transcript 用户记录元数据 / 越界与超大拒绝。预览 <img> 渲染走 UI 层——此处锁
// 端点契约。上传不经 apply 提案（用户文件进场，非内容修改），落点 .ithaca/uploads/。
import path from "node:path";

import { test, expect, getJson, postJson } from "../helpers/host.mjs";

// 内容随意字节（端点按扩展名出 Content-Type，不校验字节合法性）
const PNG_BYTES = Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 1, 2, 3, 255]);
const b64 = (buf) => buf.toString("base64");

async function waitTurnDone(host) {
    await expect.poll(async () => {
        const t = (await getJson(host.base, "/api/transcript?after=0")).body;
        return t.records.some((x) => x.type === "assistant" && /任务完成/.test(x.text ?? ""));
    }, { timeout: 15_000 }).toBe(true);
}

async function getAttachment(host, rel) {
    const r = await fetch(`${host.base}/api/attachment?path=${encodeURIComponent(rel)}`);
    return { status: r.status, contentType: r.headers.get("content-type") ?? "", body: Buffer.from(await r.arrayBuffer()) };
}

test.describe("无工作区（草稿态）", () => {
    test.use({ hostCfg: { extraEnv: { ITHACA_WORKSPACE_DIR: "" } } });

    test("ATT1 无工作区上传被拒（草稿态零落点——附件路径必须属于真实工作区）", async ({ host }) => {
        const r = await postJson(host.base, "/api/upload", { name: "a.png", data: b64(PNG_BYTES) });
        expect(r.body.ok).toBe(false);
        expect(r.body.error).toContain("没有工作区");
    });
});

test("ATT2 上传落盘：撞名避让 + 图片字节出口（content-type 与字节逐一致）", async ({ host }) => {
    const wsPath = path.join(host.ws, "att-ws");
    const created = await postJson(host.base, "/api/session/new", { message: "附件会话", workspace: wsPath });
    expect(created.body.ok).toBe(true);

    const up1 = await postJson(host.base, "/api/upload", { name: "报告.png", data: b64(PNG_BYTES) });
    expect(up1.body.ok).toBe(true);
    expect(up1.body.path).toBe(".ithaca/uploads/报告.png");
    const up2 = await postJson(host.base, "/api/upload", { name: "报告.png", data: b64(PNG_BYTES) });
    expect(up2.body.path).toBe(".ithaca/uploads/报告 (1).png");   // 撞名加序号，不破坏扩展名

    const img = await getAttachment(host, up1.body.path);
    expect(img.status).toBe(200);
    expect(img.contentType).toContain("image/png");
    expect(img.body.equals(PNG_BYTES)).toBe(true);   // 中文文件名经 %XX 解码取回，字节逐一致
    const img2 = await getAttachment(host, up2.body.path);
    expect(img2.status).toBe(200);   // 带空格与括号的落盘名同样可取回
});

test("ATT3 字节出口归界：穿越/非图片/不存在一律 404，不开二进制口子", async ({ host }) => {
    const wsPath = path.join(host.ws, "att-ws");
    const created = await postJson(host.base, "/api/session/new", { message: "附件会话", workspace: wsPath });
    expect(created.body.ok).toBe(true);
    await postJson(host.base, "/api/upload", { name: "笔记.txt", data: b64(PNG_BYTES) });

    // 穿越到 uploads 外（config.json 真实存在于 home，但 jail 是工作区——双保险断言 404）
    expect((await getAttachment(host, ".ithaca/uploads/../secret.txt")).status).toBe(404);
    expect((await getAttachment(host, ".ithaca/uploads/nope.png")).status).toBe(404);
    // 非图片扩展名：预览的唯一消费方是 <img>，其余文件继续走 /api/fs/read 文本护栏
    expect((await getAttachment(host, ".ithaca/uploads/笔记.txt")).status).toBe(404);
});

test("ATT4 发送携带附件：transcript 用户记录带展示元数据（name/path/mime）", async ({ host }) => {
    const wsPath = path.join(host.ws, "att-ws");
    const created = await postJson(host.base, "/api/session/new", { message: "附件会话", workspace: wsPath });
    expect(created.body.ok).toBe(true);
    await waitTurnDone(host);   // in_flight 门卫：新 turn 前等上一轮收口

    const up = await postJson(host.base, "/api/upload", { name: "截图.png", data: b64(PNG_BYTES) });
    expect(up.body.ok).toBe(true);
    const sent = await postJson(host.base, "/api/input", { line: "看看这张截图", attachments: [up.body.path] });
    expect(sent.body.ok).toBe(true);
    await expect.poll(async () => {
        const t = (await getJson(host.base, "/api/transcript?after=0")).body;
        const user = t.records.filter((x) => x.type === "user").pop();
        return user && Array.isArray(user.attachments) ? user.attachments : null;
    }, { timeout: 15_000 }).toEqual([expect.objectContaining({
        name: "截图.png", path: ".ithaca/uploads/截图.png", mime: "image/png",
    })]);
});

test("ATT5 上传数据面拒绝：坏 base64 点名，超 20MB 上限拒绝", async ({ host }) => {
    const wsPath = path.join(host.ws, "att-ws");
    const created = await postJson(host.base, "/api/session/new", { message: "附件会话", workspace: wsPath });
    expect(created.body.ok).toBe(true);

    const bad = await postJson(host.base, "/api/upload", { name: "x.png", data: "不是base64!!" });
    expect(bad.body.ok).toBe(false);
    expect(bad.body.error).toContain("base64");

    // base64 形态上限（28MB 字符 ≈ 解码 21MB）——请求体仍在 32MB 请求缓冲内
    const huge = await postJson(host.base, "/api/upload", { name: "big.png", data: "A".repeat(28 * 1024 * 1024 + 1) });
    expect(huge.body.ok).toBe(false);
    expect(huge.body.error).toContain("20MB");
});
