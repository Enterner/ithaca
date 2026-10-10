; Ithaca 安装包（Inno Setup 6）——二期交付物。
; 由 scripts/pack.mjs 调 ISCC 编译：ISCC /DAppVersion=0.1.0 scripts\installer.iss
; 可选宏：/DStageDir=<组装目录>（默认 pack\Ithaca-<版本>-x64，即 zip 的内容）、
;         /DWithRedist（stage 里已带 vc_redist.x64.exe 时静默安装）
;
; 设计决策：per-user 安装（{localappdata}\Programs\Ithaca，VS Code User Setup 先例）——
; 不需要管理员；exe 旁 data\ 目录可写，桌面壳的便携双模式布局代码零改动直接适用。

#define AppName "Ithaca"
#define AppPublisher "Ithaca"
#define AppExe "ithaca.exe"
#ifndef AppVersion
#define AppVersion "0.0.0"
#endif
#ifndef StageDir
#define StageDir "..\pack\Ithaca-" + AppVersion + "-x64"
#endif
#ifndef PackDir
#define PackDir "..\pack"
#endif

[Setup]
AppId={{6528CBA2-9009-4A44-A7D8-E75497CDD4F1}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\Programs\{#AppName}
PrivilegesRequired=lowest
OutputDir={#PackDir}
OutputBaseFilename=Ithaca-{#AppVersion}-x64-setup
SourceDir=.
Compression=lzma2/max
SolidCompression=yes
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\{#AppExe}
CloseApplications=yes
WizardStyle=modern

[Languages]
; 简中翻译随仓走（scripts/l10n/）——官方安装器不带，其他机器 clone 后即可复现编译
Name: "chinese"; MessagesFile: "l10n\ChineseSimplified.isl"

[Files]
Source: "{#StageDir}\ithaca.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\ithaca_host.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\plugins\*"; DestDir: "{app}\plugins"; Flags: ignoreversion recursesubdirs
Source: "{#StageDir}\web\*"; DestDir: "{app}\web"; Flags: ignoreversion recursesubdirs
Source: "{#StageDir}\启动宿主.cmd"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\README.txt"; DestDir: "{app}"; Flags: ignoreversion
#ifdef WithRedist
Source: "{#StageDir}\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
#endif
Source: "{#StageDir}\MicrosoftEdgeWebview2Setup.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall skipifsourcedoesntexist

[Icons]
Name: "{autoprograms}\Ithaca"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\Ithaca"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "创建桌面快捷方式"; Flags: unchecked

[Run]
#ifdef WithRedist
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; \
    StatusMsg: "安装 VC++ 运行库…"; Flags: waituntilterminated; Check: NeedVCRedist
#endif
Filename: "{tmp}\MicrosoftEdgeWebview2Setup.exe"; Parameters: "/install /silent /norestart"; \
    StatusMsg: "安装 WebView2 运行时…"; Flags: waituntilterminated; Check: NeedWebView2
Filename: "{app}\{#AppExe}"; Description: "启动 Ithaca"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
; 用户数据（data\ 会话与配置）保留，不随卸载删除——卸载不等于销毁工作成果

[Code]
// WebView2 运行时检测：Evergreen 的 pv 值写在 EdgeUpdate\Clients 下（HKLM/HKCU 双查）
function NeedWebView2(): Boolean;
var
  pv: String;
begin
  Result := True;
  if RegQueryStringValue(HKLM, 'SOFTWARE\WOW6432Node\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', pv) then
    Result := False
  else if RegQueryStringValue(HKCU, 'Software\Microsoft\EdgeUpdate\Clients\{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}', 'pv', pv) then
    Result := False;
end;

#ifdef WithRedist
// 已有较新的 VC++ 运行库则跳过（检测 msvcp140.dll 的存在即可——精确版本比对交给 redist 自身）
function NeedVCRedist(): Boolean;
begin
  Result := not FileExists(ExpandConstant('{sys}\msvcp140.dll'));
end;
#endif
