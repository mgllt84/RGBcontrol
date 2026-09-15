Unicode True
SetCompressor /SOLID lzma
SetCompressorDictSize 64

!include "MUI2.nsh"

!define APP_NAME "RGBCcontrol"
!define APP_VERSION "0.16.13"
!define APP_PUBLISHER "RGBCcontrol"
!define APP_EXE "RGBCcontrol.exe"

Name "${APP_NAME}"
OutFile "..\..\outputs\RGBCcontrol-CPP\RGBCcontrol-Setup.exe"
InstallDir "$LOCALAPPDATA\Programs\RGBCcontrol"
InstallDirRegKey HKCU "Software\RGBCcontrol" "InstallPath"
RequestExecutionLevel user
BrandingText "RGBCcontrol ${APP_VERSION} — application native C++"
Icon "..\resources\RGBCcontrol.ico"
UninstallIcon "..\resources\RGBCcontrol.ico"

!define MUI_ABORTWARNING
!define MUI_ICON "..\resources\RGBCcontrol.ico"
!define MUI_UNICON "..\resources\RGBCcontrol.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Lancer RGBCcontrol"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_UNPAGE_FINISH

!insertmacro MUI_LANGUAGE "French"

Function StopRunningComponents
  ; OpenRGB and the helper bridges are children of RGBCcontrol.  A silent
  ; update can otherwise reach the copy phase while one of their DLLs is
  ; still closing, which leaves OpenRGB.exe locked by Windows.
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /T /IM OpenRGB.exe'
  Pop $0
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /T /IM RGBCcontrol.HardwareBridge.exe'
  Pop $0
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /T /IM RGBCcontrol.DualSense.exe'
  Pop $0
  nsExec::ExecToLog '"$SYSDIR\taskkill.exe" /F /T /IM RGBCcontrol.GamepadBridge.exe'
  Pop $0
  Sleep 2000
FunctionEnd

Section "RGBCcontrol" SEC_MAIN
  Call StopRunningComponents
  IfSilent silent_update_wait normal_install
silent_update_wait:
  ; The running application closes immediately after spawning this updater.
  ; Waiting here prevents Windows from locking the executable being replaced.
  Sleep 1800
normal_install:
  SetOutPath "$INSTDIR"
  File /r /x "update-channel.ini" "..\dist\*.*"
  ; Always install the official channel. Older releases shipped an empty file,
  ; which otherwise prevented every future one-click update.
  File /oname=update-channel.ini "..\dist\update-channel.ini"

  WriteRegStr HKCU "Software\RGBCcontrol" "InstallPath" "$INSTDIR"
  WriteUninstaller "$INSTDIR\Desinstaller RGBCcontrol.exe"

  CreateDirectory "$SMPROGRAMS\RGBCcontrol"
  CreateShortcut "$SMPROGRAMS\RGBCcontrol\RGBCcontrol.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0
  CreateShortcut "$SMPROGRAMS\RGBCcontrol\Desinstaller RGBCcontrol.lnk" "$INSTDIR\Desinstaller RGBCcontrol.exe"
  CreateShortcut "$DESKTOP\RGBCcontrol.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${APP_EXE}" 0

  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RGBCcontrol" "DisplayName" "RGBCcontrol"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RGBCcontrol" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RGBCcontrol" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RGBCcontrol" "DisplayIcon" "$INSTDIR\${APP_EXE}"
  WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RGBCcontrol" "UninstallString" '"$INSTDIR\Desinstaller RGBCcontrol.exe"'
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RGBCcontrol" "NoModify" 1
  WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RGBCcontrol" "NoRepair" 1

  IfSilent silent_update_restart install_done
silent_update_restart:
  Exec '"$INSTDIR\${APP_EXE}" --updated'
install_done:
SectionEnd

Section "Uninstall"
  Delete "$DESKTOP\RGBCcontrol.lnk"
  RMDir /r "$SMPROGRAMS\RGBCcontrol"
  DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\RGBCcontrol"
  DeleteRegKey HKCU "Software\RGBCcontrol"
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "RGBCcontrol"
  RMDir /r "$INSTDIR"
SectionEnd
