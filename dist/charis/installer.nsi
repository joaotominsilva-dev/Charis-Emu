; SPDX-FileCopyrightText: 2026 Charis
; SPDX-License-Identifier: GPL-3.0-or-later
;
; Charis Switch installer. Installs per user (no admin) and can bring over the settings, saves,
; keys and firmware of an existing PokeDinner / Eglantine / Eden folder on this PC.
;
;   makensis -DPRODUCT_VERSION=1.1.0 -DBINARY_SOURCE_DIR=<folder with CharisSwitch.exe> installer.nsi

!ifndef PRODUCT_VERSION
  !error "PRODUCT_VERSION must be defined"
!endif

!ifndef BINARY_SOURCE_DIR
  !error "BINARY_SOURCE_DIR must be defined"
!endif

Unicode true
ManifestDPIAware true
RequestExecutionLevel user

!define PRODUCT_NAME "Charis Switch"
!define PRODUCT_EXE "CharisSwitch.exe"
!define PRODUCT_PUBLISHER "Charis"
!define PRODUCT_UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${PRODUCT_NAME}"

Name "${PRODUCT_NAME}"
OutFile "CharisSwitch-${PRODUCT_VERSION}-instalador.exe"
SetCompressor /SOLID lzma
InstallDir "$LOCALAPPDATA\Programs\${PRODUCT_NAME}"
ShowInstDetails show
ShowUnInstDetails show
BrandingText "${PRODUCT_NAME} ${PRODUCT_VERSION}"

!include "MUI2.nsh"
!include "nsDialogs.nsh"
!include "LogicLib.nsh"
!include "FileFunc.nsh"

!define MUI_ICON "charis.ico"
!define MUI_UNICON "charis.ico"
!define MUI_ABORTWARNING

Var ImportPage
Var ImportDirInput
Var ImportCheckbox
Var ImportDir
Var DoImport
Var FoundUserDir
Var ShortcutCheckbox
Var DesktopShortcut

!define MUI_WELCOMEPAGE_TITLE "Instalar o ${PRODUCT_NAME}"
!define MUI_WELCOMEPAGE_TEXT "O ${PRODUCT_NAME} é o emulador baseado no PokeDinner com o visual da Charis.$\r$\n$\r$\nNa próxima tela você pode trazer as configurações, os saves e os jogos que já usa no PokeDinner."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "..\..\LICENSE.txt"
Page custom ImportPageCreate ImportPageLeave
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_RUN "$INSTDIR\${PRODUCT_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "Abrir o ${PRODUCT_NAME} agora"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "PortugueseBR"

Function .onInit
  StrCpy $DoImport 1
  StrCpy $DesktopShortcut 1
  StrCpy $ImportDir "$DESKTOP\Pokemon"
FunctionEnd

; ---------- Import page ----------

Function ImportPageCreate
  !insertmacro MUI_HEADER_TEXT "Trazer do PokeDinner" "Configurações, saves, chaves e firmware"
  nsDialogs::Create 1018
  Pop $ImportPage
  ${If} $ImportPage == error
    Abort
  ${EndIf}

  ${NSD_CreateLabel} 0u 0u 100% 30u "Escolha a pasta onde está o PokeDinner que você usa hoje. O instalador copia a pasta de dados dele (a pasta $\"user$\") para o ${PRODUCT_NAME}. O PokeDinner continua do jeito que está."
  Pop $0

  ${NSD_CreateCheckbox} 0u 34u 100% 12u "Trazer minhas configurações e saves do PokeDinner"
  Pop $ImportCheckbox
  ${NSD_SetState} $ImportCheckbox $DoImport

  ${NSD_CreateDirRequest} 0u 52u 78% 13u "$ImportDir"
  Pop $ImportDirInput
  ${NSD_CreateBrowseButton} 80% 51u 20% 15u "Procurar..."
  Pop $0
  ${NSD_OnClick} $0 ImportBrowse

  ${NSD_CreateLabel} 0u 74u 100% 24u "Os jogos não são copiados: o ${PRODUCT_NAME} continua lendo eles da mesma pasta, como o PokeDinner faz."
  Pop $0

  ${NSD_CreateCheckbox} 0u 104u 100% 12u "Criar atalho na Área de Trabalho"
  Pop $ShortcutCheckbox
  ${NSD_SetState} $ShortcutCheckbox $DesktopShortcut

  nsDialogs::Show
