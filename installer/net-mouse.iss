; Inno Setup 6 script for Net Mouse.
;
; Build:  cmake --build build --config Release --target installer
;   or:   ISCC /DAppVersion=0.1.0 /DBinDir=..\build\Release installer\net-mouse.iss
;
; Silent install: net-mouse-setup-<ver>.exe /VERYSILENT /ROLE=master|slave  (autostart is on; add /MERGETASKS="!autostart" to skip it)

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef BinDir
  #define BinDir "..\build\Release"
#endif

#define AppName "Net Mouse"
#define AppExe "net-mouse.exe"

[Setup]
AppId={{6F3C1A52-8E4B-4D7A-9C21-5B0E7F3D2A94}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Net Mouse
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.15063
OutputDir=..\build\installer
OutputBaseFilename=net-mouse-setup-{#AppVersion}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
VersionInfoVersion={#AppVersion}
VersionInfoProductName={#AppName}
CloseApplications=no

[Tasks]
; Checked by default. Upgrades keep the previous choice; silent installs can opt out with /MERGETASKS="!autostart".
Name: "autostart"; Description: "Start {#AppName} on Windows startup (when a user signs in)"; GroupDescription: "Startup:"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#BinDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "setup-helper.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion isreadme

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"; Parameters: "{code:DefaultRoleArgs}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Parameters: "{code:DefaultRoleArgs}"; Tasks: desktopicon

[Run]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\setup-helper.ps1"" -Action firewall -Exe ""{app}\{#AppExe}"""; StatusMsg: "Adding the firewall rule..."; Flags: runhidden waituntilterminated
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\setup-helper.ps1"" -Action autostart -Exe ""{app}\{#AppExe}"" -Role ""{code:GetRole}"""; StatusMsg: "Registering autostart..."; Tasks: autostart; Flags: runhidden waituntilterminated
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\setup-helper.ps1"" -Action remove-autostart"; Tasks: not autostart; Flags: runhidden waituntilterminated
Filename: "{app}\{#AppExe}"; Parameters: "{code:DefaultRoleArgs}"; Description: "Start {#AppName} now"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\setup-helper.ps1"" -Action uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveFirewallAndTask"

[Code]
var
  RolePage: TInputOptionWizardPage;

function RoleFromIndex(Index: Integer): String;
begin
  case Index of
    0: Result := 'master';
    1: Result := 'slave';
  else
    Result := '';
  end;
end;

function GetRole(Param: String): String;
begin
  Result := RoleFromIndex(RolePage.SelectedValueIndex);
end;

function DefaultRoleArgs(Param: String): String;
begin
  if GetRole('') <> '' then
    Result := '--default-role ' + GetRole('')
  else
    Result := '';
end;

procedure InitializeWizard;
var
  Role: String;
begin
  RolePage := CreateInputOptionPage(wpSelectDir,
    'Role of this PC', 'How will Net Mouse be used on this PC?',
    'Install Net Mouse on every PC in the same local network: one master and any number of slaves. ' +
    'The role can be changed later from the tray icon menu.',
    True, False);
  RolePage.Add('Master: this PC''s mouse controls this PC and the slave PCs');
  RolePage.Add('Slave: this PC is controlled by the master PC''s mouse');
  RolePage.Add('Ask on first start');

  Role := Lowercase(ExpandConstant('{param:ROLE|}'));
  if Role = '' then
    Role := GetPreviousData('Role', '');
  if Role = 'master' then
    RolePage.SelectedValueIndex := 0
  else if Role = 'slave' then
    RolePage.SelectedValueIndex := 1
  else
    RolePage.SelectedValueIndex := 2;
end;

procedure RegisterPreviousData(PreviousDataKey: Integer);
begin
  SetPreviousData(PreviousDataKey, 'Role', GetRole(''));
end;

// Asks running instances to exit cleanly (restoring the cursor), then kills any left over,
// e.g. instances in other sessions.
procedure StopRunningInstances(Exe: String);
var
  ResultCode: Integer;
begin
  if FileExists(Exe) then
  begin
    Exec(Exe, '--quit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(1500);
  end;
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM {#AppExe}', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopRunningInstances(ExpandConstant('{app}\{#AppExe}'));
  Result := '';
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then
    StopRunningInstances(ExpandConstant('{app}\{#AppExe}'));
end;
