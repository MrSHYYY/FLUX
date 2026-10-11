!include "WinMessages.nsh"
!include "x64.nsh"

; Version: pass it on the command line so it matches CMakeLists.txt, e.g.
;   makensis /DVERSION=1.5.0 installer\installer.nsi
; The self-updater downloads the release asset named  FLUX-<version>-Setup.exe
; and runs it silently:  FLUX-<version>-Setup.exe /S /D=<install dir>
!ifndef VERSION
  !define VERSION "1.5.0"
!endif

; General — Discord-style per-user zero-click setup (no UAC prompt, no wizard pages)
Name "FLUX"
Caption "FLUX Setup"
OutFile "..\dist\Release-v${VERSION}\FLUX-${VERSION}-Setup.exe"
Unicode True
InstallDir "$LOCALAPPDATA\FLUX"
InstallDirRegKey HKCU "Software\FLUX" "Install_Dir"
RequestExecutionLevel user
AutoCloseWindow true
ShowInstDetails nevershow
XPStyle on
BrandingText " "

; Branding and icons
Icon "..\resources\flux.ico"
UninstallIcon "..\resources\flux.ico"

; Single Discord-style splash install page (no Welcome / Directory / Finish wizard pages)
Page instfiles "" "" InstFilesShow

; Uninstaller Pages
UninstPage uninstConfirm
UninstPage instfiles

; Language
LoadLanguageFile "${NSISDIR}\Contrib\Language files\English.nlf"

; Version Info
VIProductVersion "${VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductName" "FLUX"
VIAddVersionKey /LANG=1033 "Comments" "Internal Media Streaming Client"
VIAddVersionKey /LANG=1033 "CompanyName" "FLUX"
VIAddVersionKey /LANG=1033 "LegalCopyright" "FLUX"
VIAddVersionKey /LANG=1033 "FileDescription" "FLUX Setup"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}.0"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}.0"

Function .onInit
    IfSilent done
    ReadRegStr $0 HKCU "Software\FLUX" "Install_Dir"
    StrCmp $0 "" done
    StrCpy $INSTDIR $0
    done:
FunctionEnd

