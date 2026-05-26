; Inno Setup script for MediaAccess
; This script is used by the GitHub Actions workflow to create an installer

#define MyAppName "MediaAccess"
#define MyAppPublisher "Mew"
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
OutputBaseFilename=MediaAccessInstaller
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
; Install documentation
Source: "{#SourceDir}\docs\readme.txt"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "{#SourceDir}\docs\changelog.txt"; DestDir: "{app}\docs"; Flags: ignoreversion
; Create installed marker file
Source: "{#SourceDir}\MediaAccess.exe"; DestDir: "{app}"; AfterInstall: CreateInstalledMarker; Flags: ignoreversion

[Registry]
; ============================================================
; Register MediaAccess as a "Default App" candidate in Windows 10/11
; This makes MediaAccess appear in Settings > Apps > Default apps
; ============================================================

; Step 1: Register the application under HKLM\SOFTWARE\RegisteredApplications
Root: HKLM; Subkey: "SOFTWARE\RegisteredApplications"; ValueType: string; ValueName: "MediaAccess"; ValueData: "SOFTWARE\MediaAccess\Capabilities"; Flags: uninsdeletevalue

; Step 2: Application capabilities (metadata)
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "MediaAccess"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "{cm:AppDesc}"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities"; ValueType: string; ValueName: "ApplicationIcon"; ValueData: """{app}\{#MyAppExeName}"",0"

; Step 3: ProgIDs for audio and video files
Root: HKLM; Subkey: "SOFTWARE\Classes\MediaAccess.AudioFile"; ValueType: string; ValueName: ""; ValueData: "{cm:AudioFileDesc}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\MediaAccess.AudioFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"",0"
Root: HKLM; Subkey: "SOFTWARE\Classes\MediaAccess.AudioFile\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "MediaAccess"
Root: HKLM; Subkey: "SOFTWARE\Classes\MediaAccess.AudioFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

Root: HKLM; Subkey: "SOFTWARE\Classes\MediaAccess.VideoFile"; ValueType: string; ValueName: ""; ValueData: "{cm:VideoFileDesc}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\MediaAccess.VideoFile\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"",0"
Root: HKLM; Subkey: "SOFTWARE\Classes\MediaAccess.VideoFile\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "MediaAccess"
Root: HKLM; Subkey: "SOFTWARE\Classes\MediaAccess.VideoFile\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

Root: HKLM; Subkey: "SOFTWARE\Classes\MediaAccess.Playlist"; ValueType: string; ValueName: ""; ValueData: "{cm:PlaylistDesc}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\MediaAccess.Playlist\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"",0"
Root: HKLM; Subkey: "SOFTWARE\Classes\MediaAccess.Playlist\shell\open"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "MediaAccess"
Root: HKLM; Subkey: "SOFTWARE\Classes\MediaAccess.Playlist\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""

; Step 4: File extension associations (which ProgID handles which extension)
; Audio formats
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mp3";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mp2";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".wav";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ogg";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".aiff";  ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".flac";  ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".aac";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m4a";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m4b";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m4r";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".wma";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".opus";  ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".wv";    ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ape";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".alac";  ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mid";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".midi";  ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".dff";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".dsf";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".cda";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mod";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".s3m";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".xm";    ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".it";    ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mtm";   ValueData: "MediaAccess.AudioFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".umx";   ValueData: "MediaAccess.AudioFile"
; Video formats
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mp4";   ValueData: "MediaAccess.VideoFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mkv";   ValueData: "MediaAccess.VideoFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".avi";   ValueData: "MediaAccess.VideoFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mov";   ValueData: "MediaAccess.VideoFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".webm";  ValueData: "MediaAccess.VideoFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".flv";   ValueData: "MediaAccess.VideoFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".wmv";   ValueData: "MediaAccess.VideoFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ts";    ValueData: "MediaAccess.VideoFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m2ts";  ValueData: "MediaAccess.VideoFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".vob";   ValueData: "MediaAccess.VideoFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ogv";   ValueData: "MediaAccess.VideoFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".3gp";   ValueData: "MediaAccess.VideoFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mpg";   ValueData: "MediaAccess.VideoFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mpeg";  ValueData: "MediaAccess.VideoFile"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m4v";   ValueData: "MediaAccess.VideoFile"
; Playlist formats
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m3u";   ValueData: "MediaAccess.Playlist"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m3u8";  ValueData: "MediaAccess.Playlist"
Root: HKLM; Subkey: "SOFTWARE\MediaAccess\Capabilities\FileAssociations"; ValueType: string; ValueName: ".pls";   ValueData: "MediaAccess.Playlist"

; Step 5: Application registration in App Paths (allows running from Win+R)
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\{#MyAppExeName}"; ValueType: string; ValueName: ""; ValueData: "{app}\{#MyAppExeName}"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\{#MyAppExeName}"; ValueType: string; ValueName: "Path"; ValueData: "{app}"

; Step 6: Register supported file types under "Open with" for each extension
; This adds MediaAccess to the "Open with" list even before being set as default
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}"; ValueType: string; ValueName: "FriendlyAppName"; ValueData: "MediaAccess"; Flags: uninsdeletekey
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\{#MyAppExeName}"" ""%1"""
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".mp3";  ValueData: ""
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".mp4";  ValueData: ""
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".mkv";  ValueData: ""
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".avi";  ValueData: ""
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".flac"; ValueData: ""
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".m4a";  ValueData: ""
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".webm"; ValueData: ""
Root: HKLM; Subkey: "SOFTWARE\Classes\Applications\{#MyAppExeName}\SupportedTypes"; ValueType: string; ValueName: ".mov";  ValueData: ""

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\Readme"; Filename: "{app}\docs\readme.txt"
Name: "{group}\Changelog"; Filename: "{app}\docs\changelog.txt"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon
Name: "{userappdata}\Microsoft\Internet Explorer\Quick Launch\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: quicklaunchicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent

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
