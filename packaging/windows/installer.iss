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
AppName=VLTONE
AppVersion={#AppVersion}
AppVerName=VLTONE {#AppVersion}
AppPublisher=VLTONE
DefaultDirName={autopf}\VLTONE
DefaultGroupName=VLTONE
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
UninstallDisplayIcon={app}\bin\VLTONE.exe
SetupIconFile={#IconFile}
OutputDir={#OutputDir}
OutputBaseFilename=VLTONE-{#AppVersion}-x64-Setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
VersionInfoVersion={#AppVersion}.0
VersionInfoCompany=VLTONE
VersionInfoDescription=VLTONE installer
VersionInfoProductName=VLTONE
VersionInfoProductVersion={#AppVersion}.0
VersionInfoCopyright=Copyright (C) 2026 VLTONE. All rights reserved.

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#VcRedist}"; DestDir: "{tmp}"; DestName: "vc_redist.x64.exe"; Flags: deleteafterinstall

[InstallDelete]
; Remove obsolete launchers during an upgrade; AppId and saved data stay stable.
Type: files; Name: "{app}\bin\VLT Studio Pro.exe"
Type: files; Name: "{autoprograms}\VLT Studio Pro.lnk"
Type: files; Name: "{autodesktop}\VLT Studio Pro.lnk"

[Icons]
Name: "{autoprograms}\VLTONE"; Filename: "{app}\bin\VLTONE.exe"; WorkingDir: "{app}\bin"
Name: "{autodesktop}\VLTONE"; Filename: "{app}\bin\VLTONE.exe"; WorkingDir: "{app}\bin"; Tasks: desktopicon

[Registry]
; Keep the existing ProgID so project associations survive the rename.
Root: HKLM; Subkey: "Software\Classes\.vlt"; ValueType: string; ValueName: ""; ValueData: "VLTStudioPro.Project"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\VLTStudioPro.Project"; ValueType: string; ValueName: ""; ValueData: "VLTONE Project"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\VLTStudioPro.Project\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\VLTONE.exe,0"
Root: HKLM; Subkey: "Software\Classes\VLTStudioPro.Project\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\VLTONE.exe"" ""%1"""

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installing Microsoft Visual C++ Runtime..."; Flags: runhidden waituntilterminated

; User settings live in the OS roaming/local application-data folders. The
; installer intentionally never writes or deletes those folders, so an upgrade
; and an uninstall/reinstall do not destroy projects, preferences or scan data.
