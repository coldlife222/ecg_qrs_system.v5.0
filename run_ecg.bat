@echo off
REM run_ecg.bat — ECG分析系统启动菜单 v5.1
chcp 65001 > nul
setlocal

:menu
cls
echo.
echo ======================================================
echo    ECG QRS检测与心律失常分析系统  v5.1
echo ======================================================
echo.
echo    [1] 训练 + 测试 + 用户评估 (内建合成，一键完成)
echo    [2] 导入用户数据 (data/user/)
echo    [3] 交互式分析程序
echo    [0] 退出
echo.
set /p choice="  请选择 [0-3]: "

if "%choice%"=="1" goto test
if "%choice%"=="2" goto import
if "%choice%"=="3" goto interactive
if "%choice%"=="0" goto end
goto menu

:import
echo.
echo [正在导入用户数据...]
if not exist "import_user.exe" (
    echo [编译] 正在编译导入工具...
    gcc -std=c99 -Wall -O3 -finput-charset=UTF-8 -fexec-charset=UTF-8 -o import_user.exe src/import_user.c
)
"%~dp0import_user.exe"
goto menu

:test
echo.
echo [训练 + 测试 + 用户评估...]
echo   训练: 内存合成 8000 条
echo   测试: data/test/ (20 条)
echo   用户: data/user/ (独立清单)
echo.
"%~dp0ecg_test.exe"
goto menu

:interactive
echo.
echo [正在启动交互式分析...]
"%~dp0ecg_interactive.exe"
goto menu

:end
echo.
echo 再见！
exit /b 0
