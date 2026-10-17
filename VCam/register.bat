@echo off
setlocal
rem Register the VCam MediaSource DLL and start the virtual camera.
rem Requires: administrator privileges.

set "ROOT=%~dp0"
set "DLL=%ROOT%build\x64\Release\MediaSource.dll"
set "REGISTRAR=%ROOT%build\x64\Release\Registrar.exe"

if not exist "%DLL%" (
    echo DLL not found: %DLL%
    echo Build the solution first.
    exit /b 1
)

if not exist "%REGISTRAR%" (
    echo Registrar.exe not found: %REGISTRAR%
    echo Build the solution first.
    exit /b 1
)

echo Registering DLL...
regsvr32 /s "%DLL%"
if errorlevel 1 (
    echo regsvr32 failed.
    exit /b 1
)
echo DLL registered.

echo Starting virtual camera...
rem "hold" keeps Registrar.exe alive so the camera stays registered/alive
rem (see README: do not close the process).
"%REGISTRAR%" add VCam hold
if errorlevel 1 (
    echo Failed to start virtual camera.
    exit /b 1
)
echo Virtual camera started.

echo Done. Check Camera app or any UWP/MF consumer.
endlocal
