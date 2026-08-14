@echo off
chcp 65001 >nul
REM ================================================
REM XnatServerManager - Windows EXE 打包脚本
REM 环境要求: Windows + Python 3.8+ (需含 tkinter)
REM ================================================
setlocal
cd /d "%~dp0"

where python >nul 2>&1
if errorlevel 1 (
    echo [错误] 未找到 Python，请先安装 Python 3.8+ 并勾选 "Add to PATH"
    pause
    exit /b 1
)

echo [1/2] 安装 PyInstaller ...
python -m pip install -U pyinstaller
if errorlevel 1 (
    echo [错误] PyInstaller 安装失败
    pause
    exit /b 1
)

echo [2/2] 开始打包 ...
python -m PyInstaller -F -w -n XnatServerManager xnat_server_manager.py
if errorlevel 1 (
    echo [错误] 打包失败
    pause
    exit /b 1
)

echo.
echo 打包完成: dist\XnatServerManager.exe
echo 拷贝该 exe 时需与 uid_protocol.py 无关（已内嵌，单文件独立运行）
pause
endlocal