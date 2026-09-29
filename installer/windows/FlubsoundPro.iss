; Flubsound Pro - Windows installer (Inno Setup 6), docs/11 E54.
;
; Built by tools/scripts/package-desktop.sh (CI `app` job, windows-2022) from
; the staged test folder:
;
;   ISCC.exe /DAppVersion=0.1.0 /DSourceDir=<dist\Flubsound-windows>
;            /DOutputDir=<dist> installer\windows\FlubsoundPro.iss
;
; Installs (per machine, needs administrator rights for the VST3 folder):
;   {autopf}\Flubsound Pro\Flubsound Pro.exe        the desktop app
;   {autopf}\Flubsound Pro\Flubsound FX.exe         the plug-in's standalone app
;   {commoncf64}\VST3\Flubsound FX.vst3             the VST3 plug-in bundle
;   Start menu: Flubsound Pro, Flubsound FX (standalone), Uninstall
; and registers an uninstaller (Settings > Apps), which removes all of that
; (folders it created go when empty). The user's settings,
; presets and logs (%APPDATA%\Flubsound) are left in place on uninstall.
; The installer is unsigned: SmartScreen warns until code signing exists.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist\Flubsound-windows"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist"
#endif

[Setup]
; The AppId identifies the installation for upgrades and the uninstaller:
; never change it.
AppId={{E94E7009-92BD-42A3-A8FA-62E50D5D05CB}
AppName=Flubsound Pro
AppVersion={#AppVersion}
AppVerName=Flubsound Pro {#AppVersion}
AppPublisher=Flubes and Claude (co-authors)
AppCopyright=Copyright (c) 2026 Flubes and Claude (co-authors)
VersionInfoVersion={#AppVersion}
VersionInfoProductName=Flubsound Pro
VersionInfoCompany=Flubes and Claude (co-authors)
DefaultDirName={autopf}\Flubsound Pro
DefaultGroupName=Flubsound Pro
DisableProgramGroupPage=yes
UninstallDisplayName=Flubsound Pro
UninstallDisplayIcon={app}\Flubsound Pro.exe
PrivilegesRequired=admin
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; Closes a running Flubsound Pro (Restart Manager) before files are replaced.
CloseApplications=yes
RestartApplications=no
InfoBeforeFile={#SourceDir}\TESTING.txt
OutputDir={#OutputDir}
OutputBaseFilename=FlubsoundPro-Setup-{#AppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\Flubsound Pro.exe"; DestDir: "{app}"; Flags: ignoreversion
#if FileExists(SourceDir + "\Plug-ins\Standalone\Flubsound FX.exe")
Source: "{#SourceDir}\Plug-ins\Standalone\Flubsound FX.exe"; DestDir: "{app}"; Flags: ignoreversion
#endif
Source: "{#SourceDir}\Plug-ins\VST3\Flubsound FX.vst3\*"; DestDir: "{commoncf64}\VST3\Flubsound FX.vst3"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceDir}\TESTING.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "{#SourceDir}\AUTHORS.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\VERSION.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{group}\Flubsound Pro"; Filename: "{app}\Flubsound Pro.exe"
#if FileExists(SourceDir + "\Plug-ins\Standalone\Flubsound FX.exe")
Name: "{group}\Flubsound FX (standalone)"; Filename: "{app}\Flubsound FX.exe"
#endif
Name: "{group}\Uninstall Flubsound Pro"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Flubsound Pro"; Filename: "{app}\Flubsound Pro.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Flubsound Pro.exe"; Description: "{cm:LaunchProgram,Flubsound Pro}"; Flags: nowait postinstall skipifsilent

