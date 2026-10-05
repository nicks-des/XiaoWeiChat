@echo off
rem =====================================================================
rem @file build.bat
rem @brief 一键构建：CMake 配置（VS 18 2026 生成器 + vcpkg 工具链）+ 编译
rem 用法：build.bat [Debug^|Release]（默认 Debug）
rem =====================================================================
setlocal
set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=Debug"
call "%~dp0tools\scripts\env.bat"

if not exist "%VS_CMAKE%" (
    echo [ERROR] 未找到 VS 自带 CMake：%VS_CMAKE%
    exit /b 1
)

"%VS_CMAKE%" -S "%~dp0" -B "%~dp0build" -G "Visual Studio 18 2026" -A x64 ^
  -DCMAKE_TOOLCHAIN_FILE=%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake ^
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ^
  -DLINGXI_BOOST_ROOT=%LINGXI_BOOST_ROOT% ^
  -DLINGXI_MYSQL_ROOT=%LINGXI_MYSQL_ROOT% ^
  -DLINGXI_QT_DIR=%LINGXI_QT_DIR%
if errorlevel 1 exit /b 1

"%VS_CMAKE%" --build "%~dp0build" --config %CONFIG%
if errorlevel 1 exit /b 1

echo [BUILD OK] %CONFIG%
