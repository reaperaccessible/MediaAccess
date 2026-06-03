<#
.SYNOPSIS
    Download the latest static ffmpeg + ffprobe (Windows x64) into lib\.

.DESCRIPTION
    Pulls the yt-dlp FFmpeg-Builds "latest" GPL build, extracts bin\ffmpeg.exe
    and bin\ffprobe.exe into the project's lib\ folder, and rewrites
    lib\ffmpeg_version.txt with today's date. These two executables are what
    GetFfmpegLocation() resolves at runtime and what build_new.bat /
    installer.iss bundle into the shipped app.

    Run this from anywhere; paths are resolved relative to this script's
    location (scripts\ -> project root -> lib\).

.NOTES
    Source: https://github.com/yt-dlp/FFmpeg-Builds (GPL, win64)
    Re-run whenever you want to refresh the bundled ffmpeg.
#>

$ErrorActionPreference = 'Stop'

# scripts\fetch_ffmpeg.ps1  ->  project root is the parent of scripts\
$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir
$libDir     = Join-Path $projectDir 'lib'

$buildName = 'ffmpeg-master-latest-win64-gpl'
$zipUrl    = "https://github.com/yt-dlp/FFmpeg-Builds/releases/download/latest/$buildName.zip"

if (-not (Test-Path $libDir)) {
    New-Item -ItemType Directory -Path $libDir | Out-Null
}

$tmpDir = Join-Path ([System.IO.Path]::GetTempPath()) ("ffmpeg_fetch_" + [System.Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $tmpDir | Out-Null
$zipPath = Join-Path $tmpDir 'ffmpeg.zip'

try {
    Write-Host "Downloading $zipUrl ..."
    # TLS 1.2 for older PowerShell hosts; harmless on PS7.
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $zipUrl -OutFile $zipPath -UseBasicParsing

    Write-Host "Extracting ..."
    Expand-Archive -Path $zipPath -DestinationPath $tmpDir -Force

    # Archive lays out: <buildName>\bin\ffmpeg.exe, ffprobe.exe
    $ffmpeg  = Get-ChildItem -Path $tmpDir -Recurse -Filter 'ffmpeg.exe'  | Select-Object -First 1
    $ffprobe = Get-ChildItem -Path $tmpDir -Recurse -Filter 'ffprobe.exe' | Select-Object -First 1

    if (-not $ffmpeg)  { throw "ffmpeg.exe not found in downloaded archive." }
    if (-not $ffprobe) { throw "ffprobe.exe not found in downloaded archive." }

    Copy-Item -Path $ffmpeg.FullName  -Destination (Join-Path $libDir 'ffmpeg.exe')  -Force
    Copy-Item -Path $ffprobe.FullName -Destination (Join-Path $libDir 'ffprobe.exe') -Force

    $today = (Get-Date).ToString('yyyy-MM-dd')
    $verContent = "source=yt-dlp/FFmpeg-Builds`nbuild=$buildName`nupdated=$today`n"
    Set-Content -Path (Join-Path $libDir 'ffmpeg_version.txt') -Value $verContent -NoNewline -Encoding utf8

    Write-Host "Done. ffmpeg.exe + ffprobe.exe updated in $libDir (build $buildName, $today)."
}
finally {
    Remove-Item -Path $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
}
