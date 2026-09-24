@echo off
REM Wrapper so CubeMX can run restore_ld_script.py after code generation.
REM Registered in open_plc_cube_ide.ioc as ProjectManager.UAScriptAfterPath.
REM A .bat rather than the .py directly: whether CubeMX honours the Windows
REM file association for .py is not something this project relies on.
REM See $PROD/docs/build/CUBEMX-RULES.md.
setlocal
set "SCRIPT=%~dp0restore_ld_script.py"
where python >nul 2>&1
if %ERRORLEVEL%==0 (
  python "%SCRIPT%"
) else (
  py -3 "%SCRIPT%"
)
exit /b %ERRORLEVEL%
