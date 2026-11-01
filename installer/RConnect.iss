; Built by build-release.bat: ISCC /DAppVersion=<v> /DSourceDir=<dist\RConnect> /DOutputDir=<dist> /DIconFile=<icon.ico> RConnect.iss
#ifndef AppVersion
  #error Pass /DAppVersion=<version>
#endif
#ifndef SourceDir
  #error Pass /DSourceDir=<folder with the files to install>
#endif
#ifndef IconFile
  #error Pass /DIconFile=<icon.ico>
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{6F0C2A3E-5B1D-4C7A-9E58-3D2B7A10C4E1}
AppName=RConnect
AppVersion={#AppVersion}
AppVerName=RConnect {#AppVersion}
AppPublisher=Lurid Lane
AppCopyright=Copyright (c) 2026 Lurid Lane
DefaultDirName={autopf}\RConnect
DefaultGroupName=RConnect
DisableProgramGroupPage=yes
; Per user by default (no UAC); the user can still choose all users.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=RConnect-{#AppVersion}-setup
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\RConnect.exe
VersionInfoVersion={#AppVersion}
VersionInfoCompany=Lurid Lane
VersionInfoDescription=RConnect Setup
LicenseFile={#SourceDir}\LICENSE.txt
Compression=lzma2/ultra
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "turkish"; MessagesFile: "compiler:Languages\Turkish.isl"

[Tasks]
Name: "autostart"; Description: "{cm:AutoStart}"; Flags: unchecked
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[CustomMessages]
english.AutoStart=Start RConnect when Windows starts
turkish.AutoStart=Windows başladığında RConnect'i başlat

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\RConnect"; Filename: "{app}\RConnect.exe"
Name: "{group}\{cm:UninstallProgram,RConnect}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\RConnect"; Filename: "{app}\RConnect.exe"; Tasks: desktopicon

; Same entry the app writes in Settings > Start with Windows.
[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "RConnect"; ValueData: """{app}\RConnect.exe"" --autostart"; Tasks: autostart; Flags: uninsdeletevalue

[Run]
Filename: "{app}\RConnect.exe"; Description: "{cm:LaunchProgram,RConnect}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
; The app can be running from the Run entry.
Filename: "{sys}\taskkill.exe"; Parameters: "/f /im RConnect.exe"; Flags: runhidden; RunOnceId: "StopRConnect"
