@echo off
setlocal
rem Remove the VCam virtual camera and unregister the DLL.
rem Requires: administrator privileges.

set "ROOT=%~dp0"
set "DLL=%ROOT%build\x64\Release\MediaSource.dll"
set "REGISTRAR=%ROOT%build\x64\Release\Registrar.exe"

if exist "%REGISTRAR%" (
    echo Removing virtual camera...
    "%REGISTRAR%" remove VCam
    if errorlevel 1 (
        echo Warning: failed to remove virtual camera.
    ) else (
        echo Virtual camera removed.
    )
)

if exist "%DLL%" (
    echo Unregistering DLL...
    regsvr32 /su "%DLL%"
    if errorlevel 1 (
        echo regsvr32 /u failed.
    ) else (
        echo DLL unregistered.
    )
) else (
    echo DLL not found: %DLL%
)

echo Done.
endlocal
