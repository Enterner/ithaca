@echo off
cd /d "%~dp0"
set "ITHACA_WEBUI_HOME=%~dp0data"
set "ITHACA_WEBUI_WEB=%~dp0web\dist"
set "ITHACA_WORKSPACE_DIR=%~dp0data\workspace"
if not exist "%~dp0data\sessions" mkdir "%~dp0data\sessions"
start "Ithaca" /b "%~dp0ithaca_host.exe" "%~dp0plugins\llm_mock_plugin.dll" "%~dp0plugins\llm_cloud_plugin.dll" "%~dp0plugins\workspace_plugin.dll" "%~dp0plugins\runner_plugin.dll" "%~dp0plugins\agent_plugin.dll" "%~dp0plugins\webui_plugin.dll"
