; This file is part of Go.dot - https://github.com/pob31/go.dot
;
; Copyright (C) 2026 Pierre-Olivier Boulant
;
; Go.dot is free software: you can redistribute it and/or modify it under the
; terms of the GNU General Public License as published by the Free Software
; Foundation, either version 3 of the License, or (at your option) any later
; version. Go.dot is distributed in the hope that it will be useful, but
; WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
; or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
; (LICENSE, at the repository root) for more details.
;
; SPDX-License-Identifier: GPL-3.0-or-later
;
; The Windows installer, for Inno Setup 6 (author, 2026-09-30: "We can build an
; inno installer"). It installs the folder `cmake --install --component wfg`
; lays down - the same folder the zip carries - and does the two things a zip
; cannot: it tells Windows that a .wfg is a Go.dot show, with the page icon and
; Go.dot.exe to open it, and it can be taken away again from Settings.
;
;   iscc "/DStage=<the installed folder>" "/DAppVersion=0.1.0" [/DAppNumbers=0.1.0]
;        "/O<where the setup goes>" packaging\windows\go.dot.iss
;
; release.yml runs it after the Windows install step, and smoke-tests the result.
;
; FOR THIS USER BY DEFAULT, into %LOCALAPPDATA%\Programs\Go.dot, which needs no
; administrator; the first page offers all users instead, into Program Files.
; The file type follows the choice (HKA): this user's classes, or the machine's.
;
; THE FILE TYPE is GoDot.Show. A .wfg shows Go.dot.exe's second icon - the
; page (packaging/windows/launcher.rc.in) - and opens with Go.dot.exe, which
; opens the show around it. Everything written is taken back on uninstall;
; the shows and %APPDATA%\Go.dot are the user's and are left where they are.
;
; NOT SIGNED: there is no Windows certificate, so SmartScreen warns about the
; setup as it does about the zip.

#ifndef Stage
  #error Stage is required: /DStage=<the folder cmake --install wrote>
#endif

#ifndef AppVersion
  #error AppVersion is required: /DAppVersion=X.Y.Z (a -suffix is allowed)
#endif

; The file version Windows shows for the setup program, which must be numbers
; alone: the version with any -dev.<sha> or -alpha.1 taken off, unless given.
#ifndef AppNumbers
  #define DashAt Pos("-", AppVersion)
  #if DashAt > 0
    #define AppNumbers Copy(AppVersion, 1, DashAt - 1)
  #else
    #define AppNumbers AppVersion
  #endif
#endif

[Setup]
; Fixed for ever: it is how Windows knows a new setup is the same program.
AppId={{F167EFF9-6164-4F6F-8D2F-2C4302C1109E}
AppName=Go.dot
AppVersion={#AppVersion}
AppVerName=Go.dot {#AppVersion}
AppPublisher=Pierre-Olivier Boulant
AppPublisherURL=https://github.com/pob31/go.dot
AppSupportURL=https://github.com/pob31/go.dot/issues
AppUpdatesURL=https://github.com/pob31/go.dot/releases
VersionInfoVersion={#AppNumbers}
VersionInfoProductName=Go.dot
VersionInfoDescription=Go.dot setup
DefaultDirName={autopf}\Go.dot
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
ChangesAssociations=yes
LicenseFile={#Stage}\LICENSE
SetupIconFile={#SourcePath}..\icons\Go.dot.ico
UninstallDisplayIcon={app}\Go.dot.exe
UninstallDisplayName=Go.dot
OutputBaseFilename=go.dot-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern

[Tasks]
Name: desktopicon; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#Stage}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Go.dot"; Filename: "{app}\Go.dot.exe"
Name: "{autodesktop}\Go.dot"; Filename: "{app}\Go.dot.exe"; Tasks: desktopicon

[Registry]
; .wfg is a Go.dot show: the default, and in the list "Open with" offers.
Root: HKA; Subkey: "Software\Classes\.wfg"; ValueType: string; ValueName: ""; ValueData: "GoDot.Show"; Flags: uninsdeletevalue uninsdeletekeyifempty
Root: HKA; Subkey: "Software\Classes\.wfg\OpenWithProgids"; ValueType: string; ValueName: "GoDot.Show"; ValueData: ""; Flags: uninsdeletevalue uninsdeletekeyifempty
; What a Go.dot show is: its name, its page icon, and what opens it.
Root: HKA; Subkey: "Software\Classes\GoDot.Show"; ValueType: string; ValueName: ""; ValueData: "Go.dot show"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\GoDot.Show\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\Go.dot.exe"",-2"
Root: HKA; Subkey: "Software\Classes\GoDot.Show\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\Go.dot.exe"" ""%1"""
; And Go.dot.exe, known to "Open with" by what it opens.
Root: HKA; Subkey: "Software\Classes\Applications\Go.dot.exe\SupportedTypes"; ValueType: string; ValueName: ".wfg"; ValueData: ""; Flags: uninsdeletekey

[Run]
; As the person who ran the setup, even when it was raised to install for everybody.
Filename: "{app}\Go.dot.exe"; Description: "{cm:LaunchProgram,Go.dot}"; Flags: nowait postinstall skipifsilent runasoriginaluser