; Transform the NSIS window into a compact 300x350 frameless dark Discord-style setup card
Function InstFilesShow
    IfSilent silent_ui

    ; Hide standard outer wizard buttons and chrome on $HWNDPARENT
    GetDlgItem $1 $HWNDPARENT 1
    ShowWindow $1 0
    GetDlgItem $1 $HWNDPARENT 2
    ShowWindow $1 0
    GetDlgItem $1 $HWNDPARENT 3
    ShowWindow $1 0
    GetDlgItem $1 $HWNDPARENT 1028
    ShowWindow $1 0
    GetDlgItem $1 $HWNDPARENT 1035
    ShowWindow $1 0
    GetDlgItem $1 $HWNDPARENT 1036
    ShowWindow $1 0
    GetDlgItem $1 $HWNDPARENT 1037
    ShowWindow $1 0
    GetDlgItem $1 $HWNDPARENT 1038
    ShowWindow $1 0
    GetDlgItem $1 $HWNDPARENT 1039
    ShowWindow $1 0
    GetDlgItem $1 $HWNDPARENT 1045
    ShowWindow $1 0
    GetDlgItem $1 $HWNDPARENT 1256
    ShowWindow $1 0

    ; Deep dark surface (#0A0A0D)
    SetCtlColors $HWNDPARENT 0xF5F5F7 0x0A0A0D
    FindWindow $0 "#32770" "" $HWNDPARENT
    SetCtlColors $0 0xA9A9B6 0x0A0A0D

    ; Center 300x350 popup on primary screen
    System::Call 'user32::GetSystemMetrics(i 0) i .r1'
    System::Call 'user32::GetSystemMetrics(i 1) i .r2'
    IntOp $3 $1 - 300
    IntOp $3 $3 / 2
    IntOp $4 $2 - 350
    IntOp $4 $4 / 2

    ; Frameless popup window style (WS_POPUP | WS_VISIBLE | WS_CLIPCHILDREN | WS_SYSMENU)
    System::Call 'user32::SetWindowLongW(p $HWNDPARENT, i -16, i 0x96080000)'
    System::Call 'user32::SetWindowLongW(p $HWNDPARENT, i -20, i 0x00040000)'

    ; Enable Windows 10/11 DWM Dark Mode (20=1) and Rounded Corners (33=2)
    System::Call '*(i 1) p .r5'
    System::Call 'dwmapi::DwmSetWindowAttribute(p $HWNDPARENT, i 20, p r5, i 4)'
    System::Free $5
    System::Call '*(i 2) p .r5'
    System::Call 'dwmapi::DwmSetWindowAttribute(p $HWNDPARENT, i 33, p r5, i 4)'
    System::Free $5

    System::Call 'user32::SetWindowPos(p $HWNDPARENT, p 0, i r3, i r4, i 300, i 350, i 0x0060)'

    ; Expand inner page container to fill the 300x350 card
    GetDlgItem $1 $HWNDPARENT 1018
    System::Call 'user32::SetWindowPos(p r1, p 0, i 0, i 0, i 300, i 350, i 0x0014)'
    System::Call 'user32::SetWindowPos(p r0, p 0, i 0, i 0, i 300, i 350, i 0x0014)'

    ; Hide details button (1027), details listview (1016), and default small icon (1031)
    GetDlgItem $1 $0 1027
    ShowWindow $1 0
    GetDlgItem $1 $0 1016
    ShowWindow $1 0
    GetDlgItem $1 $0 1031
    ShowWindow $1 0

    ; Centered 72x72 FLUX Icon (resource ID 103 in NSIS executable)
    System::Call 'kernel32::GetModuleHandleW(p 0) p .r2'
    System::Call 'user32::LoadImageW(p r2, i 103, i 1, i 72, i 72, i 0) p .r3'
    System::Call 'user32::CreateWindowExW(i 0, w "STATIC", w "", i 0x50000203, i 114, i 48, i 72, i 72, p r0, p 0, p r2, p 0) p .r4'
    SetCtlColors $4 0xF5F5F7 0x0A0A0D
    SendMessage $4 0x0172 1 $3

    ; Bold "FLUX" brand wordmark
    System::Call 'user32::CreateWindowExW(i 0, w "STATIC", w "FLUX", i 0x50000001, i 20, i 136, i 260, i 36, p r0, p 0, p r2, p 0) p .r4'
    CreateFont $5 "Segoe UI" 20 800
    SendMessage $4 ${WM_SETFONT} $5 1
    SetCtlColors $4 0xF5F5F7 0x0A0A0D

    ; Primary Discord-style status headline
    System::Call 'user32::CreateWindowExW(i 0, w "STATIC", w "INSTALLING FLUX...", i 0x50000001, i 20, i 192, i 260, i 20, p r0, p 0, p r2, p 0) p .r4'
    CreateFont $5 "Segoe UI" 9 700
    SendMessage $4 ${WM_SETFONT} $5 1
    SetCtlColors $4 0xE50914 0x0A0A0D

    ; Secondary status label (control 1006)
    GetDlgItem $1 $0 1006
    System::Call 'user32::SetWindowLongW(p r1, i -16, i 0x50000001)'
    System::Call 'user32::SetWindowPos(p r1, p 0, i 24, i 218, i 252, i 20, i 0x0014)'
    CreateFont $5 "Segoe UI" 8 400
    SendMessage $1 ${WM_SETFONT} $5 1
    SetCtlColors $1 0xA9A9B6 0x0A0A0D

    ; Sleek crimson progress bar (control 1004)
    GetDlgItem $1 $0 1004
    System::Call 'uxtheme::SetWindowTheme(p r1, w "", w "")'
    System::Call 'user32::SetWindowLongW(p r1, i -16, i 0x50000001)'
    System::Call 'user32::SetWindowLongW(p r1, i -20, i 0)'
    System::Call 'user32::SetWindowPos(p r1, p 0, i 28, i 286, i 244, i 6, i 0x0034)'
    SendMessage $1 0x2001 0 0x002E2525 ; PBM_SETBKCOLOR (#25252E -> BGR 0x2E2525)
    SendMessage $1 0x0409 0 0x001409E5 ; PBM_SETBARCOLOR (#E50914 -> BGR 0x1409E5)

    ; Version footer
    System::Call 'user32::CreateWindowExW(i 0, w "STATIC", w "v${VERSION}", i 0x50000001, i 20, i 310, i 260, i 18, p r0, p 0, p r2, p 0) p .r4'
    CreateFont $5 "Consolas" 8 400
    SendMessage $4 ${WM_SETFONT} $5 1
    SetCtlColors $4 0x6C6C7A 0x0A0A0D

    silent_ui:
FunctionEnd

Section "FLUX" SecMain
    SectionIn RO

    DetailPrint "Preparing FLUX v${VERSION}..."

    ; Close any running FLUX instance so files are unlocked
    nsExec::ExecToStack 'taskkill /F /IM FLUX.exe'

    SetOutPath "$INSTDIR"
    DetailPrint "Extracting application files..."
    File /r "..\dist\FLUX-${VERSION}\*.*"

    ; Write uninstaller
    DetailPrint "Registering application..."
    WriteUninstaller "$INSTDIR\Uninstall.exe"

    ; Per-user Registry keys (no admin elevation required)
    WriteRegStr HKCU "Software\FLUX" "Install_Dir" "$INSTDIR"

    ; Windows Installed Apps / Add or Remove Programs (per-user)
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FLUX" "DisplayName" "FLUX"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FLUX" "DisplayVersion" "${VERSION}"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FLUX" "Publisher" "FLUX"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FLUX" "DisplayIcon" "$INSTDIR\resources\flux.ico"
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FLUX" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FLUX" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FLUX" "InstallLocation" "$INSTDIR"
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FLUX" "NoModify" 1
    WriteRegDWORD HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FLUX" "NoRepair" 1

    ; Shortcuts
    CreateDirectory "$SMPROGRAMS\FLUX"
    CreateShortcut "$SMPROGRAMS\FLUX\FLUX.lnk" "$INSTDIR\FLUX.exe" "" "$INSTDIR\resources\flux.ico" 0
    CreateShortcut "$SMPROGRAMS\FLUX\Uninstall FLUX.lnk" "$INSTDIR\Uninstall.exe" "" "$INSTDIR\Uninstall.exe" 0
    CreateShortcut "$DESKTOP\FLUX.lnk" "$INSTDIR\FLUX.exe" "" "$INSTDIR\resources\flux.ico" 0

    DetailPrint "Starting FLUX..."
SectionEnd

; Automatically launch FLUX as soon as interactive setup finishes (just like DiscordSetup.exe)
Function .onInstSuccess
    IfSilent +2
    Exec '"$INSTDIR\FLUX.exe"'
FunctionEnd

Section "Uninstall"
    nsExec::ExecToStack 'taskkill /F /IM FLUX.exe'

    ; Remove Shortcuts
    Delete "$DESKTOP\FLUX.lnk"
    Delete "$SMPROGRAMS\FLUX\FLUX.lnk"
    Delete "$SMPROGRAMS\FLUX\Uninstall FLUX.lnk"
    RMDir "$SMPROGRAMS\FLUX"

    ; Remove Registry keys
    DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\FLUX"
    DeleteRegKey HKCU "Software\FLUX"

    ; Remove Files and Directories
    RMDir /r "$INSTDIR"
SectionEnd
