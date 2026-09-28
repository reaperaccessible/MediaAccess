<#
.SYNOPSIS
    Put the latest yt-dlp NIGHTLY build into lib\yt-dlp.exe (v2.72).

.DESCRIPTION
    Called by build_new.bat before the installer is built, so every release
    ships the current nightly (the app's own updater then keeps it current).
    The download is installed only after the same checks the app performs:
    byte size == the asset size from the GitHub API, SHA-256 == its line in
    SHA2-256SUMS, and "yt-dlp.exe --version" == the release tag. On any
    failure lib\yt-dlp.exe is left untouched and the script exits 1.

.NOTES
    Source: https://github.com/yt-dlp/yt-dlp-nightly-builds
    Windows PowerShell 5.1 compatible (ASCII + BOM).
#>

$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$scriptDir  = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectDir = Split-Path -Parent $scriptDir
$libDir     = Join-Path $projectDir 'lib'
$target     = Join-Path $libDir 'yt-dlp.exe'
$tmp        = Join-Path $libDir 'yt-dlp.new.exe'   # .exe so PowerShell can run it

try {
    $headers = @{ 'Accept' = 'application/vnd.github+json'; 'User-Agent' = 'MediaAccess-build' }
    $rel = Invoke-RestMethod -Uri 'https://api.github.com/repos/yt-dlp/yt-dlp-nightly-builds/releases/latest' -Headers $headers -UseBasicParsing
    $tag = $rel.tag_name
    $exe = $rel.assets | Where-Object { $_.name -eq 'yt-dlp.exe' } | Select-Object -First 1
    $sums = $rel.assets | Where-Object { $_.name -eq 'SHA2-256SUMS' } | Select-Object -First 1
    if (-not $tag -or -not $exe -or -not $sums) { throw 'release JSON not understood' }

    if (Test-Path $target) {
        $current = (& $target --version 2>$null | Out-String).Trim()
        if ($current -eq $tag) {
            Write-Host "yt-dlp: lib\yt-dlp.exe already at nightly $tag"
            exit 0
        }
    }

    Write-Host "yt-dlp: downloading nightly $tag ..."
    Invoke-WebRequest -Uri $exe.browser_download_url -OutFile $tmp -UseBasicParsing -Headers @{ 'User-Agent' = 'MediaAccess-build' }

    $size = (Get-Item $tmp).Length
    if ($size -ne [int64]$exe.size) { throw "size check failed ($size, expected $($exe.size))" }

    $sumsText = (Invoke-WebRequest -Uri $sums.browser_download_url -UseBasicParsing -Headers @{ 'User-Agent' = 'MediaAccess-build' }).Content
    if ($sumsText -is [byte[]]) { $sumsText = [Text.Encoding]::ASCII.GetString($sumsText) }
    $expected = $null
    foreach ($line in ($sumsText -split "`n")) {
        $parts = $line.Trim() -split '\s+', 2
        if ($parts.Count -eq 2 -and $parts[1].TrimStart('*') -eq 'yt-dlp.exe') { $expected = $parts[0].ToLower() }
    }
    $actual = (Get-FileHash -Algorithm SHA256 -Path $tmp).Hash.ToLower()
    if (-not $expected -or $actual -ne $expected) { throw "SHA-256 check failed ($actual, expected $expected)" }

    $ver = (& $tmp --version 2>$null | Out-String).Trim()
    if ($ver -ne $tag) { throw "--version check failed ('$ver')" }

    Move-Item -Force $tmp $target
    Write-Host "yt-dlp: lib\yt-dlp.exe updated to nightly $tag (size, SHA-256 and version checked)"
    exit 0
}
catch {
    if (Test-Path $tmp) { Remove-Item -Force $tmp }
    Write-Host "yt-dlp: WARNING, bundled copy NOT updated: $($_.Exception.Message)"
    exit 1
}
