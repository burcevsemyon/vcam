# VCam: graceful restart of the tray host (Stop-event -> wait -> start).
# Graceful first (the host closes the shm writer itself); force-kill only as
# a fallback. Settings and preview are left untouched.
$ErrorActionPreference = 'SilentlyContinue'

try {
    [System.Threading.EventWaitHandle]::OpenExisting('VCamVideoStreamProducer.Stop').Set()
} catch { }

for ($i = 0; $i -lt 20 -and (Get-Process -Name VCamVideoStreamProducer -ErrorAction SilentlyContinue); $i++) {
    Start-Sleep -Seconds 1
}

if (Get-Process -Name VCamVideoStreamProducer -ErrorAction SilentlyContinue) {
    Stop-Process -Name VCamVideoStreamProducer -Force
}

Start-Process -FilePath (Join-Path $PSScriptRoot 'VCamVideoStreamProducer.exe')
