@echo off
REM run_user.bat — 一键：导入用户数据 → 训练 → 测试  v5.1
chcp 65001 > nul
setlocal

echo.
echo ======================================================
echo    ECG 用户数据导入 + 训练 + 测试  v5.1
echo ======================================================
echo   训练: 内存合成 | 测试: data/test/ | 用户: data/user/
echo.

REM 步骤0：编译导入工具（如果还没编译）
if not exist "import_user.exe" (
    echo [编译] 正在编译导入工具...
    gcc -std=c99 -Wall -O3 -finput-charset=UTF-8 -fexec-charset=UTF-8 -o import_user.exe src/import_user.c
    if errorlevel 1 (
        echo [错误] 编译失败，请检查 gcc 是否可用
        pause
        exit /b 1
    )
)

REM 步骤1：导入用户数据
echo [步骤 1/2] 导入 data/user/ 中的用户数据...
"%~dp0import_user.exe"

REM 步骤2：训练 + 测试 (内建信号合成)
echo.
echo [步骤 2/2] 训练 + 测试...
"%~dp0ecg_test.exe"

echo.
pause
