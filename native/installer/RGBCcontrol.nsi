Unicode True
; A solid LZMA self-extractor has repeatedly triggered Defender's generic
; Wacatac machine-learning rule.  The payload is already made of compressed
; assets, so use the standard zlib stream: it remains reasonably small while
; producing a conventional installer layout that security scanners can inspect.
SetCompressor zlib

!include "MUI2.nsh"

!define APP_NAME "RGBCcontrol"
!define APP_VERSION "0.16.24"
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

Section "RGBCcontrol" SEC_MAIN
  IfSilent silent_update_wait normal_install
silent_update_wait:
  ; RGBCcontrol closes its own OpenRGB and helper child processes during
  ; shutdown.  Wait for that graceful cleanup instead of shipping process-
  ; killing commands inside the installer (a common antivirus heuristic).
  Sleep 2600
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
