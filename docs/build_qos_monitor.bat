@echo off
setlocal
set CSC=C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe

if not exist "%CSC%" (
    echo [ERROR] csc.exe not found at %CSC%
    pause
    exit /b 1
)

echo Compiling EcoQoSMonitor.cs ...
"%CSC%" /nologo /target:winexe /platform:x64 /optimize+ /r:System.Windows.Forms.dll /r:System.Drawing.dll /out:"%~dp0EcoQoSMonitor.exe" "%~dp0EcoQoSMonitor.cs"

if %ERRORLEVEL% equ 0 (
    echo [SUCCESS] EcoQoSMonitor.exe generated successfully.
) else (
    echo [FAILED] Compilation failed.
)
pause
