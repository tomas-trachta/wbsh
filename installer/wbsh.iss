; wbsh installer -- Inno Setup 6.
; Per-user install, no UAC. Adds wbsh to the user PATH and registers an
; "Open wbsh here" Explorer context menu on folders and folder backgrounds.
; Drives the build via installer\build.ps1, which stages payload into stage\.

#ifndef AppVersion
#define AppVersion "0.1.0"
#endif

[Setup]
; Stable per-product GUID -- do NOT reuse the .vcxproj GUID. Changing this
; would orphan previous installs in Add/Remove Programs.
AppId={{F4B7E8C2-3A5D-4E1F-8B92-7D6C5A4E9F12}
AppName=wbsh
AppVersion={#AppVersion}
AppPublisher=Tomas Trachta
LicenseFile=..\LICENSE
DefaultDirName={localappdata}\Programs\wbsh
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=output
OutputBaseFilename=wbsh-setup-x64
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
Name: "addtopath"; Description: "Add wbsh to the user PATH (recommended)"; GroupDescription: "Integration:"
Name: "explorer";  Description: "Add ""Open wbsh here"" to the Explorer right-click menu"; GroupDescription: "Integration:"

[Files]
Source: "stage\wbsh.exe";           DestDir: "{app}"; Flags: ignoreversion
Source: "stage\wbsh-here.cmd";      DestDir: "{app}"; Flags: ignoreversion
; Visual C++ runtime DLLs -- staged app-local by build.ps1. skipifsourcedoesntexist
; lets the .iss compile cleanly when developers run ISCC standalone without
; pre-staging the redistributable.
Source: "stage\msvcp140.dll";       DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "stage\vcruntime140.dll";   DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "stage\vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{userprograms}\wbsh"; Filename: "{app}\wbsh.exe"; WorkingDir: "%USERPROFILE%"

[Registry]
; --- PATH -- HKCU\Environment\Path append. {olddata} is the previous string. ---
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; \
  ValueData: "{olddata};{app}"; Check: NeedsAddPath('{app}'); Tasks: addtopath

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

[Code]
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

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveFromPath(ExpandConstant('{app}'));
end;
