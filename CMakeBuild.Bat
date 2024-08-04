@echo off

REM Update imgui submodule
echo Updating ImGui submodule...
git submodule update --remote thirdparty/imgui
if %errorlevel% neq 0 (
    echo Failed to update ImGui submodule.
    pause
    exit /b %errorlevel%
)

REM Clean previous build
echo Cleaning previous build...
rmdir /s /q build
if %errorlevel% neq 0 (
    echo Failed to clean previous build directory.
    pause
    exit /b %errorlevel%
)

REM Configure the build
echo Configuring the build...
cmake -B build
if %errorlevel% neq 0 (
    echo Failed to configure the build.
    pause
    exit /b %errorlevel%
)

echo Build configuration complete.

REM Pause to view debug
pause