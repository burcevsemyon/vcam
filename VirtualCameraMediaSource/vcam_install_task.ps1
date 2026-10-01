# Создаёт задачу автозапуска Task Scheduler\VCamHost.
# Запускается инсталлятором (admin) из ssPostInstall. EXE лежит рядом с этим
# скриптом в {app}. Результат -> %LOCALAPPDATA%\VCam\setup_task.log.
# ЧЕРЕЗ COM Schedule.Service, а не schtasks: у /TR с пробелом (Program Files)
# и powershell-обвязки терялись кавычки и путь рвался на "C:\Program".
$ErrorActionPreference = 'Stop'
$log = Join-Path $env:LOCALAPPDATA 'VCam\setup_task.log'
try {
    $exe = Join-Path $PSScriptRoot 'VCamVideoStreamProducer.exe'
    $svc = New-Object -ComObject Schedule.Service
    $svc.Connect()
    $def = $svc.NewTask(0)
    $def.RegistrationInfo.Description = 'VCam tray host autostart (full token at logon)'
    # 9 = TASK_TRIGGER_LOGON
    $def.Triggers.Create(9) | Out-Null
    $def.Settings.Enabled = $true
    # иначе на ноуте с батареей задача не стартует после входа
    $def.Settings.DisallowStartIfOnBatteries = $false
    $def.Settings.StopIfGoingOnBatteries = $false
    # 0 = TASK_ACTION_EXEC
    $action = $def.Actions.Create(0)
    $action.Path = $exe
    $action.WorkingDirectory = Split-Path $exe
    $def.Principal.UserId = "$env:USERDOMAIN\$env:USERNAME"
    $def.Principal.LogonType = 3   # TASK_LOGON_INTERACTIVE_TOKEN
    $def.Principal.RunLevel = 1    # TASK_RUNLEVEL_HIGHEST
    # 6 = TASK_CREATE_OR_UPDATE, 3 = TASK_LOGON_INTERACTIVE_TOKEN.
    # XmlText, а не $def: PowerShell ломает overload RegisterTask(ITaskDefinition)
    # (HRC 0x8004131A); строка XML доходит до проверки прав как есть.
    $registered = $svc.GetFolder('\').RegisterTask('VCamHost', $def.XmlText, 6, $null, $null, 3)
    "created: $($registered.Path); run: $($action.Path)" | Set-Content -Path $log -Encoding UTF8
    exit 0
} catch {
    $_ | Out-File -FilePath $log -Encoding UTF8
    exit 1
}
