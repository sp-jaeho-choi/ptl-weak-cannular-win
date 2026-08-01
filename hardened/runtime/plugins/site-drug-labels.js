// 사이트별 약물 라벨 팩 (강화 빌드)
//
// v0.0.1 과 달리, 워크스테이션은 이 파일과 같은 이름의 `.sig` (Ed25519 분리
// 서명)를 신뢰 공개키로 검증한 뒤에만 불러온다. 서명이 없거나 맞지 않으면
// 로드하지 않고 설정 화면에 "거부"로 표시한다.
//
// 넘겨받는 컨텍스트에는 라벨 등록 함수만 있다. 등록 값은 길이와 키 형식이
// 검사된다.
//
// 서명 만들기:  node hardened/deploy/sign.js runtime/plugins/site-drug-labels.js

exports.name = 'site-drug-labels'
exports.version = '2.0.0'

exports.init = function (ctx) {
  ctx.registerDrugLabels({
    1: '모르핀 (1 mg/mL)',
    2: '펜타닐 (50 mcg/mL)',
    3: '인슐린 (1 U/mL)',
    4: '헤파린 (1000 U/mL)',
    5: '도파민 (400 mg/250 mL)',
    6: '교정용 (Calibration)'
  })

  ctx.registerAlarmLabels({
    1: '폐색 — 라인/필터 확인',
    2: '공기 감지 — 라인 프라이밍 필요',
    3: '배터리 부족 — 전원 연결',
    4: '주입 완료 / 저장통 비었음',
    5: '용량 상한 초과 — 처방 확인',
    9: '유량 오류 — 세트 재장착'
  })
}
