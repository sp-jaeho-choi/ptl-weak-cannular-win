@echo off
REM ===========================================================================
REM  CANnula 강화(v0.0.2) 네이티브 구성요소 빌드 (Windows / MinGW-w64 x86_64)
REM
REM    CANnulaBridge.exe    CAN 게이트웨이 (엔진 정적 링크, DLL 없음)
REM
REM  MSYS2 에서:  pacman -S mingw-w64-x86_64-gcc
REM
REM  v0.0.1 과 반대로 보호 기능을 모두 켠다.
REM    -O2                        최적화
REM    -fstack-protector-strong   스택 카나리   (MSVC /GS)
REM    -D_FORTIFY_SOURCE=2        런타임 경계 검사
REM    --dynamicbase              ASLR          (MSVC /DYNAMICBASE)
REM    --nxcompat                 DEP           (MSVC /NXCOMPAT)
REM    --high-entropy-va          64비트 고엔트로피 ASLR
REM ===========================================================================
setlocal
cd /d "%~dp0"

set OUT=..\runtime\bin
if not exist "%OUT%" mkdir "%OUT%"

set CFLAGS=-O2 -std=c99 -Wall -Wextra -Wformat=2 -Wformat-security -fstack-protector-strong -D_FORTIFY_SOURCE=2
set HARDEN=-Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va -Wl,--no-insert-timestamp

echo == CANnulaBridge.exe (강화) ==
gcc %CFLAGS% -o "%OUT%\CANnulaBridge.exe" ^
    cannula_bridge.c cannula_can.c cannula_sec.c -lws2_32 %HARDEN%
if errorlevel 1 goto :fail

echo.
echo 산출물: %OUT%
dir /b "%OUT%"
echo.
echo 자가진단:
"%OUT%\CANnulaBridge.exe" --selftest
goto :eof

:fail
echo 빌드 실패. MinGW-w64 gcc 가 PATH 에 있는지 확인한다.
exit /b 1
