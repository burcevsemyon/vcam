; VCam Installer Script (Inno Setup)
; Version: 0.0.3

[Setup]
AppName=VCam Virtual Camera
AppVersion=0.0.3
AppPublisher=VCam Project
DefaultDirName={autopf}\VCam
DefaultGroupName=VCam
OutputDir=.
OutputBaseFilename=VCamSetup-0.0.3-x64
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
RestartApplications=no
UsedUserAreasWarning=no
SetupLogging=yes

[Files]
Source: "build\x64\Release\MediaSource.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\x64\Release\VCamVideoStreamProducer.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "src\VCamSettingsUi\bin\Release\net10.0-windows\win-x64\publish\VCamSettingsUi.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\x64\Release\VCamPreview.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\x64\Release\Registrar.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\x64\Release\VCamProducerCli.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\x64\Release\CaptureTest.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "vcam_restart_host.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "vcam_install_task.ps1"; DestDir: "{app}"; Flags: ignoreversion
Source: "vcam_run_host.vbs"; DestDir: "{app}"; Flags: ignoreversion

[Dirs]
; Диаг-лог MediaSource пишется в %ProgramData%\VCam\msrc_diag.log из обоих
; контекстов (svchost FrameServer = LOCAL SERVICE и юзер-процессы). ACL выдаём
; в [Run]: наследованный от ProgramData даёт LOCAL SERVICE только чтение.
Name: "{commonappdata}\VCam"

[Run]
Filename: "{sys}\icacls.exe"; Parameters: """{commonappdata}\VCam"" /grant *S-1-5-19:(OI)(CI)M /grant *S-1-5-20:(OI)(CI)M /grant *S-1-5-32-545:(OI)(CI)M /T /C"; Flags: runhidden

[Icons]
; Start menu (current user, mirrors HKCU autostart semantics)
; Один ярлык — только основной процесс (хост). Настройки/Предпросмотр живут в tray-меню хоста,
; перезапуск — кнопка в VCamSettingsUi; дубли в меню Пуск отталкивают (решение пользователя).
; Ярлык идёт через задачу VCamHost (RunLevel=Highest), а НЕ напрямую на exe: прямой запуск
; из меню Пуск — не-elevated токен без SeCreateGlobalPrivilege → FrameWriter уходит в Local\-
; секцию, которую svchost FrameServer (session 0) не видит → device-режим без кадров.
; schtasks /run из не-elevated шела запускает Highest-задачу без UAC-промта (проверено).
; Обёртка — wscript+vcam_run_host.vbs (window style 0, окон НЕТ): powershell
; -WindowStyle Hidden всё равно мелькает conhost (окно создаётся до парсинга ключа).
Name: "{userprograms}\VCam\Запуск камеры VCam"; Filename: "{sys}\wscript.exe"; Parameters: """{app}\vcam_run_host.vbs"""; WorkingDir: "{app}"; IconFilename: "{app}\VCamVideoStreamProducer.exe"; IconIndex: 0

[Registry]
; Autostart: больше НЕ HKCU\Run — там всегда Limited-токен (UAC) и хост после
; ребута писал бы в Local-секцию, которую сервис MF не видит. Единственный
; механизм — задача Task Scheduler\VCamHost (создаётся ниже в ssPostInstall,
; /SC ONLOGON /RL HIGHEST = полный токен при входе).
; Camera Holder (HKCU\Run\VCamRegistrar, <=0.0.2) удалён: камеру поднимает и
; убирает сам хост (tray «Выход» → taskkill Registrar); см. vcam-camera-lifecycle.

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
    // Legacy cleanup: HKCU\Run\VCamAutostart (upgrades from <=0.0.2) would
    // double-start the host together with the new scheduled task.
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'VCamAutostart');
    // Legacy cleanup: HKCU\Run\VCamRegistrar (<=0.0.2) поднимал холдер без
    // хоста — камера оставалась в списке после «Выход». Хост владеет камерой.
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'VCamRegistrar');

    // Autostart task: ONLOGON + highest privileges = full token at logon
    // (Run key was always UAC-Limited -> host landed in Local\ section).
    // Через ps1 в {app}: прямой Exec('schtasks.exe', ...) из инсталлятора стабильно
    // возвращал 0x80004005, текст ошибки schtasks → %LOCALAPPDATA%\VCam\setup_task.log.
    if Exec('powershell.exe',
      '-NoProfile -ExecutionPolicy Bypass -File "' + ExpandConstant('{app}\vcam_install_task.ps1') + '"',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      Log('[autostart] task script rc=' + IntToStr(ResultCode))
    else
      Log('[autostart] task script exec failed');

    // Upgrade cleanup: shortcuts from the old 3-shortcut scheme (<=0.0.2 layouts)
    // survive an upgrade otherwise (Inno only keeps what the current [Icons] lists).
    DeleteFile(ExpandConstant('{userprograms}\VCam\Настройки VCam.lnk'));
    DeleteFile(ExpandConstant('{userprograms}\VCam\Предпросмотр VCam.lnk'));
    DeleteFile(ExpandConstant('{userprograms}\VCam\Перезапуск камеры VCam.lnk'));

    // Register COM MediaSource.dll
    Exec('regsvr32.exe', '/s "' + ExpandConstant('{app}\MediaSource.dll') + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    
    // Start FrameServer service back up
    Exec('sc.exe', 'start FrameServer', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    // Legacy cleanup: <=0.0.2 поднимал холдер элевированным (из инсталлятора),
    // и не-элевированный хост его не может убить (taskkill -> Access Denied).
    // Инсталлер сам элевирован — убиваем здесь, новый холдер поднимет хост.
    Exec('taskkill.exe', '/f /im Registrar.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);

    // Registrar hold больше НЕ запускается здесь: холдер поднимает сам хост
    // (StartCameraHolder) и убирает при tray «Выход».
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
    // Autostart task: powershell-посредник, как и создание (прямой schtasks-Exec
    // из инсталлятора отдавал 0x80004005); Unregister-ScheduledTask без кавычек-путей.
    Exec('powershell.exe', '-NoProfile -Command "Unregister-ScheduledTask -TaskName ''VCamHost'' -Confirm:$false"',
      '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'VCamAutostart');
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'VCamRegistrar');

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
