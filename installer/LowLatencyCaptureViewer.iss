; Low Latency Capture Viewer v1.0.3
; Build this script with Inno Setup 6 or newer from the installer directory.

#define MyAppName "Low Latency Capture Viewer"
#define MyAppVersion "1.0.3"
#define MyAppPublisher "seria-aa"
#define MyAppURL "https://github.com/seria-aa/LowLatencyCaptureViewer"
#define MyAppExeName "LowLatencyCaptureViewer.exe"
#define BuildDir "..\build-v100-release"

[Setup]
AppId={{A5B2F8BB-6F68-4A2F-BD8A-9AF2A3AA1000}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}/issues
AppUpdatesURL={#MyAppURL}/releases
DefaultDirName={autopf}\LowLatencyCaptureViewer
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputDir=..\..\outputs\v1.0.3
OutputBaseFilename=LowLatencyCaptureViewer_v1.0.3_Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
SetupIconFile=..\assets\LowLatencyCaptureViewer.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Uninstallable=yes
VersionInfoVersion=1.0.3.0
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName}
VersionInfoCopyright=Copyright (C) 2026 seria-aa

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"

[CustomMessages]
english.DeleteUserDataPrompt=Do you also want to delete your settings and diagnostic logs?%n%nChoose No to keep them for a future reinstall, or Yes to remove them permanently.
korean.DeleteUserDataPrompt=사용자 설정과 진단 로그도 삭제하시겠습니까?%n%n아니오를 선택하면 다음 설치를 위해 보존하고, 예를 선택하면 영구적으로 삭제합니다.

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional icons:"; Flags: unchecked

[Files]
Source: "{#BuildDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.ko.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\실행안내.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\DEPENDENCIES.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[Code]
var
  DeleteUserData: Boolean;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  DeleteUserData := False;
  if UninstallSilent then
    Exit;

  if not DirExists(ExpandConstant('{localappdata}\LowLatencyCaptureViewer')) then
    Exit;

  DeleteUserData := MsgBox(
    ExpandConstant('{cm:DeleteUserDataPrompt}'), mbConfirmation,
    MB_YESNO or MB_DEFBUTTON2) = IDYES;
end;

procedure CurUninstallStepChanged(UninstallStep: TUninstallStep);
begin
  if (UninstallStep = usPostUninstall) and DeleteUserData then
    DelTree(ExpandConstant('{localappdata}\LowLatencyCaptureViewer'),
      True, True, True);
end;
