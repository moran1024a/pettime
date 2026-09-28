@echo off
setlocal
rem Optional first argument: output directory. No argument: pause for double-click use.
set "RESULT=1"
if not defined PETTIME_UCRT64 set "PETTIME_UCRT64=C:\msys64\ucrt64"
set "PATH=%PETTIME_UCRT64%\bin;%PATH%"
set "BUILD_DIR=%~dp0build-windows"
if not "%~1"=="" set "BUILD_DIR=%~f1"

for %%T in (cmake.exe ninja.exe g++.exe) do (
    if not exist "%PETTIME_UCRT64%\bin\%%T" (
        echo ERROR: Missing "%PETTIME_UCRT64%\bin\%%T".
        echo Set PETTIME_UCRT64 to your MSYS2 UCRT64 directory.
        goto :finish
    )
)
if not exist "%PETTIME_UCRT64%\lib\cmake\Qt6\Qt6Config.cmake" (
    echo ERROR: Qt 6 is missing from "%PETTIME_UCRT64%".
    goto :finish
)

"%PETTIME_UCRT64%\bin\cmake.exe" -S "%~dp0." -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF "-DCMAKE_CXX_COMPILER=%PETTIME_UCRT64%\bin\g++.exe" "-DCMAKE_MAKE_PROGRAM=%PETTIME_UCRT64%\bin\ninja.exe" "-DCMAKE_PREFIX_PATH=%PETTIME_UCRT64%"
if errorlevel 1 goto :finish
"%PETTIME_UCRT64%\bin\cmake.exe" --build "%BUILD_DIR%" --target pettime --parallel
if errorlevel 1 goto :finish

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0package-windows.ps1" -BuildDir "%BUILD_DIR%" -Toolchain "%PETTIME_UCRT64%"
if errorlevel 1 goto :finish

set "RESULT=0"
echo.
echo Build and packaging succeeded: "%BUILD_DIR%\Pettime-windows-x64.zip"
echo Extract the ZIP, then run Pettime-windows-x64\pettime.exe.
echo Keep the complete extracted folder together.

:finish
if not "%RESULT%"=="0" echo ERROR: Build failed.
if "%~1"=="" pause
exit /b %RESULT%
