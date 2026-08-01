@echo off
REM ===========================================================================
REM  CANnula 네이티브 구성요소 빌드 (Windows / MinGW-w64 x86_64)
REM
REM    cannula_can.dll      CAN 엔진 (프레임 파서 + 상태 + DERS + 펌프 시뮬레이터)
REM    CANnulaBridge.exe    CAN 게이트웨이 서비스 (엔진 DLL 을 이름으로 로드)
REM
REM  MSYS2 에서:  pacman -S mingw-w64-x86_64-gcc
REM  그 뒤 MinGW64 셸이나 PATH 에 gcc 를 올린 cmd 에서 이 스크립트를 실행한다.
REM
REM  플래그 메모 (의도된 구성):
REM    -O0 -g                최적화를 끄고 심볼을 남긴다 (디버거/CE 실습용)
REM    -fno-stack-protector  스택 카나리 없음
REM    --disable-dynamicbase ASLR 없음 → 이미지 베이스 고정, 오프셋 재현 가능
REM    --disable-nxcompat    DEP 없음  → 데이터 영역 코드 실행 가능
REM    --disable-reloc-section  재배치 정보 제거
REM
REM  ⚠ 교육용 취약 빌드. 실제 제품에 이 플래그를 쓰지 말 것.
REM ===========================================================================
setlocal
cd /d "%~dp0"

set OUT=..\runtime\bin
if not exist "%OUT%" mkdir "%OUT%"

set CFLAGS=-O0 -g -std=c99 -Wall -fno-stack-protector -fno-pic
set HARDEN_OFF=-Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--disable-reloc-section

echo == cannula_can.dll ==
gcc %CFLAGS% -shared -o "%OUT%\cannula_can.dll" cannula_can.c ^
    -Wl,--out-implib,cannula_can.lib %HARDEN_OFF% ^
    -Wl,--image-base=0x0000000180000000
if errorlevel 1 goto :fail

echo == CANnulaBridge.exe ==
gcc %CFLAGS% -o "%OUT%\CANnulaBridge.exe" cannula_bridge.c ^
    -L. -lcannula_can -lws2_32 %HARDEN_OFF% ^
    -Wl,--image-base=0x0000000140000000
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
