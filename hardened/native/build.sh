#!/usr/bin/env bash
# ===========================================================================
#  CANnula 강화(v0.0.2) 네이티브 구성요소 빌드
#
#    ./build.sh            현재 플랫폼용 (로직 검증)
#    ./build.sh --win      MinGW-w64 크로스 컴파일 (Windows 산출물)
#
#  v0.0.1 과 반대로 보호 기능을 모두 켠다.
#    -O2                        최적화
#    -fstack-protector-strong   스택 카나리   (MSVC /GS)
#    -D_FORTIFY_SOURCE=2        런타임 경계 검사
#    --dynamicbase              ASLR          (MSVC /DYNAMICBASE)
#    --nxcompat                 DEP           (MSVC /NXCOMPAT)
#    --high-entropy-va          64비트 고엔트로피 ASLR
#    -Wl,--no-insert-timestamp  재현 가능한 빌드
#
#  CAN 엔진은 별도 DLL 로 내보내지 않고 실행 파일에 정적 링크한다
#  (DLL 검색 경로 바꿔치기 공격면 제거).
# ===========================================================================
set -euo pipefail
cd "$(dirname "$0")"

OUT=../runtime/bin
mkdir -p "$OUT"

CFLAGS="-O2 -std=c99 -Wall -Wextra -Wformat=2 -Wformat-security \
        -fstack-protector-strong -D_FORTIFY_SOURCE=2"
SRC="cannula_bridge.c cannula_can.c cannula_sec.c"

if [[ "${1:-}" == "--win" ]]; then
  CC=${CC_WIN:-x86_64-w64-mingw32-gcc}
  command -v "$CC" >/dev/null || { echo "$CC 가 없다. mingw-w64 를 설치한다."; exit 1; }
  HARDEN="-Wl,--dynamicbase -Wl,--nxcompat -Wl,--high-entropy-va \
          -Wl,--no-insert-timestamp"

  echo "== CANnulaBridge.exe (강화, 엔진 정적 링크) =="
  # shellcheck disable=SC2086
  $CC $CFLAGS -o "$OUT/CANnulaBridge.exe" $SRC -lws2_32 $HARDEN
else
  CC=${CC:-gcc}
  echo "== CANnulaBridge (강화, 엔진 정적 링크) =="
  # shellcheck disable=SC2086
  $CC $CFLAGS -fPIE -pie -o "$OUT/CANnulaBridge" $SRC \
      -Wl,-z,relro,-z,now
fi

echo
ls -l "$OUT"