FunctionEnd

Function ImportBrowse
  ${NSD_GetText} $ImportDirInput $0
  nsDialogs::SelectFolderDialog "Pasta do PokeDinner" "$0"
  Pop $0
  ${If} $0 != error
    ${NSD_SetText} $ImportDirInput "$0"
  ${EndIf}
FunctionEnd

Function ImportPageLeave
  ${NSD_GetState} $ImportCheckbox $DoImport
  ${NSD_GetState} $ShortcutCheckbox $DesktopShortcut
  ${NSD_GetText} $ImportDirInput $ImportDir
  ${If} $DoImport == ${BST_CHECKED}
    Call FindUserDir
    ${If} $FoundUserDir == ""
    ${AndIfNot} ${FileExists} "$APPDATA\eden\config\*.*"
      MessageBox MB_YESNO|MB_ICONEXCLAMATION "Não achei os dados do PokeDinner nessa pasta.$\r$\n$\r$\nContinuar sem trazer as configurações e saves?" IDYES +2
      Abort
    ${EndIf}
  ${EndIf}
FunctionEnd

; A data folder is a "user" folder with "config" or "nand" inside. Looks in the chosen folder
; and up to two levels below it, since the emulator usually sits in its own subfolder.
!macro CheckUserDir path
  ${If} $FoundUserDir == ""
  ${AndIf} ${FileExists} "${path}\user\config\*.*"
    StrCpy $FoundUserDir "${path}\user"
  ${EndIf}
  ${If} $FoundUserDir == ""
  ${AndIf} ${FileExists} "${path}\user\nand\*.*"
    StrCpy $FoundUserDir "${path}\user"
  ${EndIf}
!macroend

Function FindUserDir
  StrCpy $FoundUserDir ""
  !insertmacro CheckUserDir "$ImportDir"

  FindFirst $R0 $R1 "$ImportDir\*.*"
  ${DoWhile} $R1 != ""
    ${IfThen} $FoundUserDir != "" ${|} ${ExitDo} ${|}
    ${If} $R1 != "."
    ${AndIf} $R1 != ".."
    ${AndIf} ${FileExists} "$ImportDir\$R1\*.*"
      !insertmacro CheckUserDir "$ImportDir\$R1"
      ${If} $FoundUserDir == ""
        FindFirst $R2 $R3 "$ImportDir\$R1\*.*"
        ${DoWhile} $R3 != ""
          ${IfThen} $FoundUserDir != "" ${|} ${ExitDo} ${|}
          ${If} $R3 != "."
          ${AndIf} $R3 != ".."
          ${AndIf} ${FileExists} "$ImportDir\$R1\$R3\*.*"
            !insertmacro CheckUserDir "$ImportDir\$R1\$R3"
          ${EndIf}
          FindNext $R2 $R3
        ${Loop}
        FindClose $R2
      ${EndIf}
    ${EndIf}
    FindNext $R0 $R1
  ${Loop}
  FindClose $R0
FunctionEnd

; ---------- Install ----------

