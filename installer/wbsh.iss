; wbsh installer -- Inno Setup 6.
; Per-user install, no UAC. Adds wbsh to the user PATH and registers an
; "Open wbsh here" Explorer context menu on folders and folder backgrounds.
; Optional: bash.exe / sh.exe shims, SHELL, a Windows Terminal profile and
; MinGit through winget.
; Drives the build via installer\build.ps1, which stages payload into stage\
; (x64) or stage-<arch>\ and passes /DArch=<arch> for any other architecture.

#ifndef AppVersion
#define AppVersion "1.1.2"
#endif

#ifndef Arch
#define Arch "x64"
#endif

#ifndef StageDir
#if Arch == "x64"
#define StageDir "stage"
#else
#define StageDir "stage-" + Arch
#endif
#endif

#if Arch == "x64"
#define ArchAllowed "x64compatible"
#else
#define ArchAllowed Arch
#endif

[Setup]
; Stable per-product GUID -- do NOT reuse the .vcxproj GUID. Changing this
; would orphan previous installs in Add/Remove Programs. Shared by every
; architecture, so an ARM64 install replaces an x64 one in place.
AppId={{F4B7E8C2-3A5D-4E1F-8B92-7D6C5A4E9F12}
AppName=wbsh
AppVersion={#AppVersion}
AppPublisher=Tomas Trachta
LicenseFile=..\LICENSE
DefaultDirName={localappdata}\Programs\wbsh
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed={#ArchAllowed}
ArchitecturesInstallIn64BitMode={#ArchAllowed}
OutputDir=output
OutputBaseFilename=wbsh-setup-{#Arch}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=wbsh
UninstallDisplayIcon={app}\wbsh.exe
SetupIconFile=wbsh.ico
; Tells Inno to broadcast WM_SETTINGCHANGE after install so already-running
; processes (Explorer, taskbar) pick up the new PATH without a logout.
ChangesEnvironment=yes
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "addtopath";  Description: "Add wbsh to the user PATH (recommended)"; GroupDescription: "Integration:"
Name: "explorer";   Description: "Add ""Open wbsh here"" to the Explorer right-click menu"; GroupDescription: "Integration:"
Name: "wtprofile";  Description: "Add a wbsh profile to Windows Terminal"; GroupDescription: "Integration:"
; Copies named bash.exe and sh.exe, in a bin folder of their own that goes
; on PATH only with this task. As sh.exe wbsh starts in POSIX mode.
Name: "shims";      Description: "Install bash.exe and sh.exe shims (a bin folder added to the user PATH)"; GroupDescription: "Drop-in replacement for Git Bash:"; Flags: unchecked
Name: "setshell";   Description: "Set SHELL to wbsh for your user"; GroupDescription: "Drop-in replacement for Git Bash:"; Flags: unchecked
; Offered only when no git.exe is found. Git is not bundled: winget fetches
; MinGit, Git for Windows' minimal build, which keeps this installer small.
Name: "installgit"; Description: "No git.exe was found: install MinGit with winget (needs internet)"; GroupDescription: "Git:"; Flags: unchecked; Check: GitMissing

[Files]
Source: "{#StageDir}\wbsh.exe";           DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\wbsh-here.cmd";      DestDir: "{app}"; Flags: ignoreversion
; Visual C++ runtime DLLs -- staged app-local by build.ps1. skipifsourcedoesntexist
; lets the .iss compile cleanly when developers run ISCC standalone without
; pre-staging the redistributable.
Source: "{#StageDir}\wbshsdk.dll";        DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\msvcp140.dll";       DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#StageDir}\vcruntime140.dll";   DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#StageDir}\vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
; Microsoft's current ConPTY (MIT, see wbshterm\conpty), for script and unbuffer.
; Optional: without it they use the pseudoconsole built into Windows.
Source: "{#StageDir}\conpty.dll";         DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#StageDir}\OpenConsole.exe";    DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

; --- bash.exe / sh.exe shims. Windows finds an exe's DLLs beside the name
; it was started by, so each copy needs the runtime, SDK and ConPTY next
; to it as well. ---
Source: "{#StageDir}\wbsh.exe";           DestDir: "{app}\bin"; DestName: "bash.exe"; Tasks: shims; Flags: ignoreversion
Source: "{#StageDir}\wbsh.exe";           DestDir: "{app}\bin"; DestName: "sh.exe";   Tasks: shims; Flags: ignoreversion
Source: "{#StageDir}\wbshsdk.dll";        DestDir: "{app}\bin"; Tasks: shims; Flags: ignoreversion
Source: "{#StageDir}\msvcp140.dll";       DestDir: "{app}\bin"; Tasks: shims; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#StageDir}\vcruntime140.dll";   DestDir: "{app}\bin"; Tasks: shims; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#StageDir}\vcruntime140_1.dll"; DestDir: "{app}\bin"; Tasks: shims; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#StageDir}\conpty.dll";         DestDir: "{app}\bin"; Tasks: shims; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#StageDir}\OpenConsole.exe";    DestDir: "{app}\bin"; Tasks: shims; Flags: ignoreversion skipifsourcedoesntexist

; --- Windows Terminal reads profile fragments from here on its next start,
; both the Store and the unpackaged builds; settings.json is not touched. ---
Source: "windows-terminal\wbsh.json"; DestDir: "{localappdata}\Microsoft\Windows Terminal\Fragments\wbsh"; \
  Tasks: wtprofile; Flags: ignoreversion; AfterInstall: PointFragmentAtApp

[Dirs]
; Where a third-party util goes. Created empty so there is somewhere
; obvious to drop one; see sdk/README.md. The shims in bin\ read
; %APPDATA%\wbsh\plugins rather than this one.
Name: "{app}\plugins"

[Icons]
Name: "{userprograms}\wbsh"; Filename: "{app}\wbsh.exe"; WorkingDir: "%USERPROFILE%"

[Registry]
; --- PATH -- HKCU\Environment\Path append. {olddata} is the previous string. ---
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; \
  ValueData: "{olddata};{app}"; Check: NeedsAddPath('{app}'); Tasks: addtopath
; The user PATH comes after the system one, so WSL's System32\bash.exe, when
; present, still answers a bare `bash` in cmd and PowerShell; agents and
; editors take the shim's full path.
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; \
  ValueData: "{olddata};{app}\bin"; Check: NeedsAddPath('{app}\bin'); Tasks: shims

; --- SHELL, read by editors, make, cron and script when they need a shell.
; Removed on uninstall only while it still names this wbsh. ---
Root: HKCU; Subkey: "Environment"; ValueType: string; ValueName: "SHELL"; \
  ValueData: "{app}\wbsh.exe"; Tasks: setshell

; --- "Open wbsh here" -- folder background (right-click empty space in folder). ---
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\wbsh"; \
  ValueType: string; ValueName: ""; ValueData: "Open &wbsh here"; \
  Tasks: explorer; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\wbsh"; \
  ValueType: string; ValueName: "Icon"; ValueData: """{app}\wbsh.exe"""; \
  Tasks: explorer
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\wbsh\command"; \
  ValueType: string; ValueName: ""; ValueData: """{app}\wbsh-here.cmd"" ""%V"""; \
  Tasks: explorer; Flags: uninsdeletekey

; --- "Open wbsh here" -- folder itself (right-click on a folder icon). ---
Root: HKCU; Subkey: "Software\Classes\Directory\shell\wbsh"; \
  ValueType: string; ValueName: ""; ValueData: "Open &wbsh here"; \
  Tasks: explorer; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Directory\shell\wbsh"; \
  ValueType: string; ValueName: "Icon"; ValueData: """{app}\wbsh.exe"""; \
  Tasks: explorer
Root: HKCU; Subkey: "Software\Classes\Directory\shell\wbsh\command"; \
  ValueType: string; ValueName: ""; ValueData: """{app}\wbsh-here.cmd"" ""%1"""; \
  Tasks: explorer; Flags: uninsdeletekey

[UninstallDelete]
Type: dirifempty; Name: "{localappdata}\Microsoft\Windows Terminal\Fragments\wbsh"
Type: dirifempty; Name: "{app}\bin"

[Code]
const
  MinGitPackage = 'Git.MinGit';
  DefaultFragmentDir = '%LOCALAPPDATA%\\Programs\\wbsh';

function NeedsAddPath(Param: string): Boolean;
var
  OrigPath: string;
  Expanded: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath) then
  begin
    Result := True;
    exit;
  end;
  Expanded := ExpandConstant(Param);
  Result := Pos(';' + Lowercase(Expanded) + ';',
                ';' + Lowercase(OrigPath)  + ';') = 0;
end;

function GitForWindowsInstalled: Boolean;
var
  InstallPath: string;
begin
  Result := RegQueryStringValue(HKEY_LOCAL_MACHINE, 'SOFTWARE\GitForWindows', 'InstallPath', InstallPath)
         or RegQueryStringValue(HKEY_CURRENT_USER, 'SOFTWARE\GitForWindows', 'InstallPath', InstallPath);
end;

function GitMissing: Boolean;
begin
  Result := (FileSearch('git.exe', GetEnv('PATH')) = '') and not GitForWindowsInstalled;
end;

procedure InstallMinGit;
var
  ResultCode: Integer;
begin
  WizardForm.StatusLabel.Caption := 'Installing MinGit with winget...';
  if Exec('winget.exe', 'install --id ' + MinGitPackage + ' -e --silent --disable-interactivity'
          + ' --accept-package-agreements --accept-source-agreements',
          '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0) then
    exit;
  SuppressibleMsgBox('winget could not install MinGit. To use git from wbsh, run'
    + #13#10 + '    winget install --id ' + MinGitPackage + ' -e' + #13#10
    + 'or install Git for Windows from https://git-scm.com.', mbInformation, MB_OK, IDOK);
end;

{ JSON string body for a path: backslashes doubled, anything past ASCII
  as \uXXXX, so the fragment stays ASCII whatever the user's name. }
function JsonPath(Path: string): string;
var
  I: Integer;
begin
  Result := '';
  for I := 1 to Length(Path) do
    if Path[I] = '\' then
      Result := Result + '\\'
    else if Ord(Path[I]) > 127 then
      Result := Result + Format('\u%.4x', [Ord(Path[I])])
    else
      Result := Result + Path[I];
end;

{ The fragment names the default install folder; any other one is
  written into it here. }
procedure PointFragmentAtApp;
var
  Fragment: string;
  Raw: AnsiString;
  Text: string;
begin
  if CompareText(ExpandConstant('{app}'), ExpandConstant('{localappdata}\Programs\wbsh')) = 0 then
    exit;
  Fragment := ExpandConstant(CurrentFileName);
  if not LoadStringFromFile(Fragment, Raw) then
    exit;
  Text := String(Raw);
  StringChangeEx(Text, DefaultFragmentDir, JsonPath(ExpandConstant('{app}')), True);
  SaveStringToFile(Fragment, AnsiString(Text), False);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('installgit') then
    InstallMinGit;
end;

procedure RemoveFromPath(Path: string);
var
  CurPath: string;
  Wrapped: string;
  Lower:   string;
  Idx:     Integer;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', CurPath) then
    exit;
  Wrapped := ';' + CurPath + ';';
  Lower   := Lowercase(Wrapped);
  Idx := Pos(';' + Lowercase(Path) + ';', Lower);
  if Idx = 0 then exit;
  Delete(Wrapped, Idx, Length(Path) + 1);
  if (Length(Wrapped) > 0) and (Wrapped[1] = ';') then
    Delete(Wrapped, 1, 1);
  if (Length(Wrapped) > 0) and (Wrapped[Length(Wrapped)] = ';') then
    Delete(Wrapped, Length(Wrapped), 1);
  RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', Wrapped);
end;

procedure RemoveShellIfOurs;
var
  Shell: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'SHELL', Shell) then
    exit;
  if CompareText(Shell, ExpandConstant('{app}\wbsh.exe')) = 0 then
    RegDeleteValue(HKEY_CURRENT_USER, 'Environment', 'SHELL');
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep <> usPostUninstall then
    exit;
  RemoveFromPath(ExpandConstant('{app}'));
  RemoveFromPath(ExpandConstant('{app}\bin'));
  RemoveShellIfOurs;
end;
