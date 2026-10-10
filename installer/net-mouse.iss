; Inno Setup 6 script for Net Mouse.
;
; Build:  cmake --build build --config Release --target installer
;   or:   ISCC /DAppVersion=0.1.0 /DBinDir=..\build\Release installer\net-mouse.iss
;
; Installs the NetMouse Windows service (does the work, also at the logon screen) and the control
; app (tray icon, settings). Settings, key and logs: %ProgramData%\NetMouse.
;
; Silent install: net-mouse-setup-<ver>.exe /VERYSILENT /ROLE=master|slave
;   autostart is on; add /MERGETASKS="!autostart" for a manually started service

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
; Unchecked: the service is set to start manually and the tray icon does not start at sign-in.
Name: "autostart"; Description: "Start {#AppName} automatically with Windows"; GroupDescription: "Startup:"
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#BinDir}\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "setup-helper.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion isreadme

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\setup-helper.ps1"" -Action prepare -Role ""{code:GetRole}"""; StatusMsg: "Preparing the settings..."; Flags: runhidden waituntilterminated
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\setup-helper.ps1"" -Action firewall -Exe ""{app}\{#AppExe}"""; StatusMsg: "Adding the firewall rule..."; Flags: runhidden waituntilterminated
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\setup-helper.ps1"" -Action service -Exe ""{app}\{#AppExe}"" -Autostart {code:AutostartFlag}"; StatusMsg: "Installing and starting the Net Mouse service..."; Flags: runhidden waituntilterminated
Filename: "{app}\{#AppExe}"; Description: "Open the {#AppName} control app"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "powershell.exe"; Parameters: "-NoProfile -ExecutionPolicy Bypass -File ""{app}\setup-helper.ps1"" -Action uninstall"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveServiceAndFirewall"

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

function AutostartFlag(Param: String): String;
begin
  if WizardIsTaskSelected('autostart') then
    Result := '1'
  else
    Result := '0';
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
  RolePage.Add('Keep the current role, or ask on first start');

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

// Stops the service (its agent restores the cursor and exits), asks control apps and older per-user
// versions to close cleanly, then ends anything left over (e.g. in other sessions).
procedure StopRunningInstances(Exe: String);
var
  ResultCode: Integer;
begin
  Exec('powershell.exe', '-NoProfile -Command "Stop-Service -Name NetMouse -Force -ErrorAction SilentlyContinue"', '',
       SW_HIDE, ewWaitUntilTerminated, ResultCode);
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
