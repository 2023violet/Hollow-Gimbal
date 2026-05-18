@echo off
setlocal

set "SCRIPT_DIR=%~dp0"
for %%I in ("%SCRIPT_DIR%..") do set "REPO_DIR=%%~fI"
set "BUNDLED_EXE=%REPO_DIR%\tools\clang-format\clang-format.exe"

if defined CLANG_FORMAT_EXE if exist "%CLANG_FORMAT_EXE%" (
    "%CLANG_FORMAT_EXE%" %*
    exit /b %ERRORLEVEL%
)

for /f "delims=" %%P in ('where clang-format 2^>nul') do (
    "%%~fP" %*
    exit /b %ERRORLEVEL%
)

for %%P in (
    "%BUNDLED_EXE%"
    "C:\Program Files\LLVM\bin\clang-format.exe"
    "C:\Program Files (x86)\LLVM\bin\clang-format.exe"
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\x64\bin\clang-format.exe"
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\Llvm\bin\clang-format.exe"
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-format.exe"
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Tools\Llvm\bin\clang-format.exe"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\x64\bin\clang-format.exe"
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Tools\Llvm\bin\clang-format.exe"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\Llvm\x64\bin\clang-format.exe"
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Tools\Llvm\bin\clang-format.exe"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\Llvm\x64\bin\clang-format.exe"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Tools\Llvm\bin\clang-format.exe"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Tools\Llvm\x64\bin\clang-format.exe"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Tools\Llvm\bin\clang-format.exe"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Tools\Llvm\x64\bin\clang-format.exe"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Professional\VC\Tools\Llvm\bin\clang-format.exe"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Tools\Llvm\x64\bin\clang-format.exe"
    "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Tools\Llvm\bin\clang-format.exe"
) do (
    if exist %%~P (
        "%%~P" %*
        exit /b %ERRORLEVEL%
    )
)

echo clang-format executable not found.
echo Expected one of:
echo   %BUNDLED_EXE%
echo   C:\Program Files\LLVM\bin\clang-format.exe
echo   Visual Studio 2019/2022 LLVM toolchains
echo   or any clang-format on PATH
exit /b 1
