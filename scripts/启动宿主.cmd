@echo off
rem LLM 插件互斥装载（与桌面壳/dev.mjs 同策）：便携包面向真实使用只带 llm_cloud；
rem mock 仅供开发测试（node scripts/dev.mjs --provider mock）。
rem 插件清单唯一出处：scripts/plugins.mjs（本文件 = CLOUD_FULL 全链）。
cd /d "%~dp0"
set "ITHACA_WEBUI_HOME=%~dp0data"
set "ITHACA_WEBUI_WEB=%~dp0web\dist"
set "ITHACA_WORKSPACE_DIR=%~dp0data\workspace"
if not exist "%~dp0data\sessions" mkdir "%~dp0data\sessions"
start "Ithaca" /b "%~dp0ithaca_host.exe" "%~dp0plugins\llm_cloud_plugin.dll" "%~dp0plugins\workspace_plugin.dll" "%~dp0plugins\runner_plugin.dll" "%~dp0plugins\agent_plugin.dll" "%~dp0plugins\webui_plugin.dll" "%~dp0plugins\mcp_plugin.dll"
