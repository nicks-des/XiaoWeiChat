@echo off
rem =====================================================================
rem @file test.bat
rem @brief 一键测试：运行 CTest 全量单元测试（先 build.bat）
rem 用法：test.bat [Debug^|Release]（默认 Debug）
rem =====================================================================
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
call "%~dp0tools\scripts\env.bat"

if not exist "%VS_CMAKE%" (
    echo [ERROR] 未找到 VS 自带 CMake：%VS_CMAKE%
    exit /b 1
)

"%VS_CMAKE%" --build "%~dp0build" --config %CONFIG% 2>nul 1>nul
"%VS_CMAKE%" --build "%~dp0build" --config %CONFIG% --target RUN_TESTS
if errorlevel 1 exit /b 1

echo [TEST OK] %CONFIG%
