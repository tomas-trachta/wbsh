; wbshterm installer -- Inno Setup 6.
; Per-user install, no UAC. Ships wbsh.exe alongside the terminal: the
; terminal looks for the shell next to itself first, so an install of
; wbshterm is usable on its own without a separate wbsh install.
; Driven by installer\build.ps1, which stages payload into stage-wbshterm\.

#ifndef AppVersion
#define AppVersion "1.0.14"
#endif

[Setup]
; Stable per-product GUID -- distinct from wbsh's, so the two install and
; uninstall independently. Do not reuse or regenerate it.
AppId={{6E1C9D4A-2F83-4B77-9A16-58C0E7D3B441}
AppName=wbshterm
AppVersion={#AppVersion}
AppPublisher=Tomas Trachta
LicenseFile=..\LICENSE
DefaultDirName={localappdata}\Programs\wbshterm
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=output
OutputBaseFilename=wbshterm-setup-x64
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName=wbshterm
UninstallDisplayIcon={app}\wbshterm.exe
SetupIconFile=wbshterm.ico
ChangesEnvironment=yes
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Integration:"
Name: "addtopath";   Description: "Add wbshterm to the user PATH"; GroupDescription: "Integration:"; Flags: unchecked
Name: "explorer";    Description: "Add ""Open wbshterm here"" to the Explorer right-click menu"; GroupDescription: "Integration:"

[Files]
Source: "stage-wbshterm\wbshterm.exe";      DestDir: "{app}"; Flags: ignoreversion
; The shell the terminal hosts. Bundled so wbshterm works on its own; an
; existing wbsh install elsewhere is left alone.
Source: "stage-wbshterm\wbsh.exe";          DestDir: "{app}"; Flags: ignoreversion
Source: "stage-wbshterm\wbshterm-here.cmd"; DestDir: "{app}"; Flags: ignoreversion
; Visual C++ runtime DLLs, staged app-local by build.ps1.
Source: "stage-wbshterm\msvcp140.dll";      DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "stage-wbshterm\vcruntime140.dll";  DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "stage-wbshterm\vcruntime140_1.dll"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist

[Icons]
Name: "{userprograms}\wbshterm"; Filename: "{app}\wbshterm.exe"; WorkingDir: "%USERPROFILE%"
Name: "{userdesktop}\wbshterm";  Filename: "{app}\wbshterm.exe"; WorkingDir: "%USERPROFILE%"; Tasks: desktopicon

[Run]
Filename: "{app}\wbshterm.exe"; Description: "Open wbshterm now"; \
  WorkingDir: "%USERPROFILE%"; Flags: nowait postinstall skipifsilent

[Registry]
; --- PATH -- HKCU\Environment\Path append. {olddata} is the previous string. ---
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; \
  ValueData: "{olddata};{app}"; Check: NeedsAddPath('{app}'); Tasks: addtopath

; --- "Open wbshterm here" -- folder background (right-click empty space). ---
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\wbshterm"; \
  ValueType: string; ValueName: ""; ValueData: "Open wbsh&term here"; \
  Tasks: explorer; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\wbshterm"; \
  ValueType: string; ValueName: "Icon"; ValueData: """{app}\wbshterm.exe"""; \
  Tasks: explorer
Root: HKCU; Subkey: "Software\Classes\Directory\Background\shell\wbshterm\command"; \
  ValueType: string; ValueName: ""; ValueData: """{app}\wbshterm-here.cmd"" ""%V"""; \
  Tasks: explorer; Flags: uninsdeletekey

; --- "Open wbshterm here" -- folder itself (right-click on a folder icon). ---
Root: HKCU; Subkey: "Software\Classes\Directory\shell\wbshterm"; \
  ValueType: string; ValueName: ""; ValueData: "Open wbsh&term here"; \
  Tasks: explorer; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\Directory\shell\wbshterm"; \
  ValueType: string; ValueName: "Icon"; ValueData: """{app}\wbshterm.exe"""; \
  Tasks: explorer
Root: HKCU; Subkey: "Software\Classes\Directory\shell\wbshterm\command"; \
  ValueType: string; ValueName: ""; ValueData: """{app}\wbshterm-here.cmd"" ""%1"""; \
  Tasks: explorer; Flags: uninsdeletekey

[UninstallDelete]
; The settings file and any themes the user made are theirs: only the
; example this installer's build wrote is removed with the program.
Type: files; Name: "{userappdata}\wbshterm\themes\example.conf.txt"

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
