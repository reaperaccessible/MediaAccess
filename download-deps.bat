@echo off
REM Download dependencies for MediaAccess
setlocal enabledelayedexpansion
cd /d "%~dp0"

echo ============================================
echo MediaAccess Dependency Downloader
echo ============================================
echo.

REM Check for git
where git >nul 2>&1
if errorlevel 1 (
    echo Error: Git is not installed or not in PATH.
    echo Please install Git from https://git-scm.com/
    exit /b 1
)

REM Create directories
if not exist "lib" mkdir lib
if not exist "deps" mkdir deps

REM Temporary download folder
if not exist "temp_dl" mkdir temp_dl

echo Downloading BASS libraries...
echo.

REM BASS core
echo [1/18] Downloading BASS...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/bass24.zip' -OutFile 'temp_dl\bass.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\bass.zip' -DestinationPath 'temp_dl\bass' -Force"
copy /y "temp_dl\bass\c\x64\bass.lib" "lib\" >nul
copy /y "temp_dl\bass\x64\bass.dll" "lib\" >nul

REM BASS_FX
echo [2/18] Downloading BASS_FX...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/z/0/bass_fx24.zip' -OutFile 'temp_dl\bass_fx.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\bass_fx.zip' -DestinationPath 'temp_dl\bass_fx' -Force"
copy /y "temp_dl\bass_fx\c\x64\bass_fx.lib" "lib\" >nul
copy /y "temp_dl\bass_fx\x64\bass_fx.dll" "lib\" >nul

REM BASS_AAC
echo [3/18] Downloading BASS_AAC...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/z/2/bass_aac24.zip' -OutFile 'temp_dl\bass_aac.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\bass_aac.zip' -DestinationPath 'temp_dl\bass_aac' -Force"
copy /y "temp_dl\bass_aac\c\x64\bass_aac.lib" "lib\" >nul
copy /y "temp_dl\bass_aac\x64\bass_aac.dll" "lib\" >nul

REM BASSALAC
echo [4/18] Downloading BASSALAC...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/bassalac24.zip' -OutFile 'temp_dl\bassalac.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\bassalac.zip' -DestinationPath 'temp_dl\bassalac' -Force"
copy /y "temp_dl\bassalac\x64\bassalac.dll" "lib\" >nul

REM BASSAPE
echo [5/18] Downloading BASSAPE...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/bassape24.zip' -OutFile 'temp_dl\bassape.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\bassape.zip' -DestinationPath 'temp_dl\bassape' -Force"
copy /y "temp_dl\bassape\x64\bassape.dll" "lib\" >nul

REM BASSCD
echo [6/18] Downloading BASSCD...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/basscd24.zip' -OutFile 'temp_dl\basscd.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\basscd.zip' -DestinationPath 'temp_dl\basscd' -Force"
copy /y "temp_dl\basscd\x64\basscd.dll" "lib\" >nul

REM BASSDSD
echo [7/18] Downloading BASSDSD...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/bassdsd24.zip' -OutFile 'temp_dl\bassdsd.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\bassdsd.zip' -DestinationPath 'temp_dl\bassdsd' -Force"
copy /y "temp_dl\bassdsd\x64\bassdsd.dll" "lib\" >nul

REM BASSFLAC
echo [8/18] Downloading BASSFLAC...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/bassflac24.zip' -OutFile 'temp_dl\bassflac.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\bassflac.zip' -DestinationPath 'temp_dl\bassflac' -Force"
copy /y "temp_dl\bassflac\x64\bassflac.dll" "lib\" >nul

REM BASSHLS
echo [9/18] Downloading BASSHLS...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/basshls24.zip' -OutFile 'temp_dl\basshls.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\basshls.zip' -DestinationPath 'temp_dl\basshls' -Force"
copy /y "temp_dl\basshls\x64\basshls.dll" "lib\" >nul

REM BASSMIDI
echo [10/18] Downloading BASSMIDI...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/bassmidi24.zip' -OutFile 'temp_dl\bassmidi.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\bassmidi.zip' -DestinationPath 'temp_dl\bassmidi' -Force"
copy /y "temp_dl\bassmidi\c\x64\bassmidi.lib" "lib\" >nul
copy /y "temp_dl\bassmidi\x64\bassmidi.dll" "lib\" >nul

REM BASSOPUS
echo [11/18] Downloading BASSOPUS...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/bassopus24.zip' -OutFile 'temp_dl\bassopus.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\bassopus.zip' -DestinationPath 'temp_dl\bassopus' -Force"
copy /y "temp_dl\bassopus\x64\bassopus.dll" "lib\" >nul

