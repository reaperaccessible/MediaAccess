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

; v2.41 — Explorer right-click "Play with MediaAccess" verb label (install language)
english.PlayWithVerb=Play with MediaAccess
french.PlayWithVerb=Lire avec MediaAccess

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "quicklaunchicon"; Description: "{cm:CreateQuickLaunchIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked; OnlyBelowVersion: 6.1; Check: not IsAdminInstallMode

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
; Step 7 (v2.51) - Explorer right-click "Lire avec MediaAccess".
; Registered on * (ALL file types) so the command stays available even on a
; MIXED selection (e.g. audio files + a .jpg): a per-extension verb vanishes
; as soon as one selected file lacks it, and Windows refuses Enter on a
; mixed-default selection. MultiSelectModel=Player makes Windows hand the
; WHOLE selection to a single MediaAccess instance, which then ignores the
; unsupported files (IsOpenableMediaPath). Does NOT change the default player.
; On Windows 11 it appears under "Show more options" (Shift+F10 / Menu key).
; ============================================================
Root: HKA; Subkey: "SOFTWARE\Classes\*\shell\MediaAccess.Play"; ValueType: string; ValueName: ""; ValueData: "{cm:PlayWithVerb}"; Flags: uninsdeletekey
Root: HKA; Subkey: "SOFTWARE\Classes\*\shell\MediaAccess.Play"; ValueType: string; ValueName: "Icon"; ValueData: """{app}\{#MyAppExeName}"",0"
Root: HKA; Subkey: "SOFTWARE\Classes\*\shell\MediaAccess.Play"; ValueType: string; ValueName: "MultiSelectModel"; ValueData: "Player"
Root: HKA; Subkey: "SOFTWARE\Classes\*\shell\MediaAccess.Play\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

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
