' VCam: launch host via the VCamHost scheduled task (RunLevel=Highest).
' Direct exe launch from a shortcut = non-elevated token, no SeCreateGlobalPrivilege
' -> FrameWriter falls back to Local\ section -> svchost FrameServer cannot see frames.
' WScript with window style 0: zero windows (powershell -WindowStyle Hidden still flashes conhost).
CreateObject("WScript.Shell").Run "schtasks /run /tn VCamHost", 0, False
