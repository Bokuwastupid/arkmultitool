; Inno Setup script for a compiled KOPT installer.
;
; This is the packaged form of what Install-KOPT.ps1 does by hand: same
; per-user install root, same shortcuts, same Apps & Features entry, and the
; same rule that %LOCALAPPDATA%\KOPT settings survive uninstall. The PowerShell
; installer exists because it needs no tooling at all; this one exists because a
; single signed .exe is what people expect to be handed.
;
; Build with:  installer\build-installer.ps1
; (or directly: ISCC.exe installer\kopt.iss /DSourceDir=..\build-msvc\dist)

#ifndef SourceDir
  #define SourceDir "..\build-msvc\dist"
#endif
#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif

[Setup]
AppId={{9C0F5F1E-7C1B-4C77-9F0A-5E1B0B7A2D31}
AppName=KOPT
AppVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\KOPT
DefaultGroupName=KOPT
DisableProgramGroupPage=yes
; Per-user throughout: no elevation prompt, nothing written outside the
; user's profile, no machine-wide state.
PrivilegesRequired=lowest
OutputDir=..\build-msvc\installer
OutputBaseFilename=KOPT-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
UninstallDisplayIcon={app}\KOPT_Inject.exe

[Files]
Source: "{#SourceDir}\KOPT_Inject.exe";   DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\kopt_injector.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceDir}\kopt_payload.dll";  DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\KOPT";           Filename: "{app}\KOPT_Inject.exe"; WorkingDir: "{app}"
Name: "{userdesktop}\KOPT";     Filename: "{app}\KOPT_Inject.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"

[Run]
Filename: "{app}\KOPT_Inject.exe"; Description: "Launch KOPT"; Flags: nowait postinstall skipifsilent

; Nothing under [UninstallDelete] for %LOCALAPPDATA%\KOPT on purpose: that is
; the user's configuration and log, and an uninstall must not throw away
; settings that a reinstall would otherwise pick straight back up.
