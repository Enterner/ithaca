// Playwright 配置（e2e 分层：api=纯 HTTP 契约不开浏览器；ui=系统 Edge 零下载）。
// api 项目：纯 HTTP 契约（request 直连，不开浏览器）；ui 项目：系统 Edge（零下载）。
import { defineConfig } from "@playwright/test";

export default defineConfig({
    testDir: "tests/e2e",
    timeout: 90_000,
    retries: 1,          // UI 用例对时序抖动敏感；失败重试一次
    workers: 2,
    reporter: [["list"]],
    projects: [
        { name: "api", testMatch: /api\.spec\.mjs$/ },
        {
            name: "ui",
            testMatch: /ui\.spec\.mjs$/,
            use: { channel: "msedge", headless: true },
        },
    ],
});
