@echo off
REM Wrapper for validate_keymaps.py — invoked from build_new.bat before compile.
REM Propagates the Python exit code so a non-zero validation aborts the build.

setlocal
set "SCRIPT_DIR=%~dp0"
python "%SCRIPT_DIR%validate_keymaps.py"
exit /b %ERRORLEVEL%
