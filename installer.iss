; Inno Setup script for MediaAccess
; This script is used by the GitHub Actions workflow to create an installer

#define MyAppName "MediaAccess"
#define MyAppPublisher "ReaperAccessible"
#define MyAppURL "https://reaperaccessible.fr"
#define MyAppExeName "MediaAccess.exe"

; Version is passed via command line: /DMyAppVersion=x.x.x
#ifndef MyAppVersion
  #define MyAppVersion "1.0.0"
#endif

; SourceDir is passed via command line: /DSourceDir=path
#ifndef SourceDir
  #define SourceDir "."
#endif

; OutputDir is passed via command line: /DOutputDir=path
; Defaults to "Output" subfolder if not specified
#ifndef OutputDir
  #define OutputDir "Output"
#endif

[Setup]
AppId={{18934CAA-C315-4A4C-96D1-8DEB433EF4D7}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
AllowNoIcons=yes
; Output installer filename
OutputBaseFilename=MediaAccessInstaller_{#MyAppVersion}
OutputDir={#OutputDir}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; v2.64 — we register shell verbs / file associations, so tell Windows to refresh
; the shell (SHChangeNotify SHCNE_ASSOCCHANGED) when Setup finishes.
ChangesAssociations=yes
; Require admin rights to install to Program Files
PrivilegesRequired=admin
; Allow installation for current user only as alternative
PrivilegesRequiredOverridesAllowed=dialog
; Architecture
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Uninstaller settings
SetupIconFile={#SourceDir}\MediaAccess.ico
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName}
; v2.29 — brand the installer EXE itself (file properties)
VersionInfoCompany={#MyAppPublisher}
VersionInfoProductName={#MyAppName}
VersionInfoVersion={#MyAppVersion}
VersionInfoDescription={#MyAppName} Setup

; ============================================================
; Auto-close running MediaAccess before file copy
; ------------------------------------------------------------
; Without this, upgrading while MediaAccess.exe is running silently
; FAILS to overwrite the running .exe (Windows file lock). The
; installer reports success, but on the next launch the user is
; still on the previous version — and the in-app update checker
; offers the same update again. Infinite loop.
;
; CloseApplications=yes makes Inno Setup use Restart Manager to
; gracefully close MediaAccess before copying files. The AppMutex
; lets Inno detect the running instance even when it doesn't hold a
; visible window (e.g. minimized to tray) — the name must match the
; one passed to CreateMutexW in src/main.cpp (currently
; "MediaAccessSingleInstance", defined as MUTEX_NAME in
; src/globals.cpp). Works in silent install mode too, so the in-app
; auto-update path is covered.
; ============================================================
CloseApplications=yes
RestartApplications=no
AppMutex=MediaAccessSingleInstance

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"

[CustomMessages]
english.AudioFileDesc=MediaAccess Audio File
english.VideoFileDesc=MediaAccess Video File
english.PlaylistDesc=MediaAccess Playlist
english.AppDesc=Accessible audio and video player with tempo, pitch and effects controls

french.AudioFileDesc=Fichier audio MediaAccess
french.VideoFileDesc=Fichier vidéo MediaAccess
french.PlaylistDesc=Liste de lecture MediaAccess
french.AppDesc=Lecteur audio et vidéo accessible avec contrôles de tempo, pitch et effets

; v2.41 — Explorer right-click verb labels (install language). v2.64: the verbs
; moved into a "MediaAccess" submenu and gained "&" mnemonics — in the legacy
; (full) context menu a screen-reader user can then do Shift+F10, M, then P or A.
; NOTE: these follow the language chosen when INSTALLING; switching the app's
; language in Options does not relabel the shell menu (re-run the installer).
english.MediaAccessSubmenu=&MediaAccess
english.PlayWithVerb=&Play with MediaAccess
english.EnqueueVerb=&Add to MediaAccess queue
english.ExplorerMenuTask=Add a MediaAccess submenu to the Windows Explorer right-click menu
french.MediaAccessSubmenu=&MediaAccess
french.PlayWithVerb=&Lire avec MediaAccess
french.EnqueueVerb=&Ajouter à la file MediaAccess
french.ExplorerMenuTask=Ajouter un sous-menu MediaAccess au menu contextuel de l'Explorateur Windows

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "quicklaunchicon"; Description: "{cm:CreateQuickLaunchIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked; OnlyBelowVersion: 6.1; Check: not IsAdminInstallMode
; v2.64 — checked by default so upgraders keep the context-menu entry they already
; have. Unchecking it on a re-run genuinely REMOVES the keys (see [Registry]).
Name: "explorermenu"; Description: "{cm:ExplorerMenuTask}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
; Install main executable
Source: "{#SourceDir}\MediaAccess.exe"; DestDir: "{app}"; Flags: ignoreversion
; Install config file if exists
Source: "{#SourceDir}\MediaAccess.ini"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
; Install lib folder with DLLs
Source: "{#SourceDir}\lib\*.dll"; DestDir: "{app}\lib"; Flags: ignoreversion
; Bundle yt-dlp.exe for YouTube support (auto-detected by the app)
Source: "{#SourceDir}\lib\yt-dlp.exe"; DestDir: "{app}\lib"; Flags: ignoreversion skipifsourcedoesntexist
; Bundle ffmpeg/ffprobe for YouTube format download + audio/video merge
; (GetFfmpegLocation auto-detects {app}\lib\ffmpeg.exe)
Source: "{#SourceDir}\lib\ffmpeg.exe"; DestDir: "{app}\lib"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#SourceDir}\lib\ffprobe.exe"; DestDir: "{app}\lib"; Flags: ignoreversion skipifsourcedoesntexist
; Bundle FluidR3_GM SoundFont so MIDI files sound great out of the box.
; ApplyMidiSettings() auto-loads this when no user SoundFont is set.
; skipifsourcedoesntexist so an installer can still be built without it.
Source: "{#SourceDir}\lib\FluidR3_GM.sf2"; DestDir: "{app}\lib"; Flags: ignoreversion skipifsourcedoesntexist
; Install documentation
Source: "{#SourceDir}\docs\readme.txt"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#SourceDir}\docs\changelog_fr.html"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#SourceDir}\docs\changelog_en.html"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#SourceDir}\docs\manual_fr.html"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#SourceDir}\docs\manual_en.html"; DestDir: "{app}\docs"; Flags: ignoreversion
; Install regional default keymaps (USA / FR-CA / FR-FR). The app also
; regenerates these on first run if missing, so this entry is safe to skip
; when building from a fresh tree before the binaries have run once.
Source: "{#SourceDir}\KeyMaps\*.MediaAccessKeyMap"; DestDir: "{app}\KeyMaps"; Flags: ignoreversion skipifsourcedoesntexist
; Create installed marker file
Source: "{#SourceDir}\MediaAccess.exe"; DestDir: "{app}"; AfterInstall: CreateInstalledMarker; Flags: ignoreversion

[Registry]
; ============================================================
; Register MediaAccess as a "Default App" candidate in Windows 10/11
; This makes MediaAccess appear in Settings > Apps > Default apps
; ============================================================

; Step 1: Register the application under HKLM\SOFTWARE\RegisteredApplications
Root: HKA; Subkey: "SOFTWARE\RegisteredApplications"; ValueType: string; ValueName: "MediaAccess"; ValueData: "SOFTWARE\MediaAccess\Capabilities"; Flags: uninsdeletevalue

; Step 2: Application capabilities (metadata)
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "MediaAccess"; Flags: uninsdeletekey
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "{cm:AppDesc}"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities"; ValueType: string; ValueName: "ApplicationIcon"; ValueData: """{app}\{#MyAppExeName}"",0"

; Step 3: ProgIDs for audio and video files
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.AudioFile"; ValueType: string; ValueName: ""; ValueData: "{cm:AudioFileDesc}"; Flags: uninsdeletekey
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.AudioFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"",0"
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.AudioFile\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "MediaAccess"
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.AudioFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.VideoFile"; ValueType: string; ValueName: ""; ValueData: "{cm:VideoFileDesc}"; Flags: uninsdeletekey
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.VideoFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"",0"
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.VideoFile\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "MediaAccess"
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.VideoFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.Playlist"; ValueType: string; ValueName: ""; ValueData: "{cm:PlaylistDesc}"; Flags: uninsdeletekey
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.Playlist\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"",0"
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.Playlist\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "MediaAccess"
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.Playlist\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

; Step 4: File extension associations (which ProgID handles which extension)
; Audio formats
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mp3";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mp2";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".wav";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ogg";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".aiff";  ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".flac";  ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".aac";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m4a";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m4b";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m4r";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".wma";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".opus";  ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".wv";    ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ape";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".alac";  ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mid";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".midi";  ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".dff";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".dsf";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".cda";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mod";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".s3m";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".xm";    ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".it";    ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mtm";   ValueData: "MediaAccess.AudioFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".umx";   ValueData: "MediaAccess.AudioFile"
; Video formats
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mp4";   ValueData: "MediaAccess.VideoFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mkv";   ValueData: "MediaAccess.VideoFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".avi";   ValueData: "MediaAccess.VideoFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mov";   ValueData: "MediaAccess.VideoFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".webm";  ValueData: "MediaAccess.VideoFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".flv";   ValueData: "MediaAccess.VideoFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".wmv";   ValueData: "MediaAccess.VideoFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ts";    ValueData: "MediaAccess.VideoFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m2ts";  ValueData: "MediaAccess.VideoFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".vob";   ValueData: "MediaAccess.VideoFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ogv";   ValueData: "MediaAccess.VideoFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".3gp";   ValueData: "MediaAccess.VideoFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mpg";   ValueData: "MediaAccess.VideoFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mpeg";  ValueData: "MediaAccess.VideoFile"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m4v";   ValueData: "MediaAccess.VideoFile"
; Playlist formats
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m3u";   ValueData: "MediaAccess.Playlist"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m3u8";  ValueData: "MediaAccess.Playlist"
Root: HKA; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".pls";   ValueData: "MediaAccess.Playlist"

; Step 5: Application registration in App Paths (allows running from Win+R)
Root: HKA; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\{#MyAppExeName}"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName}"; Flags: uninsdeletekey
Root: HKA; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\{#MyAppExeName}"; ValueType: string; ValueName: "Path"; ValueData: "{app}"

; Step 6: Register supported file types under "Open with" for each extension
; This adds MediaAccess to the "Open with" list even before being set as default
Root: HKA; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "MediaAccess"; Flags: uninsdeletekey
Root: HKA; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""
Root: HKA; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".mp3";  ValueData: ""
Root: HKA; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".mp4";  ValueData: ""
Root: HKA; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".mkv";  ValueData: ""
Root: HKA; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".avi";  ValueData: ""
Root: HKA; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".flac"; ValueData: ""
Root: HKA; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".m4a";  ValueData: ""
Root: HKA; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".webm"; ValueData: ""
Root: HKA; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".mov";  ValueData: ""

; ============================================================
; Step 7 (v2.64) - Explorer right-click "MediaAccess" SUBMENU.
;
; Structure: a parent verb key carrying MUIVerb + ExtendedSubCommandsKey, whose
; (Default) is DELIBERATELY left unset (a parent with a default value renders as
; a FLAT item instead of a submenu). ExtendedSubCommandsKey is the MICROSOFT-
; DOCUMENTED way to build a cascading menu without a COM handler; unlike the
; SubCommands+CommandStore form it needs no HKLM-only key, so it works in both
; admin and "just for me" install modes, and both the file and the folder parent
; can point at the SAME shared definition below.
;
; Registered on * (ALL file types) so the commands stay available on a MIXED
; selection (audio + a .jpg): a per-extension verb vanishes as soon as one
; selected file lacks it. Unsupported files are dropped by IsOpenableMediaPath.
; Also on Directory so a selected FOLDER works (new in v2.64; the app enumerates
; it recursively - src\main.cpp ParseCommandLine / WM_COPYDATA). Deliberately NOT
; on Folder or Drive: those cover drives and virtual folders, where a path can end
; in a backslash and break the "%1" quoting. Does NOT change the default player.
;
; MultiSelectModel=Player raises Explorer's LEGACY-verb ceiling from 15 to 100
; selected items (unset would mean Document = 15). It does NOT hand the whole
; selection to one process: Explorer launches ONE MediaAccess.exe PER SELECTED
; ITEM. Merging is done by MediaAccess itself (single-instance mutex ->
; WM_COPYDATA -> IDT_BATCH_FILES / IDT_BATCH_ENQUEUE coalescing). Above 100
; selected items the entry simply does not appear - use the folder entry instead.
; (The pre-2.64 comment here claimed Windows merged the selection; that was wrong.)
;
; On Windows 11 this lives in the legacy menu: Shift+F10 (mouse right-click shows
; the modern menu, where it is under "Show more options").
; ============================================================

; -- Remove the pre-2.64 FLAT verb so upgraders never see it alongside the new
; -- submenu. NO Tasks: clause on purpose - it must run even when the user
; -- DECLINES the submenu, otherwise the old entry would survive forever.
Root: HKA; Subkey: "SOFTWARE\Classes\*\shell\MediaAccess.Play"; Flags: deletekey

; -- Shared submenu definition (referenced by both parents below).
; -- deletekey sits on the FIRST entry that writes this subtree so the old shape is
; -- cleared before the new one is written, without relying on [Registry] ordering.
; --
; -- CRITICAL, VERIFIED ON A REAL MACHINE (v2.64 testing): NO key in this subtree
; -- may carry a (Default) value. With a (Default) on this key (a friendly name) or
; -- on its "shell" subkey (a "Play,Queue" order list), the submenu APPEARS in the
; -- context menu but REFUSES TO EXPAND (Enter / Right arrow do nothing) — the shell
; -- stops treating the target as a subcommand container. Hence ValueType: none,
; -- which creates the key WITHOUT any value, and no explicit "shell" entry at all
; -- (the Play/Queue entries below create it implicitly).
; -- Order therefore follows registry enumeration: "Play" sorts before "Queue",
; -- which is the order we want. Keep that in mind when adding a third command.
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.ContextMenu"; ValueType: none; Flags: deletekey uninsdeletekey; Tasks: explorermenu

Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.ContextMenu\shell\Play"; ValueType: string; ValueName: "MUIVerb"; ValueData: "{cm:PlayWithVerb}"; Tasks: explorermenu
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.ContextMenu\shell\Play"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\{#MyAppExeName}"",0"; Tasks: explorermenu
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.ContextMenu\shell\Play"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: explorermenu
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.ContextMenu\shell\Play\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""; Tasks: explorermenu

Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.ContextMenu\shell\Queue"; ValueType: string; ValueName: "MUIVerb"; ValueData: "{cm:EnqueueVerb}"; Tasks: explorermenu
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.ContextMenu\shell\Queue"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\{#MyAppExeName}"",0"; Tasks: explorermenu
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.ContextMenu\shell\Queue"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: explorermenu
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.ContextMenu\shell\Queue\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""/enqueue:%1"""; Tasks: explorermenu

; -- Parent verb on FILES. (Default) intentionally never written (see above).
Root: HKA; Subkey: "SOFTWARE\Classes\*\shell\MediaAccess"; ValueType: string; ValueName: "MUIVerb"; ValueData: "{cm:MediaAccessSubmenu}"; Flags: deletekey uninsdeletekey; Tasks: explorermenu
Root: HKA; Subkey: "SOFTWARE\Classes\*\shell\MediaAccess"; ValueType: string; ValueName: "ExtendedSubCommandsKey"; ValueData: "MediaAccess.ContextMenu"; Tasks: explorermenu
Root: HKA; Subkey: "SOFTWARE\Classes\*\shell\MediaAccess"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\{#MyAppExeName}"",0"; Tasks: explorermenu
Root: HKA; Subkey: "SOFTWARE\Classes\*\shell\MediaAccess"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: explorermenu

; -- Parent verb on FOLDERS (new in v2.64).
Root: HKA; Subkey: "SOFTWARE\Classes\Directory\shell\MediaAccess"; ValueType: string; ValueName: "MUIVerb"; ValueData: "{cm:MediaAccessSubmenu}"; Flags: deletekey uninsdeletekey; Tasks: explorermenu
Root: HKA; Subkey: "SOFTWARE\Classes\Directory\shell\MediaAccess"; ValueType: string; ValueName: "ExtendedSubCommandsKey"; ValueData: "MediaAccess.ContextMenu"; Tasks: explorermenu
Root: HKA; Subkey: "SOFTWARE\Classes\Directory\shell\MediaAccess"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\{#MyAppExeName}"",0"; Tasks: explorermenu
Root: HKA; Subkey: "SOFTWARE\Classes\Directory\shell\MediaAccess"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"; Tasks: explorermenu

; -- When the submenu task is DECLINED, remove any subtree left by a previous
; -- install (these run unconditionally; deletekey is a no-op when absent).
Root: HKA; Subkey: "SOFTWARE\Classes\*\shell\MediaAccess"; Flags: deletekey; Check: not WantsExplorerMenu
Root: HKA; Subkey: "SOFTWARE\Classes\Directory\shell\MediaAccess"; Flags: deletekey; Check: not WantsExplorerMenu
Root: HKA; Subkey: "SOFTWARE\Classes\MediaAccess.ContextMenu"; Flags: deletekey; Check: not WantsExplorerMenu

; v2.51 - remove the 2.41-2.50 per-extension verbs so upgraders never see a
; duplicate "Lire avec MediaAccess" alongside the new * verb. deletekey runs at
; install time and is a harmless no-op when the key is absent (fresh install).
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.mp3\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.mp2\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.wav\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.ogg\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.aiff\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.flac\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.aac\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.m4a\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.m4b\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.m4r\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.wma\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.opus\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.wv\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.ape\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.alac\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.mid\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.midi\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.dff\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.dsf\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.cda\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.mod\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.s3m\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.xm\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.it\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.mtm\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.umx\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.mp4\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.mkv\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.avi\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.mov\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.webm\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.flv\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.wmv\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.ts\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.m2ts\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.vob\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.ogv\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.3gp\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.mpg\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.mpeg\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.m4v\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.m3u\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.m3u8\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.pls\shell\MediaAccess.Play"; Flags: deletekey
Root: HKA; Subkey: "SOFTWARE\Classes\SystemFileAssociations\.cue\shell\MediaAccess.Play"; Flags: deletekey


[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Readme"; Filename: "{app}\docs\readme.txt"
Name: "{group}\Changelog"; Filename: "{app}\docs\changelog_fr.html"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon
Name: "{userappdata}\Microsoft\Internet Explorer\Quick Launch\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: quicklaunchicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
; v2.33 — in-app auto-update relaunch. The in-app updater runs this installer
; as "/SILENT /AUTOUPDATE=1", which skips the postinstall checkbox above. This
; entry fires only on that path (Check: IsAutoUpdate) and relaunches the app
; even in silent mode, as the original (non-elevated) user, with /fromupdate so
; the new instance forces itself to the foreground for screen-reader focus.
Filename: "{app}\{#MyAppExeName}"; Parameters: "/fromupdate"; Flags: nowait runasoriginaluser; Check: IsAutoUpdate

[UninstallDelete]
Type: files; Name: "{app}\installed.txt"

[Code]
// v2.64 — Check function for the [Registry] cleanup entries that must run when
// the user DECLINES the Explorer submenu task (so a previous install's keys are
// removed rather than left behind).
function WantsExplorerMenu(): Boolean;
begin
  Result := WizardIsTaskSelected('explorermenu');
end;

// Create installed marker file after installation
procedure CreateInstalledMarker();
var
  MarkerFile: String;
begin
  MarkerFile := ExpandConstant('{app}\installed.txt');
  SaveStringToFile(MarkerFile, 'Installed via setup', False);
end;

// v2.33 — true when the in-app updater launched us with /AUTOUPDATE=1.
// Used by the [Run] entry that auto-relaunches the app after a silent update.
function IsAutoUpdate(): Boolean;
begin
  Result := ExpandConstant('{param:AUTOUPDATE|0}') = '1';
end;
