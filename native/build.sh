#!/usr/bin/env bash
# ===========================================================================
#  CANnula 네이티브 구성요소 빌드
#
#    ./build.sh            현재 플랫폼용 (로직 검증 / 리눅스 랩)
#    ./build.sh --win      MinGW-w64 크로스 컴파일 (Windows 산출물)
#
#  Windows 산출물은 native/build.bat 과 같은 플래그를 쓴다
#  (ASLR / DEP / 스택 카나리 없음 — 의도된 구성).
# ===========================================================================
set -euo pipefail
cd "$(dirname "$0")"

OUT=../runtime/bin
mkdir -p "$OUT"

CFLAGS="-O0 -g -std=c99 -Wall -fno-stack-protector"

if [[ "${1:-}" == "--win" ]]; then
  CC=x86_64-w64-mingw32-gcc
  command -v $CC >/dev/null || { echo "$CC 가 없다. mingw-w64 를 설치한다."; exit 1; }
  HARDEN_OFF="-Wl,--disable-dynamicbase -Wl,--disable-nxcompat -Wl,--disable-reloc-section"

  echo "== cannula_can.dll =="
  $CC $CFLAGS -fno-pic -shared -o "$OUT/cannula_can.dll" cannula_can.c \
      -Wl,--out-implib,cannula_can.lib $HARDEN_OFF \
      -Wl,--image-base=0x0000000180000000

  echo "== CANnulaBridge.exe =="
  $CC $CFLAGS -fno-pic -o "$OUT/CANnulaBridge.exe" cannula_bridge.c \
      -L. -lcannula_can -lws2_32 $HARDEN_OFF \
      -Wl,--image-base=0x0000000140000000
else
  CC=${CC:-gcc}
  echo "== libcannula_can.so =="
  $CC $CFLAGS -fPIC -shared -o "$OUT/libcannula_can.so" cannula_can.c

  echo "== CANnulaBridge =="
  $CC $CFLAGS -no-pie -o "$OUT/CANnulaBridge" cannula_bridge.c \
      -L"$OUT" -lcannula_can -Wl,-rpath,'$ORIGIN'
fi

echo
ls -l "$OUT"
