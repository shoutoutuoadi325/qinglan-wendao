; Compiler and language resources come from the locked official Inno Setup release.
#ifndef PayloadPath
  #error PayloadPath is required
#endif
#ifndef OutputPath
  #error OutputPath is required
#endif
#ifndef ReleaseVersion
  #error ReleaseVersion is required
#endif

#define AppName "Qinglan - Path of Ascension"
#define GameExe "QinglanAscension\Binaries\Win64\QinglanAscension-Win64-Shipping.exe"

[Setup]
; Keep this ID across future direct-download upgrades. Store identity is separate.
AppId={{028A5C95-3510-56CD-B9D9-29F884E79E3E}
AppName={#AppName}
AppVersion={#ReleaseVersion}
AppPublisher=Zhiqi Zhang
AppPublisherURL=https://github.com/shoutoutuoadi325/qinglan-wendao
AppSupportURL=https://github.com/shoutoutuoadi325/qinglan-wendao/issues
AppUpdatesURL=https://github.com/shoutoutuoadi325/qinglan-wendao/releases
DefaultDirName={autopf}\Qinglan
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\{#GameExe}
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequired=admin
WizardStyle=modern
OutputDir={#OutputPath}
OutputBaseFilename=Qinglan-{#ReleaseVersion}-Windows-x64-Setup
VersionInfoVersion={#ReleaseVersion}.0
VersionInfoProductName={#AppName}
Compression=lzma2/normal
SolidCompression=yes
SetupLogging=yes
CloseApplications=yes
RestartApplications=no
; Save data lives under LOCALAPPDATA, outside the installed file inventory.

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#PayloadPath}\*"; DestDir: "{app}"; Excludes: "Prerequisites\*"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#PayloadPath}\Prerequisites\vc_redist.x64.exe"; Flags: dontcopy

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\{#GameExe}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#GameExe}"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{group}\{cm:UninstallProgram,{#AppName}}"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\{#GameExe}"; WorkingDir: "{app}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
var
  RuntimeInstalled: Boolean;
  RuntimeNeedsRestart: Boolean;

function RuntimeInViewIsSufficient(RootKey: Integer; RequiredMS, RequiredLS: Cardinal): Boolean;
var
  Installed, Major, Minor, Build, Revision: Cardinal;
  InstalledMS, InstalledLS: Cardinal;
  Key: String;
begin
  Result := False;
  Key := 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64';
  if not RegQueryDWordValue(RootKey, Key, 'Installed', Installed) then Exit;
  if Installed <> 1 then Exit;
  if not RegQueryDWordValue(RootKey, Key, 'Major', Major) then Exit;
  if not RegQueryDWordValue(RootKey, Key, 'Minor', Minor) then Exit;
  if not RegQueryDWordValue(RootKey, Key, 'Bld', Build) then Exit;
  if not RegQueryDWordValue(RootKey, Key, 'Rbld', Revision) then Exit;
  InstalledMS := (Major shl 16) or Minor;
  InstalledLS := (Build shl 16) or Revision;
  Result := (InstalledMS > RequiredMS) or
    ((InstalledMS = RequiredMS) and (InstalledLS >= RequiredLS));
end;

function CurrentRuntimeIsSufficient(RuntimePath: String): Boolean;
var
  RequiredMS, RequiredLS: Cardinal;
begin
  Result := False;
  if not GetVersionNumbers(RuntimePath, RequiredMS, RequiredLS) then Exit;
  // Microsoft's bootstrapper has used both registry views for x64 runtimes.
  Result := RuntimeInViewIsSufficient(HKLM64, RequiredMS, RequiredLS) or
    RuntimeInViewIsSufficient(HKLM32, RequiredMS, RequiredLS);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  RuntimePath: String;
  ResultCode: Integer;
begin
  Result := '';
  if RuntimeInstalled then Exit;
  ExtractTemporaryFile('vc_redist.x64.exe');
  RuntimePath := ExpandConstant('{tmp}\vc_redist.x64.exe');
  if CurrentRuntimeIsSufficient(RuntimePath) then begin
    RuntimeInstalled := True;
    Exit;
  end;
  if not Exec(RuntimePath, '/install /quiet /norestart', '',
    SW_HIDE, ewWaitUntilTerminated, ResultCode) then begin
    Result := FmtMessage(SetupMessage(msgErrorExecutingProgram),
      ['Microsoft Visual C++ x64 Redistributable']) + #13#10 + SysErrorMessage(ResultCode);
    Exit;
  end;
  if (ResultCode <> 0) and (ResultCode <> 3010) then begin
    Result := FmtMessage(SetupMessage(msgErrorExecutingProgram),
      ['Microsoft Visual C++ x64 Redistributable']) + #13#10 +
      SysErrorMessage(ResultCode) + ' (' + IntToStr(ResultCode) + ')';
    Exit;
  end;
  RuntimeInstalled := True;
  // A successful 3010 does not reboot while files are being installed.
  // Inno asks about the reboot after completing the game installation.
  if ResultCode = 3010 then RuntimeNeedsRestart := True;
end;

function NeedRestart: Boolean;
begin
  Result := RuntimeNeedsRestart;
end;
