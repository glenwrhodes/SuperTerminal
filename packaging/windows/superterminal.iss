; SuperTerminal installer for Windows (Inno Setup 6).
;
; Built by build.ps1 -Installer and by CI:
;   ISCC.exe /DAppVersion=0.4.0 /DSourceDir=<folder with st.exe> /DOutputDir=<out> packaging\windows\superterminal.iss
;
; Installs for the current user by default (no admin prompt) into %LOCALAPPDATA%\Programs\SuperTerminal,
; or for all users into Program Files when chosen on the first page or with /ALLUSERS.
; Adds a Start menu shortcut (search "SuperTerminal", right-click for Run as administrator),
; puts st on the PATH and adds SuperTerminal to Windows Terminal's profile list.
; Silent install: SuperTerminal-Setup-<ver>-x64.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build\windows\Release"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\build\installer"
#endif

[Setup]
AppId={{6F1C2B7E-3D4A-4B8E-9C61-5A2E7D0F9B13}
AppName=SuperTerminal
AppVersion={#AppVersion}
AppVerName=SuperTerminal {#AppVersion}
AppPublisher=Glen Rhodes
AppPublisherURL=https://github.com/glenwrhodes/SuperTerminal
AppSupportURL=https://github.com/glenwrhodes/SuperTerminal/issues
AppUpdatesURL=https://github.com/glenwrhodes/SuperTerminal/releases
AppCopyright=Copyright (c) 2026 Glen Rhodes
VersionInfoVersion={#AppVersion}
VersionInfoProductName=SuperTerminal
VersionInfoDescription=SuperTerminal Setup
DefaultDirName={autopf}\SuperTerminal
DefaultGroupName=SuperTerminal
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog commandline
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
ChangesEnvironment=yes
CloseApplications=yes
SetupIconFile=st.ico
UninstallDisplayIcon={app}\st.exe
UninstallDisplayName=SuperTerminal
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
OutputDir={#OutputDir}
OutputBaseFilename=SuperTerminal-Setup-{#AppVersion}-x64

[Tasks]
Name: "path"; Description: "Add &st to the PATH (type st in any terminal)"
Name: "terminalprofile"; Description: "Add SuperTerminal to &Windows Terminal's profile list"
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked

[Files]
Source: "{#SourceDir}\st.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "st.ico"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\SuperTerminal"; Filename: "{app}\st.exe"; WorkingDir: "{%USERPROFILE}"; Comment: "PowerShell that understands plain English"
Name: "{autodesktop}\SuperTerminal"; Filename: "{app}\st.exe"; WorkingDir: "{%USERPROFILE}"; Comment: "PowerShell that understands plain English"; Tasks: desktopicon

[Run]
Filename: "{app}\st.exe"; Parameters: "--install-terminal-profile"; Flags: runhidden waituntilterminated; Tasks: terminalprofile; StatusMsg: "Adding SuperTerminal to Windows Terminal..."
Filename: "{app}\st.exe"; WorkingDir: "{%USERPROFILE}"; Description: "Open SuperTerminal now"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "{app}\st.exe"; Parameters: "--uninstall-terminal-profile"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveTerminalProfile"

[Code]
const
  UserEnvKey = 'Environment';
  SystemEnvKey = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';

function EnvRoot: Integer;
begin
  if IsAdminInstallMode then Result := HKEY_LOCAL_MACHINE else Result := HKEY_CURRENT_USER;
end;

function EnvKey: String;
begin
  if IsAdminInstallMode then Result := SystemEnvKey else Result := UserEnvKey;
end;

// True when Dir is one of the ;-separated entries of PathValue (case-insensitive, ignoring a trailing \).
function PathHas(PathValue, Dir: String): Boolean;
var
  Padded: String;
begin
  Padded := ';' + Lowercase(PathValue) + ';';
  Result := (Pos(';' + Lowercase(Dir) + ';', Padded) > 0) or (Pos(';' + Lowercase(Dir) + '\;', Padded) > 0);
end;

procedure AddToPath(Dir: String);
var
  Value: String;
begin
  if not RegQueryStringValue(EnvRoot, EnvKey, 'Path', Value) then Value := '';
  if PathHas(Value, Dir) then exit;
  if (Value <> '') and (Value[Length(Value)] <> ';') then Value := Value + ';';
  RegWriteExpandStringValue(EnvRoot, EnvKey, 'Path', Value + Dir);
end;

procedure RemoveFromPath(Dir: String);
var
  Value, Rest, Entry, Kept: String;
  P: Integer;
begin
  if not RegQueryStringValue(EnvRoot, EnvKey, 'Path', Value) then exit;
  if not PathHas(Value, Dir) then exit;
  Rest := Value + ';';
  Kept := '';
  while Rest <> '' do
  begin
    P := Pos(';', Rest);
    Entry := Copy(Rest, 1, P - 1);
    Delete(Rest, 1, P);
    if (Entry <> '') and (CompareText(Entry, Dir) <> 0) and (CompareText(Entry, Dir + '\') <> 0) then
    begin
      if Kept <> '' then Kept := Kept + ';';
      Kept := Kept + Entry;
    end;
  end;
  RegWriteExpandStringValue(EnvRoot, EnvKey, 'Path', Kept);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('path') then
    AddToPath(ExpandConstant('{app}'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveFromPath(ExpandConstant('{app}'));
end;
