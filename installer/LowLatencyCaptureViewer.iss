; Low Latency Capture Viewer v1.0.0
; Build this script with Inno Setup 6 or newer from the installer directory.

#define MyAppName "Low Latency Capture Viewer"
#define MyAppVersion "1.0.0"
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
OutputDir=..\..\outputs\v1.0.0
OutputBaseFilename=LowLatencyCaptureViewer_v1.0.0_Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
SetupIconFile=..\assets\LowLatencyCaptureViewer.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
Uninstallable=yes
VersionInfoVersion=1.0.0.0
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription={#MyAppName}
VersionInfoCopyright=Copyright (C) 2026 seria-aa

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "korean"; MessagesFile: "compiler:Languages\Korean.isl"

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

; User settings and logs under %LOCALAPPDATA% are intentionally not removed by
; the uninstaller, so a reinstall preserves the user's configuration.
