; VCam Installer Script (Inno Setup)
; Version: 0.0.2

[Setup]
AppName=VCam Virtual Camera
AppVersion=0.0.2
AppPublisher=VCam Project
DefaultDirName={autopf}\VCam
DefaultGroupName=VCam
OutputDir=.
OutputBaseFilename=VCamSetup-0.0.2-x64
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
RestartApplications=no
UsedUserAreasWarning=no

[Files]
Source: "build\x64\Release\MediaSource.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\x64\Release\VCamVideoStreamProducer.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "src\VCamSettingsUi\bin\Release\net10.0-windows\win-x64\publish\VCamSettingsUi.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\x64\Release\VCamPreview.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\x64\Release\Registrar.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\x64\Release\VCamProducerCli.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\x64\Release\CaptureTest.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "vcam_restart_host.ps1"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
; Start menu (current user, mirrors HKCU autostart semantics)
; Один ярлык — только основной процесс (хост). Настройки/Предпросмотр живут в tray-меню хоста,
; перезапуск — кнопка в VCamSettingsUi; дубли в меню Пуск отталкивают (решение пользователя).
Name: "{userprograms}\VCam\Запуск камеры VCam"; Filename: "{app}\VCamVideoStreamProducer.exe"; WorkingDir: "{app}"

[Registry]
; Autostart Tray Host for current user
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "VCamAutostart"; ValueData: """{app}\VCamVideoStreamProducer.exe"""; Flags: uninsdeletevalue
; Autostart Camera Holder (Registrar hold) for current user (hidden powershell wrapper)
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "VCamRegistrar"; ValueData: "powershell.exe -WindowStyle Hidden -Command ""Start-Process -FilePath '{app}\Registrar.exe' -ArgumentList 'add','VCam','hold' -WindowStyle Hidden"""; Flags: uninsdeletevalue

[Code]
function InitializeSetup(): Boolean;
var
  ResultCode: Integer;
begin
  Result := True;
  // Stop FrameServer service before update to allow DLL replacement
  Exec('sc.exe', 'stop FrameServer', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  // Give it a second to release the DLL
  Sleep(1500);
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep = ssPostInstall then
  begin
    // Register COM MediaSource.dll
    Exec('regsvr32.exe', '/s "' + ExpandConstant('{app}\MediaSource.dll') + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    
    // Start FrameServer service back up
    Exec('sc.exe', 'start FrameServer', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    
    // Immediately launch Registrar hold so camera is active right now
    Exec('powershell.exe', '-WindowStyle Hidden -Command "Start-Process -FilePath ''' + ExpandConstant('{app}\Registrar.exe') + ''' -ArgumentList ''add'',''VCam'',''hold'' -WindowStyle Hidden"', '', SW_HIDE, ewNoWait, ResultCode);
    
    // Launch tray host
    Exec(ExpandConstant('{app}\VCamVideoStreamProducer.exe'), '', '', SW_SHOW, ewNoWait, ResultCode);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  if CurUninstallStep = usUninstall then
  begin
    // Kill VCam processes if running
    Exec('taskkill.exe', '/f /im VCamVideoStreamProducer.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec('taskkill.exe', '/f /im VCamPreview.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec('taskkill.exe', '/f /im VCamSettingsUi.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Exec('taskkill.exe', '/f /im Registrar.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    
    // Stop FrameServer temporarily to unregister DLL
    Exec('sc.exe', 'stop FrameServer', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(1000);
    
    // Unregister COM DLL
    Exec('regsvr32.exe', '/u /s "' + ExpandConstant('{app}\MediaSource.dll') + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    
    // Restart FrameServer so system camera subsystem stays healthy
    Exec('sc.exe', 'start FrameServer', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  end;
end;
