; Mettle GUI installer (Inno Setup).
;
; Build locally:
;   iscc Mettle.iss
; Stamp a version: iscc /DMyAppVersion=0.13.0 Mettle.iss
; CI builds this in release.yml and attaches Mettle-Setup.exe to the Release.
;
; WizardStyle uses Inno Setup 6's built-in modern UI (HiDPI, light/dark aware).
; No custom bitmap assets — branding comes from SetupIconFile and copy in [Messages].
;
; The installer lets the user choose a per-user or all-users install at launch
; (PrivilegesRequiredOverridesAllowed=dialog). Install location, PATH scope, and
; the file association all follow that choice automatically via the {auto*}
; constants and the HKA registry root.

#ifndef MyAppVersion
  #define MyAppVersion "v0.13.0"
#endif

#define MyAppName "Mettle"
#define MyRepoUrl "https://github.com/The-Mettle-Project/Mettle"

[Setup]
AppId={{D9F4D8EF-0B2C-4B37-A9B0-5A6E30C3D2A1}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher=The Mettle Project
AppPublisherURL={#MyRepoUrl}
AppSupportURL={#MyRepoUrl}/issues
AppUpdatesURL={#MyRepoUrl}/releases
AppCopyright=Copyright (C) The Mettle Project. Apache-2.0.
DefaultDirName={code:DefaultInstallDir}
DefaultGroupName=Mettle
UninstallDisplayIcon={app}\mettle.ico
ChangesEnvironment=yes
ChangesAssociations=yes
UsePreviousAppDir=no
UsePreviousGroup=no
UsePreviousTasks=no
WizardStyle=modern dynamic windows11
LicenseFile=..\LICENSE
UninstallDisplayName=Mettle {#MyAppVersion}
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Let the user pick "just me" (no UAC) or "all users" (UAC) at launch. The
; {auto*} constants below resolve to the matching location and registry hive.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
OutputDir=out
OutputBaseFilename=Mettle-Setup
SetupIconFile=..\mettle.ico
DisableProgramGroupPage=yes
DisableWelcomePage=no
DisableFinishedPage=no

[Messages]
SetupAppTitle=Mettle {#MyAppVersion} Setup
SetupWindowTitle=Mettle {#MyAppVersion} Setup
BeveledLabel=Mettle {#MyAppVersion}
WelcomeLabel1=Welcome to the Mettle {#MyAppVersion} Setup Wizard
WelcomeLabel2=This will install Mettle on your computer.%n%nMettle is a native x86-64 compiler with standard library, runtime helpers, and documentation.%n%nIt is recommended that you close other applications before continuing.%n%nClick Next to continue, or Cancel to exit Setup.
SelectDirLabel3=Setup will install Mettle in the following folder.
SelectTasksLabel2=Select the additional tasks you would like Setup to perform:
FinishedHeadingLabel=Completing the Mettle {#MyAppVersion} Setup Wizard
FinishedLabelNoIcons=Mettle has been installed on your computer.%n%nOpen a new Command Prompt or PowerShell window and run:%n%n    mettle --version%n%nDocumentation is available via mettle help and from the Start menu shortcuts.
FinishedLabel=Mettle has been installed on your computer.%n%nOpen a new Command Prompt or PowerShell window and run:%n%n    mettle --version%n%nDocumentation is available via mettle help and from the Start menu shortcuts.
LicenseLabel=Mettle is distributed under the Apache License 2.0. If you accept the terms of the agreement, click I Agree to continue.
LicenseLabel3=Please read the following license agreement. You must accept the terms of the agreement before continuing with the installation.

[Tasks]
Name: "addtopath"; Description: "Add Mettle to the {code:PathScopeLabel} &PATH (recommended)"; GroupDescription: "Additional tasks:"; Flags: checkedonce
Name: "associate"; Description: "Associate &.mettle files with Mettle"; GroupDescription: "Additional tasks:"

[Files]
Source: "..\bin\mettle.exe"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "mettle-build.bat"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "..\mettle.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\stdlib\*"; DestDir: "{app}\stdlib"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\bin\runtime\*"; DestDir: "{app}\runtime"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\bin\mlopt\*"; DestDir: "{app}\mlopt"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Mettle on GitHub"; Filename: "{#MyRepoUrl}"
Name: "{group}\Uninstall Mettle"; Filename: "{uninstallexe}"

[Registry]
; HKA = HKLM for an all-users install, HKCU for a per-user install.
Root: HKA; Subkey: "Software\Classes\.mettle"; ValueType: string; ValueName: ""; ValueData: "MettleFile"; Tasks: associate; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\MettleFile"; ValueType: string; ValueName: ""; ValueData: "Mettle Source File"; Tasks: associate; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\MettleFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\mettle.ico"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\MettleFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\mettle.exe"" ""%1"""; Tasks: associate

[Run]
Filename: "{cmd}"; Parameters: "/K ""{app}\bin\mettle.exe"" --version & echo. & echo Get started: mettle --build your_file.mettle -o your_file.exe & echo Docs: mettle help build"; WorkingDir: "{app}"; Description: "&Launch a terminal to verify the installation"; Flags: postinstall nowait skipifsilent unchecked

[Code]
var
  ExistingInstallFound: Boolean;
  ExistingInstallDir: string;
  ExistingInstallVersion: string;
  ExistingInstallScope: string;
  LegacyInstallFound: Boolean;
  LegacyInstallDir: string;
  LegacyInstallVersion: string;
  InstallDiscoveryDone: Boolean;

const
  AppUninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{D9F4D8EF-0B2C-4B37-A9B0-5A6E30C3D2A1}_is1';
  UserEnvKey = 'Environment';
  MachineEnvKey = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';
  LegacyStartMenuName = 'Methlang';
  LegacyFileClass = 'MettleFile';

function RootName(const RootKey: Integer): string;
begin
  if RootKey = HKEY_LOCAL_MACHINE then
    Result := 'all-users'
  else
    Result := 'current-user';
end;

function CleanDir(const Dir: string): string;
begin
  Result := RemoveBackslash(ExpandConstant(Dir));
end;

function IsLegacyDir(const Dir: string): Boolean;
begin
  Result := Pos('METHLANG', Uppercase(Dir)) > 0;
end;

function IsMettleDir(const Dir: string): Boolean;
begin
  Result := (Dir <> '') and not IsLegacyDir(Dir);
end;

function QueryInstallDir(const RootKey: Integer; var Dir: string): Boolean;
begin
  Result :=
    RegQueryStringValue(RootKey, AppUninstallKey, 'InstallLocation', Dir) or
    RegQueryStringValue(RootKey, AppUninstallKey, 'Inno Setup: App Path', Dir);

  if Result then
    Dir := CleanDir(Dir);
end;

function QueryInstallVersion(const RootKey: Integer): string;
begin
  if not RegQueryStringValue(RootKey, AppUninstallKey, 'DisplayVersion', Result) then
    Result := '';
end;

procedure NoteInstall(const RootKey: Integer; const Dir: string);
begin
  if IsLegacyDir(Dir) then
  begin
    LegacyInstallFound := True;
    LegacyInstallDir := Dir;
    LegacyInstallVersion := QueryInstallVersion(RootKey);
  end
  else if IsMettleDir(Dir) and (not ExistingInstallFound) then
  begin
    ExistingInstallFound := True;
    ExistingInstallDir := Dir;
    ExistingInstallVersion := QueryInstallVersion(RootKey);
    ExistingInstallScope := RootName(RootKey);
  end;
end;

procedure DiscoverExistingInstalls;
var
  Dir: string;
begin
  if InstallDiscoveryDone then
    Exit;

  InstallDiscoveryDone := True;
  if QueryInstallDir(HKEY_CURRENT_USER, Dir) then
    NoteInstall(HKEY_CURRENT_USER, Dir);
  if QueryInstallDir(HKEY_LOCAL_MACHINE, Dir) then
    NoteInstall(HKEY_LOCAL_MACHINE, Dir);
end;

function FindExistingInstallForRoot(const RootKey: Integer; var Dir, Version: string): Boolean;
begin
  Result := QueryInstallDir(RootKey, Dir) and IsMettleDir(Dir);
  if Result then
    Version := QueryInstallVersion(RootKey)
  else
    Version := '';
end;

function FindMatchingExistingInstall(var Dir, Version, Scope: string): Boolean;
var
  RootKey: Integer;
begin
  if IsAdminInstallMode then
    RootKey := HKEY_LOCAL_MACHINE
  else
    RootKey := HKEY_CURRENT_USER;

  Result := FindExistingInstallForRoot(RootKey, Dir, Version);
  if Result then
    Scope := RootName(RootKey)
  else
    Scope := RootName(RootKey);
end;

function DefaultInstallDir(Param: string): string;
var
  Dir: string;
  Version: string;
  Scope: string;
begin
  DiscoverExistingInstalls;
  if FindMatchingExistingInstall(Dir, Version, Scope) then
    Result := Dir
  else
    Result := ExpandConstant('{autopf}\Mettle');
end;

procedure InitializeWizard;
var
  Dir: string;
  Version: string;
  Scope: string;
  UpgradeText: string;
begin
  DiscoverExistingInstalls;

  if FindMatchingExistingInstall(Dir, Version, Scope) then
  begin
    UpgradeText := 'Setup found Mettle';
    if Version <> '' then
      UpgradeText := UpgradeText + ' ' + Version;
    UpgradeText := UpgradeText + ' in ' + Dir + ' for the ' + Scope + ' install.';

    WizardForm.WelcomeLabel1.Caption := 'Welcome to the Mettle {#MyAppVersion} Setup Wizard';
    WizardForm.WelcomeLabel2.Caption :=
      UpgradeText + #13#10 + #13#10 +
      'Setup will refresh the compiler, standard library, runtime helpers, and documentation in the existing location.';
  end
  else if ExistingInstallFound then
  begin
    WizardForm.WelcomeLabel1.Caption := 'Welcome to the Mettle {#MyAppVersion} Setup Wizard';
    WizardForm.WelcomeLabel2.Caption :=
      'Setup found an existing ' + ExistingInstallScope + ' Mettle install in ' + ExistingInstallDir + '.' + #13#10 + #13#10 +
      'This run is using the ' + Scope + ' install mode, so it will use a separate clean Mettle folder. Restart setup and choose the matching install mode if you want to upgrade that existing install in place.';
  end
  else if LegacyInstallFound then
  begin
    WizardForm.WelcomeLabel1.Caption := 'Welcome to the Mettle {#MyAppVersion} Setup Wizard';
    WizardForm.WelcomeLabel2.Caption :=
      'Setup found an older Methlang-era install in ' + LegacyInstallDir + '.' + #13#10 + #13#10 +
      'Mettle will install to a clean Mettle folder and remove stale Methlang shortcuts, PATH entries, and file associations when it finishes.';
  end;
end;

{ Human-readable label for the PATH task, reflecting the chosen install scope. }
function PathScopeLabel(Param: string): string;
begin
  if IsAdminInstallMode then
    Result := 'system'
  else
    Result := 'user';
end;

function PathRootKey: Integer;
begin
  if IsAdminInstallMode then
    Result := HKEY_LOCAL_MACHINE
  else
    Result := HKEY_CURRENT_USER;
end;

function PathKeyName: string;
begin
  if IsAdminInstallMode then
    Result := MachineEnvKey
  else
    Result := UserEnvKey;
end;

function ContainsPathEntry(const PathList, Entry: string): Boolean;
begin
  Result := Pos(';' + Uppercase(Entry) + ';', ';' + Uppercase(PathList) + ';') > 0;
end;

function AddPathEntry(const PathList, Entry: string): string;
begin
  if PathList = '' then
    Result := Entry
  else if PathList[Length(PathList)] = ';' then
    Result := PathList + Entry
  else
    Result := PathList + ';' + Entry;
end;

procedure AddToPath(const RootKey: Integer; const KeyName, Entry: string);
var
  Paths: string;
begin
  Entry := CleanDir(Entry);
  if not RegQueryStringValue(RootKey, KeyName, 'Path', Paths) then
    Paths := '';

  if not ContainsPathEntry(Paths, Entry) then
  begin
    Paths := AddPathEntry(Paths, Entry);
    RegWriteStringValue(RootKey, KeyName, 'Path', Paths);
  end;
end;

procedure RemoveFromPath(const RootKey: Integer; const KeyName, Entry: string);
var
  Paths: string;
  NeedleUpper: string;
  PathsUpper: string;
  PosStart: Integer;
begin
  Entry := CleanDir(Entry);
  if not RegQueryStringValue(RootKey, KeyName, 'Path', Paths) then
    Exit;

  NeedleUpper := ';' + Uppercase(Entry) + ';';
  Paths := ';' + Paths + ';';
  PathsUpper := Uppercase(Paths);
  PosStart := Pos(NeedleUpper, PathsUpper);

  while PosStart > 0 do
  begin
    Delete(Paths, PosStart, Length(Entry) + 1);
    PathsUpper := Uppercase(Paths);
    PosStart := Pos(NeedleUpper, PathsUpper);
  end;

  while (Length(Paths) > 0) and (Paths[1] = ';') do
    Delete(Paths, 1, 1);
  while (Length(Paths) > 0) and (Paths[Length(Paths)] = ';') do
    Delete(Paths, Length(Paths), 1);

  RegWriteStringValue(RootKey, KeyName, 'Path', Paths);
end;

procedure RemoveLegacyPathEntries;
begin
  if LegacyInstallDir <> '' then
  begin
    RemoveFromPath(HKEY_CURRENT_USER, UserEnvKey, LegacyInstallDir + '\bin');
    RemoveFromPath(HKEY_LOCAL_MACHINE, MachineEnvKey, LegacyInstallDir + '\bin');
  end;

  RemoveFromPath(HKEY_CURRENT_USER, UserEnvKey, ExpandConstant('{userpf}\Methlang\bin'));
  RemoveFromPath(HKEY_LOCAL_MACHINE, MachineEnvKey, ExpandConstant('{commonpf}\Methlang\bin'));
end;

procedure RemoveLegacyStartMenuFolder(const ProgramsDir: string);
var
  LegacyGroupDir: string;
begin
  LegacyGroupDir := AddBackslash(ProgramsDir) + LegacyStartMenuName;
  if DirExists(LegacyGroupDir) then
    DelTree(LegacyGroupDir, True, True, True);
end;

procedure RemoveLegacyFileAssociation;
var
  FileClass: string;
  OpenCommand: string;
begin
  if RegQueryStringValue(HKEY_CURRENT_USER, 'Software\Classes\.mettle', '', FileClass) and
     (FileClass = LegacyFileClass) and
     RegQueryStringValue(HKEY_CURRENT_USER, 'Software\Classes\' + LegacyFileClass + '\shell\open\command', '', OpenCommand) and
     IsLegacyDir(OpenCommand) then
  begin
    RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, 'Software\Classes\' + LegacyFileClass);
    RegDeleteKeyIncludingSubkeys(HKEY_CURRENT_USER, 'Software\Classes\.mettle');
  end;

  if RegQueryStringValue(HKEY_LOCAL_MACHINE, 'Software\Classes\.mettle', '', FileClass) and
     (FileClass = LegacyFileClass) and
     RegQueryStringValue(HKEY_LOCAL_MACHINE, 'Software\Classes\' + LegacyFileClass + '\shell\open\command', '', OpenCommand) and
     IsLegacyDir(OpenCommand) then
  begin
    RegDeleteKeyIncludingSubkeys(HKEY_LOCAL_MACHINE, 'Software\Classes\' + LegacyFileClass);
    RegDeleteKeyIncludingSubkeys(HKEY_LOCAL_MACHINE, 'Software\Classes\.mettle');
  end;
end;

procedure CleanupLegacyInstallState;
begin
  RemoveLegacyPathEntries;
  RemoveLegacyStartMenuFolder(ExpandConstant('{userprograms}'));
  RemoveLegacyStartMenuFolder(ExpandConstant('{commonprograms}'));
  RemoveLegacyFileAssociation;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  BinDir: string;
begin
  if CurStep <> ssPostInstall then
    Exit;

  BinDir := ExpandConstant('{app}\bin');
  if WizardIsTaskSelected('addtopath') then
    AddToPath(PathRootKey, PathKeyName, BinDir);

  CleanupLegacyInstallState;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  BinDir: string;
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;

  BinDir := ExpandConstant('{app}\bin');
  { Clean both scopes; this also covers installs that changed scope on upgrade. }
  RemoveFromPath(HKEY_CURRENT_USER, UserEnvKey, BinDir);
  RemoveFromPath(HKEY_LOCAL_MACHINE, MachineEnvKey, BinDir);
end;