REM BASSWMA
echo [12/18] Downloading BASSWMA...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/basswm24.zip' -OutFile 'temp_dl\basswma.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\basswma.zip' -DestinationPath 'temp_dl\basswma' -Force"
copy /y "temp_dl\basswma\x64\basswma.dll" "lib\" >nul

REM BASSWV
echo [13/18] Downloading BASSWV...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/basswv24.zip' -OutFile 'temp_dl\basswv.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\basswv.zip' -DestinationPath 'temp_dl\basswv' -Force"
copy /y "temp_dl\basswv\x64\basswv.dll" "lib\" >nul

REM BASSMIX
echo [14/18] Downloading BASSMIX...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/bassmix24.zip' -OutFile 'temp_dl\bassmix.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\bassmix.zip' -DestinationPath 'temp_dl\bassmix' -Force"
copy /y "temp_dl\bassmix\c\x64\bassmix.lib" "lib\" >nul
copy /y "temp_dl\bassmix\x64\bassmix.dll" "lib\" >nul

REM BASSENC
echo [15/18] Downloading BASSENC...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/bassenc24.zip' -OutFile 'temp_dl\bassenc.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\bassenc.zip' -DestinationPath 'temp_dl\bassenc' -Force"
copy /y "temp_dl\bassenc\c\x64\bassenc.lib" "lib\" >nul
copy /y "temp_dl\bassenc\x64\bassenc.dll" "lib\" >nul

REM BASSENC_MP3
echo [16/18] Downloading BASSENC_MP3...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/bassenc_mp324.zip' -OutFile 'temp_dl\bassenc_mp3.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\bassenc_mp3.zip' -DestinationPath 'temp_dl\bassenc_mp3' -Force"
copy /y "temp_dl\bassenc_mp3\c\x64\bassenc_mp3.lib" "lib\" >nul
copy /y "temp_dl\bassenc_mp3\x64\bassenc_mp3.dll" "lib\" >nul

REM BASSENC_OGG
echo [17/18] Downloading BASSENC_OGG...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/bassenc_ogg24.zip' -OutFile 'temp_dl\bassenc_ogg.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\bassenc_ogg.zip' -DestinationPath 'temp_dl\bassenc_ogg' -Force"
copy /y "temp_dl\bassenc_ogg\c\x64\bassenc_ogg.lib" "lib\" >nul
copy /y "temp_dl\bassenc_ogg\x64\bassenc_ogg.dll" "lib\" >nul

REM BASSENC_FLAC
echo [18/18] Downloading BASSENC_FLAC...
powershell -Command "Invoke-WebRequest -Uri 'https://www.un4seen.com/files/bassenc_flac24.zip' -OutFile 'temp_dl\bassenc_flac.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\bassenc_flac.zip' -DestinationPath 'temp_dl\bassenc_flac' -Force"
copy /y "temp_dl\bassenc_flac\c\x64\bassenc_flac.lib" "lib\" >nul
copy /y "temp_dl\bassenc_flac\x64\bassenc_flac.dll" "lib\" >nul

echo.
echo Downloading Steam Audio SDK (for 3D spatial audio)...
if exist "deps\steamaudio" rmdir /s /q "deps\steamaudio"
mkdir "deps\steamaudio\include"
powershell -Command "Invoke-WebRequest -Uri 'https://github.com/ValveSoftware/steam-audio/releases/download/v4.6.1/steamaudio_4.6.1.zip' -OutFile 'temp_dl\steamaudio.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\steamaudio.zip' -DestinationPath 'temp_dl\steamaudio' -Force"
copy /y "temp_dl\steamaudio\steamaudio\include\phonon.h" "deps\steamaudio\include\" >nul
copy /y "temp_dl\steamaudio\steamaudio\include\phonon_interfaces.h" "deps\steamaudio\include\" >nul
copy /y "temp_dl\steamaudio\steamaudio\include\phonon_version.h" "deps\steamaudio\include\" >nul
copy /y "temp_dl\steamaudio\steamaudio\lib\windows-x64\phonon.dll" "lib\" >nul
copy /y "temp_dl\steamaudio\steamaudio\lib\windows-x64\phonon.lib" "lib\" >nul
echo Steam Audio SDK installed.

echo.
echo Downloading SQLite...
powershell -Command "Invoke-WebRequest -Uri 'https://sqlite.org/2026/sqlite-amalgamation-3510200.zip' -OutFile 'temp_dl\sqlite.zip'"
powershell -Command "Expand-Archive -Path 'temp_dl\sqlite.zip' -DestinationPath 'temp_dl\sqlite' -Force"
copy /y "temp_dl\sqlite\sqlite-amalgamation-3510200\sqlite3.c" "src\" >nul

