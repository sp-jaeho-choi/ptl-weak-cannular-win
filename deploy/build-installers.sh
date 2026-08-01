#!/usr/bin/env bash
# ===========================================================================
#  CANnula Workstation — 버전별 Windows 설치 파일 빌드
#
#    0.0.1  CANnula-Workstation-WEAK       침투 테스트 대상 (취약)
#    0.0.2  CANnula-Workstation-HARDENED   방어 대조군 (강화)
#
#  각 버전마다 NSIS 설치본(.exe) 과 포터블 zip 을 만든다.
#
#    ./deploy/build-installers.sh              둘 다
#    ./deploy/build-installers.sh weak         0.0.1 만
#    ./deploy/build-installers.sh hardened     0.0.2 만
#
#  ⚠ 코드 서명은 하지 않는다.
#     0.0.1 은 "서명·검증 없는 배포"가 실습의 공격 표면이므로 의도된 설정이다.
#     0.0.2 는 확장·업데이트에 대한 자체 서명 검증(Ed25519)을 구현했지만,
#     설치 파일 자체의 Authenticode 서명은 인증서가 없어 생략한다.
#
#  네이티브 산출물은 이 스크립트가 먼저 빌드한다 (MinGW-w64 필요).
#  MinGW 가 없으면 이미 빌드된 runtime/bin 의 산출물을 그대로 쓴다.
#
#  Windows 에서 실행할 때는 MSYS2/Git Bash 에서 이 스크립트를 쓰거나,
#  각 폴더에서 native\build.bat → npm run dist:win 을 직접 실행한다.
#  리눅스에서 NSIS 설치본을 만들려면 wine 이 필요하다 (zip 은 wine 없이도 된다).
# ===========================================================================
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

TARGET="${1:-both}"
CC_WIN=${CC_WIN:-x86_64-w64-mingw32-gcc}

have_mingw() { command -v "$CC_WIN" >/dev/null 2>&1; }

build_native() {
  local dir="$1" label="$2"
  echo ">> [$label] 네이티브 빌드"
  if have_mingw; then
    (cd "$dir" && bash native/build.sh --win)
  else
    echo "   $CC_WIN 없음 — 기존 runtime/bin 산출물을 사용한다"
    ls -l "$dir/runtime/bin" || true
  fi
}

build_app() {
  local dir="$1" label="$2"
  echo ">> [$label] Electron 패키징"
  cd "$dir"
  [ -d node_modules ] || npm install --no-audit --no-fund
  npx electron-builder --win
  cd "$ROOT"
}

collect() {
  local dir="$1" label="$2"
  echo ">> [$label] 산출물"
  ls -lh "$dir/dist" 2>/dev/null | grep -E "\.exe$|\.zip$" || echo "   (없음)"
}

if [ "$TARGET" = "both" ] || [ "$TARGET" = "weak" ]; then
  echo "=========================================================="
  echo " 0.0.1  CANnula-Workstation-WEAK (취약 — 침투 테스트 대상)"
  echo "=========================================================="
  build_native "$ROOT" "weak"
  build_app    "$ROOT" "weak"
  collect      "$ROOT" "weak"
  echo
fi

if [ "$TARGET" = "both" ] || [ "$TARGET" = "hardened" ]; then
  echo "=========================================================="
  echo " 0.0.2  CANnula-Workstation-HARDENED (강화 — 방어 대조군)"
  echo "=========================================================="
  build_native "$ROOT/hardened" "hardened"
  echo ">> [hardened] 확장 서명"
  (cd "$ROOT/hardened" && node deploy/sign.js runtime/plugins/site-drug-labels.js)
  build_app    "$ROOT/hardened" "hardened"
  collect      "$ROOT/hardened" "hardened"
  echo
fi

echo "=========================================================="
echo " 완료"
echo "=========================================================="
echo " 취약  : dist/"
echo " 강화  : hardened/dist/"
echo
echo " 두 빌드의 PE 하드닝 차이 확인:"
echo "   objdump -p <exe> | grep DllCharacteristics"
echo "     0x0000  = ASLR/DEP 없음  (0.0.1 게이트웨이)"
echo "     0x0160  = HIGH_ENTROPY_VA + DYNAMIC_BASE + NX_COMPAT  (0.0.2)"
