#ifndef SourceDir
  #error SourceDir must point at the deployed application directory
#endif
#ifndef OutputDir
  #error OutputDir must point at the artifact directory
#endif
#ifndef AppVersion
  #error AppVersion must match the CMake project version
#endif
#ifndef VcRedist
  #error VcRedist must point at vc_redist.x64.exe
#endif
#ifndef IconFile
  #error IconFile must point at the Windows application icon
#endif

[Setup]
AppId={{C4B1512F-06CD-48F5-AF80-63DB4C6969F2}
AppName=VLT Studio Pro
AppVersion={#AppVersion}
AppVerName=VLT Studio Pro {#AppVersion}
AppPublisher=VLT Studio
DefaultDirName={autopf}\VLT Studio Pro
DefaultGroupName=VLT Studio Pro
DisableProgramGroupPage=yes
AllowNoIcons=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
PrivilegesRequired=admin
ChangesAssociations=yes
CloseApplications=yes
RestartApplications=no
UsePreviousAppDir=yes
UninstallDisplayIcon={app}\bin\VLT Studio Pro.exe
SetupIconFile={#IconFile}
OutputDir={#OutputDir}
OutputBaseFilename=VLT-Studio-Pro-{#AppVersion}-x64-Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
VersionInfoVersion={#AppVersion}.0
VersionInfoCompany=VLT Studio
VersionInfoDescription=VLT Studio Pro installer
VersionInfoProductName=VLT Studio Pro
VersionInfoProductVersion={#AppVersion}.0
VersionInfoCopyright=Copyright (C) 2026 VLT Studio. All rights reserved.

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#VcRedist}"; DestDir: "{tmp}"; DestName: "vc_redist.x64.exe"; Flags: deleteafterinstall

[Icons]
Name: "{autoprograms}\VLT Studio Pro"; Filename: "{app}\bin\VLT Studio Pro.exe"; WorkingDir: "{app}\bin"
Name: "{autodesktop}\VLT Studio Pro"; Filename: "{app}\bin\VLT Studio Pro.exe"; WorkingDir: "{app}\bin"; Tasks: desktopicon

[Registry]
Root: HKLM; Subkey: "Software\Classes\.vlt"; ValueType: string; ValueName: ""; ValueData: "VLTStudioPro.Project"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\VLTStudioPro.Project"; ValueType: string; ValueName: ""; ValueData: "VLT Studio Pro Project"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\VLTStudioPro.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\VLT Studio Pro.exe,0"
Root: HKLM; Subkey: "Software\Classes\VLTStudioPro.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\VLT Studio Pro.exe"" ""%1"""

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ Runtime..."; Flags: runhidden waituntilterminated

; User settings live in the OS roaming/local application-data folders. The
; installer intentionally never writes or deletes those folders, so an upgrade
; and an uninstall/reinstall do not destroy projects, preferences or scan data.