echo.
echo Cloning Speedy (Google's nonlinear speech speedup)...
if exist "deps\speedy" rmdir /s /q "deps\speedy"
git clone --depth 1 https://github.com/google/speedy.git "deps\speedy"

echo.
echo Cloning Signalsmith Stretch...
if exist "deps\signalsmith-stretch" rmdir /s /q "deps\signalsmith-stretch"
git clone --depth 1 https://github.com/Signalsmith-Audio/signalsmith-stretch.git "deps\signalsmith-stretch"
REM Also clone signalsmith-linear which is a separate dependency
git clone --depth 1 https://github.com/Signalsmith-Audio/linear.git "deps\signalsmith-stretch\signalsmith-linear"

echo.
echo Cloning Sonic...
if exist "deps\sonic" rmdir /s /q "deps\sonic"
git clone --depth 1 https://github.com/waywardgeek/sonic.git "deps\sonic"

echo.
echo Cloning KissFFT...
if exist "deps\kissfft" rmdir /s /q "deps\kissfft"
git clone --depth 1 https://github.com/mborgerding/kissfft.git "deps\kissfft"

echo.
echo Cloning Universal Speech (screen reader support)...
if exist "deps\UniversalSpeech" rmdir /s /q "deps\UniversalSpeech"
git clone --depth 1 https://github.com/samtupy/UniversalSpeechMSVCStatic.git "deps\UniversalSpeech"

echo.
echo Building Universal Speech...
pushd deps\UniversalSpeech
REM Build using SCons (requires Python and SCons: pip install scons)
where scons >nul 2>&1
if errorlevel 1 (
    echo SCons not found. Installing via pip...
    pip install scons
)
call scons
if exist "UniversalSpeechStatic.lib" (
    copy /y "UniversalSpeechStatic.lib" "..\..\lib\" >nul
    echo Universal Speech built successfully.
) else (
    echo WARNING: Universal Speech build failed.
    echo Make sure Python, pip, and Visual C++ Build Tools are installed.
)
REM Copy screen reader DLLs from build x64 folder
if exist "bin-x64\nvdaControllerClient64.dll" (
    copy /y "bin-x64\nvdaControllerClient64.dll" "..\..\lib\" >nul
    echo NVDA controller client DLL copied.
) else (
    echo WARNING: nvdaControllerClient64.dll not found in bin-x64 folder.
)
if exist "bin-x64\SAAPI64.dll" (
    copy /y "bin-x64\SAAPI64.dll" "..\..\lib\" >nul
    echo SAAPI64.dll copied.
)
popd

echo.
echo Cleaning up...
rmdir /s /q temp_dl 2>nul

echo.
echo ============================================
echo Download and build complete!
echo ============================================
echo.
echo Run build_new.bat to compile MediaAccess.
echo.

echo.

REM ============================================================
REM libmpv (video playback engine) - auto-download and bundle
REM ============================================================
echo.
echo ===== libmpv (video playback engine) =====
if exist "lib\libmpv-2.dll" (
    echo libmpv-2.dll already present in lib\, skipping download.
) else if exist "lib\mpv-2.dll" (
    echo mpv-2.dll already present in lib\, skipping download.
) else if exist "lib\mpv-1.dll" (
    echo mpv-1.dll already present in lib\, skipping download.
) else (
    echo Fetching latest libmpv release from GitHub...
    if not exist temp_dl mkdir temp_dl

    REM Use PowerShell to query GitHub API and download the GENERIC (non-v3) libmpv build.
    REM The -v3 build requires x86_64-v3 CPU baseline (AVX2/BMI/FMA, Haswell 2013+ /
    REM Excavator 2015+) and silently fails to load on older CPUs. The generic build
    REM works on every 64-bit Windows CPU. (Switched in v1.0.20 after user reports
    REM of "video engine could not be loaded" — diagnosed as the v3 ISA baseline.)
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "$ErrorActionPreference='Stop';" ^
        "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12;" ^
        "$r = Invoke-RestMethod -Uri 'https://api.github.com/repos/shinchiro/mpv-winbuild-cmake/releases/latest' -UseBasicParsing;" ^
        "$asset = $r.assets | Where-Object { $_.name -like 'mpv-dev-x86_64-*.7z' -and $_.name -notlike '*v3*' } | Select-Object -First 1;" ^
        "if (-not $asset) { $asset = $r.assets | Where-Object { $_.name -like 'mpv-dev-x86_64-v3-*.7z' } | Select-Object -First 1 }" ^
        "if (-not $asset) { Write-Host 'No libmpv asset found in latest release.'; exit 1 }" ^
        "Write-Host ('Downloading ' + $asset.name + ' (' + [math]::Round($asset.size/1MB,1) + ' MB)...');" ^
        "Invoke-WebRequest -Uri $asset.browser_download_url -OutFile 'temp_dl\libmpv.7z' -UseBasicParsing;" ^
        "Write-Host 'Extracting...';" ^
        "if (Test-Path 'temp_dl\libmpv-ext') { Remove-Item 'temp_dl\libmpv-ext' -Recurse -Force }" ^
        "New-Item 'temp_dl\libmpv-ext' -ItemType Directory | Out-Null;" ^
        "tar -xf 'temp_dl\libmpv.7z' -C 'temp_dl\libmpv-ext';" ^
        "if (-not (Test-Path 'lib')) { New-Item 'lib' -ItemType Directory | Out-Null }" ^
        "Copy-Item 'temp_dl\libmpv-ext\libmpv-2.dll' 'lib\' -Force;" ^
        "Write-Host 'libmpv-2.dll installed in lib\' -ForegroundColor Green;"

    if errorlevel 1 (
        echo WARNING: libmpv download failed. MediaAccess will work as audio-only player.
        echo You can manually download libmpv from:
        echo   https://github.com/shinchiro/mpv-winbuild-cmake/releases/latest
        echo and copy libmpv-2.dll to lib\
    )
    REM Cleanup extraction
    if exist temp_dl\libmpv-ext rmdir /s /q temp_dl\libmpv-ext 2>/dev/null
    if exist temp_dl\libmpv.7z del /q temp_dl\libmpv.7z 2>/dev/null
    if exist temp_dl rmdir temp_dl 2>/dev/null
)
echo.

REM ============================================================
REM FluidR3_GM SoundFont (bundled so MIDI files sound great out of the box)
REM
REM License: MIT (Frank Wen, 2002). The Internet Archive item "fluidr3-gm-gs"
REM hosts the GM+GS variant (151 MB), which is a strict superset of the
REM GM-only build — fine for everything MediaAccess does with MIDI. We save
REM it locally as FluidR3_GM.sf2 because that's what player.cpp probes for.
REM
REM URL stability: Internet Archive items don't move, and IA serves stable
REM 302 redirects to mirrors. If this ever breaks, alternative mirrors:
REM   - https://musical-artifacts.com/artifacts/738 (manual download page)
REM   - https://member.keymusician.com/Member/FluidR3_GM/index.html
REM ============================================================
echo.
echo ===== FluidR3_GM SoundFont (for MIDI playback) =====
if exist "lib\FluidR3_GM.sf2" (
    echo lib\FluidR3_GM.sf2 already present, skipping download.
) else (
    echo Downloading FluidR3_GM.sf2 ^(~144 MB, this may take a few minutes^)...
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "$ErrorActionPreference='Stop';" ^
        "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12;" ^
        "$tmp = 'lib\FluidR3_GM.sf2.part';" ^
        "Invoke-WebRequest -Uri 'https://archive.org/download/fluidr3-gm-gs/FluidR3_GM_GS.sf2' -OutFile $tmp -UseBasicParsing;" ^
        "$sz = (Get-Item $tmp).Length;" ^
        "if ($sz -lt 100000000) { Remove-Item $tmp -Force; throw ('Downloaded file too small (' + $sz + ' bytes) — likely an HTML error page. Aborting.') }" ^
        "Move-Item -Force $tmp 'lib\FluidR3_GM.sf2';" ^
        "Write-Host ('FluidR3_GM.sf2 installed in lib\ (' + [math]::Round($sz/1MB,1) + ' MB)') -ForegroundColor Green;"
    if errorlevel 1 (
        echo WARNING: FluidR3_GM.sf2 download failed. MIDI files will fall back
        echo to the BASSMIDI built-in synth ^(basic sound^). You can manually
        echo place a SoundFont at lib\FluidR3_GM.sf2 or set the SoundFont path
        echo in MediaAccess Options ^> MIDI.
    )
)

REM ============================================================
REM yt-dlp.exe (YouTube downloader, bundled with installer)
REM ============================================================
echo.
echo ===== yt-dlp.exe =====
if exist "lib\yt-dlp.exe" (
    echo yt-dlp.exe already present in lib\, skipping download.
) else (
    echo Downloading latest yt-dlp.exe...
    powershell -NoProfile -ExecutionPolicy Bypass -Command ^
        "$ErrorActionPreference='Stop';" ^
        "[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12;" ^
        "Invoke-WebRequest -Uri 'https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe' -OutFile 'lib\yt-dlp.exe' -UseBasicParsing;" ^
        "Write-Host 'yt-dlp.exe installed in lib\' -ForegroundColor Green;"
    if errorlevel 1 (
        echo WARNING: yt-dlp.exe download failed. YouTube features will be unavailable.
    )
)