Section "Charis Switch" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"

  ; An update started from inside the program: wait until it has closed and its exe can be
  ; replaced (up to about 30 seconds).
  StrCpy $R8 0
  ${Do}
    ${IfNot} ${FileExists} "$INSTDIR\${PRODUCT_EXE}"
      ${Break}
    ${EndIf}
    ClearErrors
    FileOpen $R9 "$INSTDIR\${PRODUCT_EXE}" a
    ${IfNot} ${Errors}
      FileClose $R9
      ${Break}
    ${EndIf}
    Sleep 500
    IntOp $R8 $R8 + 1
  ${LoopUntil} $R8 >= 60
  Delete "$INSTDIR\eglantine.exe"
  File /r "${BINARY_SOURCE_DIR}\*"
  File "charis.ico"

  ${If} $DoImport == ${BST_CHECKED}
    ${If} ${FileExists} "$INSTDIR\user\config\*.*"
      DetailPrint "O ${PRODUCT_NAME} já tem dados nesta pasta; nada foi substituído."
    ${ElseIf} $FoundUserDir != ""
      DetailPrint "Copiando configurações e saves de $FoundUserDir ..."
      CreateDirectory "$INSTDIR\user"
      CopyFiles "$FoundUserDir\*.*" "$INSTDIR\user"
      DetailPrint "Pronto: dados do PokeDinner copiados."
    ${ElseIf} ${FileExists} "$APPDATA\eden\config\*.*"
      ; Without a "user" folder next to it, the program reads %APPDATA%\eden, the same place
      ; PokeDinner uses in this mode, so the settings and saves are already shared.
      DetailPrint "Os dados do PokeDinner estão em $APPDATA\eden e serão usados direto."
    ${EndIf}
  ${EndIf}

  CreateShortCut "$SMPROGRAMS\${PRODUCT_NAME}.lnk" "$INSTDIR\${PRODUCT_EXE}" "" "$INSTDIR\charis.ico"
  ${If} $DesktopShortcut == ${BST_CHECKED}
    CreateShortCut "$DESKTOP\${PRODUCT_NAME}.lnk" "$INSTDIR\${PRODUCT_EXE}" "" "$INSTDIR\charis.ico"
  ${EndIf}
SectionEnd

Section -Post
  WriteUninstaller "$INSTDIR\desinstalar.exe"

  ; Reopen the program after an update started from inside it
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/RELAUNCH" $R1
  ${IfNot} ${Errors}
    Exec '"$INSTDIR\${PRODUCT_EXE}"'
  ${EndIf}
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "DisplayName" "${PRODUCT_NAME}"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "DisplayVersion" "${PRODUCT_VERSION}"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "UninstallString" "$INSTDIR\desinstalar.exe"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "DisplayIcon" "$INSTDIR\charis.ico"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "Publisher" "${PRODUCT_PUBLISHER}"
  WriteRegStr HKCU "${PRODUCT_UNINST_KEY}" "InstallLocation" "$INSTDIR"
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${PRODUCT_UNINST_KEY}" "EstimatedSize" "$0"
SectionEnd

Section Uninstall
  Delete "$DESKTOP\${PRODUCT_NAME}.lnk"
  Delete "$SMPROGRAMS\${PRODUCT_NAME}.lnk"

  ; Program files go; the "user" folder (settings and saves) stays unless the user agrees.
  MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "Apagar também as configurações e os saves do ${PRODUCT_NAME}?$\r$\n$\r$\n(A pasta do PokeDinner não é tocada.)" IDNO keep_user
    RMDir /r "$INSTDIR\user"
  keep_user:

  Push $INSTDIR
  Call un.RemoveProgramFiles
  Delete "$INSTDIR\desinstalar.exe"
  RMDir "$INSTDIR"

  DeleteRegKey HKCU "${PRODUCT_UNINST_KEY}"
  SetAutoClose true
SectionEnd

; Removes everything in the install folder except the "user" data folder.
Function un.RemoveProgramFiles
  Pop $R9
  FindFirst $R0 $R1 "$R9\*.*"
  ${DoWhile} $R1 != ""
    ${If} $R1 != "."
    ${AndIf} $R1 != ".."
    ${AndIf} $R1 != "user"
    ${AndIf} $R1 != "desinstalar.exe"
      ${If} ${FileExists} "$R9\$R1\*.*"
        RMDir /r "$R9\$R1"
      ${Else}
        Delete "$R9\$R1"
      ${EndIf}
    ${EndIf}
    FindNext $R0 $R1
  ${Loop}
  FindClose $R0
FunctionEnd
