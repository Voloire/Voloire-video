; Inno Setup script of the Voloire-video setup.exe. Built by scripts\package.ps1:
;   ISCC /DAppVersion=1.0.0 /DStageDir=<staged app> /DRepoDir=<repo> voloire-video.iss
#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef StageDir
  #error StageDir must point at the staged application folder
#endif
#ifndef RepoDir
  #define RepoDir ".."
#endif

[Setup]
AppId={{D805A9EB-DC61-4740-89FF-C14B5957BC12}
AppName=Voloire-video
AppVersion={#AppVersion}
AppVerName=Voloire-video {#AppVersion}
AppPublisher=Voloire
AppPublisherURL=https://github.com/Voloire/Voloire-video
AppSupportURL=https://github.com/Voloire/Voloire-video/issues
DefaultDirName={autopf}\Voloire-video
DefaultGroupName=Voloire-video
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
CloseApplications=yes
RestartApplications=no
SetupIconFile={#RepoDir}\assets\icon.ico
UninstallDisplayIcon={app}\voloire-video.exe
LicenseFile={#RepoDir}\LICENSE
OutputDir={#RepoDir}\dist
OutputBaseFilename=voloire-video-setup-{#AppVersion}

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked
Name: "autostart"; Description: "Start Voloire-video with Windows (tray icon only)"; Flags: unchecked

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{autoprograms}\Voloire-video"; Filename: "{app}\voloire-video.exe"
Name: "{autodesktop}\Voloire-video"; Filename: "{app}\voloire-video.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "Voloire-video"; \
  ValueData: """{app}\voloire-video.exe"" --minimized"; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\voloire-video.exe"; Description: "Launch Voloire-video"; Flags: nowait postinstall skipifsilent
