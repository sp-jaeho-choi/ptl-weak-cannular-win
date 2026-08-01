// CANnula Clinical Workstation v0.0.2 (강화) — 렌더러
//
// 여기에는 Node 가 없다. window.cannula (프리로드가 contextBridge 로 열어 준
// 좁은 API) 만 쓸 수 있다.
//
// 규칙: 장치·서버·사용자에서 온 문자열은 절대 innerHTML 로 넣지 않는다.
//       DOM 은 textContent 와 createElement 로만 만든다.
'use strict'

const api = window.cannula
const $ = (id) => document.getElementById(id)

let RT = {}
let S = {}
let session = null
let DRUGS = []
let LIMITS = { hardMax: 500, softMax: 200, hardMin: 1, maxVtbi: 9999, maxBolus: 20 }
let frames = []
let busPaused = false
let events = []
let alarms = []
let rateHistory = []

// --- DOM 만들기 보조 (textContent 만 쓴다) ----------------------------------
function el(tag, text, className) {
  const n = document.createElement(tag)
  if (text !== undefined && text !== null) n.textContent = String(text)
  if (className) n.className = className
  return n
}

function fillRows(tbody, rows) {
  tbody.replaceChildren()
  for (const cells of rows) {
    const tr = document.createElement('tr')
    for (const c of cells) {
      const td = el('td', c.text, c.cls)
      tr.appendChild(td)
    }
    tbody.appendChild(tr)
  }
}

function setMsg(id, text, kind) {
  const n = $(id)
  n.textContent = text || ''
  n.className = 'msg' + (kind ? ' ' + kind : '')
}

// --- 이벤트 로그 ------------------------------------------------------------
function logEvent(kind, text) {
  const e = { t: new Date().toISOString(), kind, text: String(text) }
  events.unshift(e)
  if (events.length > 800) events.pop()
  api.logEvent(kind, text)
  if ($('view-events').classList.contains('active')) renderEvents()
}

function renderEvents() {
  fillRows($('evRows'), events.slice(0, 300).map((e) => [
    { text: e.t.slice(11, 19), cls: 'mono' },
    { text: e.kind },
    { text: e.text }
  ]))
}

// --- 로그인 ----------------------------------------------------------------
async function doLogin() {
  const user = $('lgUser').value.trim()
  const pass = $('lgPass').value
  $('lgErr').textContent = ''
  const r = await api.login(user, pass)
  if (r && r.error) { $('lgErr').textContent = r.error; return }
  session = r
  $('lgPass').value = ''
  $('login').classList.add('hide')
  applySession()
  logEvent('auth', `로그인: ${session.user} (${session.role})`)
  await afterLogin()
}

function applySession() {
  if (!session) return
  $('hdrWho').replaceChildren(el('b', session.user), document.createTextNode(` · ${session.role}`))
  // 권한이 낮으면 메뉴를 숨긴다. 실제 차단은 메인 프로세스가 한다.
  document.querySelectorAll('#nav button[data-level]').forEach((b) => {
    b.hidden = session.level < Number(b.dataset.level)
  })
}

$('lgGo').addEventListener('click', doLogin)
$('lgPass').addEventListener('keydown', (e) => { if (e.key === 'Enter') doLogin() })
$('hdrLogout').addEventListener('click', async () => {
  await api.logout()
  session = null
  $('hdrWho').replaceChildren()
  $('login').classList.remove('hide')
})

// --- 화면 전환 -------------------------------------------------------------
document.querySelectorAll('#nav button').forEach((b) => {
  b.addEventListener('click', () => {
    document.querySelectorAll('#nav button').forEach((x) => x.classList.remove('active'))
    b.classList.add('active')
    document.querySelectorAll('section.view').forEach((v) => v.classList.remove('active'))
    $('view-' + b.dataset.view).classList.add('active')
    if (b.dataset.view === 'events') renderEvents()
    if (b.dataset.view === 'about') renderAbout()
    if (b.dataset.view === 'diag') loadServiceLists()
  })
})

// --- 라벨 -----------------------------------------------------------------
function drugLabel(id) {
  const custom = RT.drugLabels || {}
  if (custom[id]) return custom[id]
  const d = DRUGS.find((x) => x.id === Number(id))
  return d ? d.name : `ID ${id}`
}

function alarmLabel(code, fallback) {
  const custom = RT.alarmLabels || {}
  if (custom[code]) return custom[code]
  return fallback || `코드 ${code}`
}

// --- 상태 반영 -------------------------------------------------------------
function applyState(s) {
  S = s || {}

  $('tRate').textContent = S.rate
  $('tSet').textContent  = S.setpoint
  $('tDose').textContent = S.dose
  $('tBatt').textContent = S.battery
  $('battBar').style.width = Math.max(0, Math.min(100, Number(S.battery) || 0)) + '%'
  $('tInf').textContent  = S.infused
  $('tVtbi').textContent = S.vtbi
  $('tRem').textContent  = S.remaining
  $('tState').textContent = S.stateLabel
  $('tDrug').textContent = `${drugLabel(S.drugId)} (ID ${S.drugId})`

  const pct = S.vtbi > 0 ? Math.min(100, (S.infused / S.vtbi) * 100) : 0
  $('volBar').style.width = pct + '%'

  $('dersState').textContent = S.dersEnabled ? '작동 중' : '해제됨'
  $('dersSoft').textContent  = S.dersSoftMax
  $('dersHard').textContent  = S.dersHardMax
  $('dersIntegrity').textContent = S.safetyIntegrity ? '정상' : '실패 (안전 측으로 전환)'
  $('dersAuth').textContent  = S.authLevel

  const sec = S.sec || {}
  $('secOk').textContent      = sec.accepted
  $('secMac').textContent     = sec.mac
  $('secReplay').textContent  = sec.replay
  $('secStale').textContent   = sec.stale
  $('secForm').textContent    = sec.form
  $('secDropped').textContent = S.framesDropped

  const banner = $('alarmBanner')
  if (S.alarmCode) {
    banner.classList.add('show')
    // textContent 로만 넣는다 — 장치 문자열이 마크업으로 해석되지 않는다.
    $('alarmMain').textContent = '⚠ ' + alarmLabel(S.alarmCode, S.alarm)
    $('alarmSub').textContent =
      `펌프 ${S.deviceId} · 상태 ${S.stateLabel} · 속도 ${S.rate} mL/h`
  } else {
    banner.classList.remove('show')
    $('alarmMain').textContent = ''
    $('alarmSub').textContent = ''
  }

  $('hdrDevice').textContent = '장치 ' + (S.deviceId || '—')
  const hs = $('hdrSec')
  hs.textContent = S.hasKey ? `인증 ok ${sec.accepted || 0}` : '인증 키 없음'
  hs.className = 'badge ' + (S.hasKey ? 'on' : 'off')

  $('statusLine').textContent =
    `${drugLabel(S.drugId)} ${S.rate} mL/h · ${S.infused}/${S.vtbi} mL · ${S.stateLabel}`
  $('frameStat').textContent =
    `rx ${S.framesRx} · tx ${S.framesTx} · 폐기 ${S.framesDropped}`

  $('svcOut').textContent = S.serviceText || '—'

  rateHistory.push(Number(S.rate) || 0)
  if (rateHistory.length > 300) rateHistory.shift()
  drawChart()
}

function drawChart() {
  const c = $('chart')
  const ctx = c.getContext('2d')
  const w = c.width, h = c.height
  ctx.clearRect(0, 0, w, h)
  ctx.strokeStyle = '#e2e6ea'
  ctx.lineWidth = 1
  for (let i = 0; i <= 4; i++) {
    const y = (h - 20) * i / 4 + 10
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke()
  }
  if (rateHistory.length < 2) return
  const max = Math.max(50, ...rateHistory)
  ctx.strokeStyle = '#16736b'
  ctx.lineWidth = 2
  ctx.beginPath()
  rateHistory.forEach((v, i) => {
    const x = i / (rateHistory.length - 1) * (w - 4) + 2
    const y = h - 10 - (v / max) * (h - 20)
    i ? ctx.lineTo(x, y) : ctx.moveTo(x, y)
  })
  ctx.stroke()
  ctx.fillStyle = '#66717f'
  ctx.font = '10px monospace'
  ctx.fillText(String(max) + ' mL/h', 4, 10)
}

// --- 게이트웨이 이벤트 -----------------------------------------------------
api.on('bridge:state', (s) => applyState(s))

api.on('bridge:status', (st) => {
  const b = $('hdrBridge')
  b.textContent = '게이트웨이 ' + (st && st.up ? '연결' : '끊김')
  b.className = 'badge ' + (st && st.up ? 'on' : 'off')
  $('stBridge').textContent = st && st.up
    ? `연결됨 (127.0.0.1:${st.port}, 토큰 인증)` : '끊김'
})

api.on('bridge:hello', (h) => {
  $('stTransport').value = h.transport === 'slcan' ? 'slcan' : 'sim'
  if (h.bitrate) $('stBitrate').value = String(h.bitrate)
  logEvent('bus', `게이트웨이 연결: ${h.engine}`)
})

api.on('bridge:log', (msg) => logEvent('bus', msg))

api.on('bridge:frame', (f) => {
  if (busPaused) return
  f.at = new Date()
  frames.unshift(f)
  if (frames.length > 300) frames.pop()

  if (f.dir === 'rx' && f.note && f.note.indexOf('폐기') === 0) {
    // 폐기된 프레임은 침입 신호일 수 있으므로 이벤트로 남긴다.
    logEvent('security', `프레임 ${f.note} (ID 0x${Number(f.id).toString(16)})`)
  }
  if ($('view-diag').classList.contains('active')) renderBus()
})

function renderBus() {
  $('busCount').textContent = `${frames.length} 프레임`
  fillRows($('busRows'), frames.slice(0, 250).map((f) => {
    const clock = f.at
      ? f.at.toLocaleTimeString('en-GB') + '.' + String(f.at.getMilliseconds()).padStart(3, '0')
      : '—'
    return [
      { text: clock, cls: 'mono' },
      { text: f.dir === 'rx' ? '수신' : '송신' },
      { text: '0x' + Number(f.id).toString(16).toUpperCase().padStart(3, '0'), cls: 'mono' },
      { text: f.dlc },
      { text: f.note || '' }
    ]
  }))
}

$('busPause').addEventListener('click', () => {
  busPaused = !busPaused
  $('busPause').textContent = busPaused ? '재개' : '일시정지'
})
$('busClear').addEventListener('click', () => { frames = []; renderBus() })

// --- 알람 -----------------------------------------------------------------
let lastAlarmCode = 0
setInterval(() => {
  if (S.alarmCode && S.alarmCode !== lastAlarmCode) {
    alarms.unshift({ t: new Date(), code: S.alarmCode, label: alarmLabel(S.alarmCode, S.alarm), state: S.stateLabel })
    if (alarms.length > 200) alarms.pop()
    renderAlarms()
    logEvent('alarm', `알람 ${S.alarmCode}`)
  }
  lastAlarmCode = S.alarmCode || 0
}, 500)

function renderAlarms() {
  fillRows($('alarmRows'), alarms.slice(0, 200).map((a) => [
    { text: a.t.toLocaleTimeString('en-GB'), cls: 'mono' },
    { text: a.code, cls: 'mono' },
    { text: a.label },
    { text: a.state }
  ]))
}

$('btnAck').addEventListener('click', async () => {
  const r = await api.ackAlarm(S.alarmCode || 0)
  setMsg('alarmMsg', r && r.error ? r.error : '확인 요청을 보냈습니다.', r && r.error ? 'err' : 'ok')
  if (!(r && r.error)) logEvent('alarm', '알람 확인')
})
$('btnAlarmClear').addEventListener('click', () => { alarms = []; renderAlarms() })

// --- 대시보드 제어 --------------------------------------------------------
$('btnStart').addEventListener('click', async () => {
  const r = await api.control(1, 0)
  setMsg('dashMsg', r && r.error ? r.error : '주입을 시작했습니다.', r && r.error ? 'err' : 'ok')
  if (!(r && r.error)) logEvent('rx', '주입 시작')
})
$('btnStop').addEventListener('click', async () => {
  const r = await api.control(2, 0)
  setMsg('dashMsg', r && r.error ? r.error : '주입을 정지했습니다.', r && r.error ? 'err' : 'ok')
  if (!(r && r.error)) logEvent('rx', '주입 정지')
})
$('btnBolus').addEventListener('click', async () => {
  const v = Number($('bolusVol').value)
  if (!Number.isInteger(v) || v < 1 || v > LIMITS.maxBolus) {
    setMsg('dashMsg', `볼루스는 1–${LIMITS.maxBolus} mL 범위여야 합니다.`, 'err')
    return
  }
  const r = await api.control(3, v)
  setMsg('dashMsg', r && r.error ? r.error : `볼루스 ${v} mL 를 요청했습니다.`, r && r.error ? 'err' : 'ok')
  if (!(r && r.error)) logEvent('rx', `볼루스 ${v} mL`)
})

// --- 주입 설정 ------------------------------------------------------------
function fillDrugs() {
  const sel = $('rxDrug')
  sel.replaceChildren()
  for (const d of DRUGS) {
    const o = document.createElement('option')
    o.value = String(d.id)
    o.textContent = drugLabel(d.id)
    sel.appendChild(o)
  }
  fillRows($('drugRows'), DRUGS.map((d) => [
    { text: d.id, cls: 'mono' },
    { text: drugLabel(d.id) },
    { text: (d.concMcgPerMl / 1000).toFixed(3) + ' mg/mL', cls: 'mono' },
    { text: d.min, cls: 'mono' },
    { text: d.max, cls: 'mono' }
  ]))
}

function localVerdict(rate, vtbi, drugId, weight) {
  const d = DRUGS.find((x) => x.id === drugId)
  if (!d) return { cls: 'v-block', text: '차단: 등록되지 않은 약물' }
  if (weight < 1 || weight > 250) return { cls: 'v-block', text: '차단: 체중이 허용 범위 밖' }
  if (vtbi < 1 || vtbi > LIMITS.maxVtbi) return { cls: 'v-block', text: '차단: 총 주입량이 허용 범위 밖' }
  const hardMax = Math.min(d.max, LIMITS.hardMax)
  const hardMin = Math.max(d.min, LIMITS.hardMin)
  if (rate > hardMax) return { cls: 'v-block', text: `차단: 절대 상한 ${hardMax} mL/h 초과` }
  if (rate < hardMin) return { cls: 'v-block', text: `차단: 하한 ${hardMin} mL/h 미달` }
  if (rate > LIMITS.softMax) return { cls: 'v-warn', text: '경고 상한 초과 — 처방 재확인 필요' }
  return { cls: 'v-pass', text: '통과' }
}

function rxRecalc() {
  const rate = Number($('rxRate').value)
  const vtbi = Number($('rxVtbi').value)
  const wt   = Number($('rxWeight').value)
  const did  = Number($('rxDrug').value)
  const d = DRUGS.find((x) => x.id === did)

  if (d && wt >= 1) {
    const mcgPerHour = rate * d.concMcgPerMl
    const perKgMin = mcgPerHour / 60 / wt
    const hours = rate > 0 ? vtbi / rate : Infinity
    $('rxCalc').textContent =
      `${(mcgPerHour / 1000).toFixed(2)} mg/h · ${perKgMin.toFixed(2)} mcg/kg/min · ` +
      `예상 소요 ${isFinite(hours) ? hours.toFixed(1) + ' h' : '—'}`
  } else {
    $('rxCalc').textContent = '—'
  }

  const v = localVerdict(rate, vtbi, did, wt)
  const n = $('rxVerdict')
  n.replaceChildren(el('span', '가드레일: ' + v.text, v.cls))
}

;['rxRate', 'rxVtbi', 'rxWeight'].forEach((id) => $(id).addEventListener('input', rxRecalc))
$('rxDrug').addEventListener('change', rxRecalc)

$('rxSend').addEventListener('click', async () => {
  const rate = Number($('rxRate').value)
  const vtbi = Number($('rxVtbi').value)
  const drug = Number($('rxDrug').value)
  const weight = Number($('rxWeight').value)
  rxRecalc()
  const r = await api.setRate(rate, vtbi, drug, weight)
  if (r && r.error) {
    setMsg('rxMsg', r.error, 'err')
    logEvent('rx', `SET_RATE 거부 (${rate} mL/h): ${r.error}`)
    return
  }
  setMsg('rxMsg', r.warn
    ? `전송했습니다 — ${r.verdict}`
    : '전송했습니다. 가드레일 판정: ' + r.verdict, r.warn ? 'err' : 'ok')
  logEvent('rx', `SET_RATE ${drugLabel(drug)} ${rate} mL/h VTBI ${vtbi} mL / ${weight} kg (${r.verdict})`)
})

// --- 환자 정보 ------------------------------------------------------------
function currentPatient() {
  return {
    id: $('ptId').value, name: $('ptName').value, bed: $('ptBed').value,
    weight: Number($('ptWeight').value), height: Number($('ptHeight').value),
    allergy: $('ptAllergy').value
  }
}

function renderPatient() {
  const p = currentPatient()
  $('ptCard').replaceChildren(
    el('b', p.name || '(미배정)'),
    document.createTextNode(` (ID ${p.id}) · ${p.bed} · ${p.weight} kg / ${p.height} cm · 알레르기: ${p.allergy}`)
  )
}

$('ptSave').addEventListener('click', async () => {
  const p = currentPatient()
  const r = await api.patientSave(p)
  if (r && r.error) { setMsg('ptMsg', r.error, 'err'); return }
  $('rxWeight').value = String(p.weight)
  rxRecalc()
  renderPatient()
  setMsg('ptMsg', '저장했습니다.', 'ok')
  // 이벤트에는 개인정보를 넣지 않는다.
  logEvent('patient', '환자 배정 정보를 갱신했습니다.')
})

// --- 진단 -----------------------------------------------------------------
async function loadServiceLists() {
  const sl = await api.serviceCommands()
  const sel = $('svcCmd')
  sel.replaceChildren()
  if (sl && sl.commands) {
    for (const c of sl.commands) {
      const o = document.createElement('option')
      o.value = c; o.textContent = c
      sel.appendChild(o)
    }
  }
  const dl = await api.diagTools()
  const dsel = $('diagTool')
  dsel.replaceChildren()
  if (dl && dl.tools) {
    for (const t of dl.tools) {
      const o = document.createElement('option')
      o.value = t; o.textContent = t
      dsel.appendChild(o)
    }
  }
}

$('svcGo').addEventListener('click', async () => {
  const cmd = $('svcCmd').value
  const r = await api.serviceRun(cmd)
  setMsg('svcMsg', r && r.error ? r.error : '전송했습니다.', r && r.error ? 'err' : 'ok')
  if (!(r && r.error)) logEvent('service', `서비스 명령: ${cmd}`)
})

$('diagGo').addEventListener('click', async () => {
  const tool = $('diagTool').value
  $('diagOut').textContent = '실행 중…'
  const r = await api.diagRun(tool)
  $('diagOut').textContent = r && r.error
    ? '오류: ' + r.error
    : `${r.tool} (종료 코드 ${r.exitCode})\n\n${r.output || '(출력 없음)'}`
  logEvent('service', `진단 도구 실행: ${tool}`)
})

// --- 이벤트 로그 저장 -----------------------------------------------------
$('evSave').addEventListener('click', async () => {
  const r = await api.logExport(events.slice().reverse())
  setMsg('evMsg', r && r.error ? r.error : `저장: ${r.name}`, r && r.error ? 'err' : 'ok')
})

$('evList').addEventListener('click', async () => {
  const r = await api.logList()
  if (r && r.error) { $('evFiles').textContent = r.error; return }
  $('evFiles').textContent = (r.files || []).length
    ? r.files.map((f) => `${String(f.size).padStart(9)}  ${f.name}`).join('\n')
    : '(없음)'
})

// --- 설정 -----------------------------------------------------------------
$('stApply').addEventListener('click', async () => {
  const mode = $('stTransport').value
  const serial = $('stSerial').value.trim()
  const bitrate = Number($('stBitrate').value)
  const r = await api.transport(mode, serial, bitrate)
  setMsg('stMsg', r && r.error ? r.error : '적용했습니다.', r && r.error ? 'err' : 'ok')
  if (!(r && r.error)) logEvent('config', `전송 방식 변경: ${mode} ${bitrate}kbps`)
})

// --- 업데이트 -------------------------------------------------------------
$('upCheck').addEventListener('click', async () => {
  $('upOut').textContent = '확인 중…'
  const r = await api.updateCheck()
  if (r && r.error) {
    $('upOut').textContent = '오류: ' + r.error
    $('upApply').disabled = true
    return
  }
  $('upOut').textContent =
    `새 버전 ${r.version}\n서명자 ${r.signer}\n서버 ${r.base}\n\n${r.notes || ''}`
  $('upApply').disabled = false
})

$('upApply').addEventListener('click', async () => {
  $('upOut').textContent = '검증하며 내려받는 중…'
  const r = await api.updateApply()
  $('upOut').textContent = r && r.error
    ? '오류: ' + r.error
    : `${r.message}\n\n파일: ${r.saved}\n서명자: ${r.signer}`
  if (!(r && r.error)) logEvent('update', `업데이트 패키지 검증 완료: ${r.saved}`)
})

// --- 정보 -----------------------------------------------------------------
function renderAbout() {
  const pl = (RT.plugins || [])
    .map((p) => `${p.ok ? 'OK  ' : '거부'} ${p.file} ${p.ok ? '(서명자 ' + p.signer + ')' : '— ' + (p.error || '')}`)
    .join('\n      ') || '(없음)'
  $('aboutOut').textContent =
`제품            CANnula Clinical Workstation
버전            ${RT.version} (강화 빌드)
플랫폼          ${RT.platform}
사이트          ${(RT.site || {}).name || '-'} / ${(RT.site || {}).ward || '-'}
전송            ${RT.transport}
등록 계정 수    ${RT.users}
신뢰 공개키     ${(RT.trustedKeys || []).join(', ') || '(없음)'}
확장            ${pl}
로그 폴더       ${RT.logDir}
업데이트 서버   ${RT.updateBase || '(미설정)'}
CAN 인증        CANnula-SEC v1 (HMAC-SHA256 + 시퀀스 + 신선도)
가드레일        절대 ${LIMITS.hardMax} / 경고 ${LIMITS.softMax} mL/h (빌드 고정)
대상 펌웨어     CANnula v0.0.2-secure`
}

// --- 기동 -----------------------------------------------------------------
async function afterLogin() {
  const d = await api.drugs()
  if (d && d.drugs) { DRUGS = d.drugs; LIMITS = d.limits }
  $('bolusMax').textContent = String(LIMITS.maxBolus)
  fillDrugs()
  rxRecalc()

  const p = await api.patientLoad()
  if (p && !p.error) {
    $('ptId').value = p.id || ''
    $('ptName').value = p.name || ''
    $('ptBed').value = p.bed || ''
    $('ptWeight').value = p.weight || ''
    $('ptHeight').value = p.height || ''
    $('ptAllergy').value = p.allergy || ''
    if (p.weight) $('rxWeight').value = String(p.weight)
  }
  renderPatient()
  await loadServiceLists()

  const s0 = await api.bridgeState()
  if (s0 && s0.deviceId !== undefined) applyState(s0)
}

;(async function boot() {
  RT = await api.runtime()
  $('hdrVer').textContent = 'v' + RT.version
  $('upCur').textContent = RT.version
  $('upBase').textContent = RT.updateBase || '(미설정)'

  const site = RT.site || {}
  $('siteInfo').replaceChildren(
    el('div', site.name || '—'),
    el('div', `${site.ward || ''} · ${site.workstation || ''}`)
  )

  $('stPlugins').textContent = (RT.plugins || []).map((p) =>
    `${p.ok ? 'OK  ' : '거부'} ${p.file}  ${p.name} ${p.version}` +
    (p.ok ? `  (서명자 ${p.signer})` : `  — ${p.error || ''}`)
  ).join('\n') || '(없음)'

  const bs = await api.bridgeStatus()
  if (bs) {
    const b = $('hdrBridge')
    b.textContent = '게이트웨이 ' + (bs.up ? '연결' : '끊김')
    b.className = 'badge ' + (bs.up ? 'on' : 'off')
  }

  // 이전 세션이 살아 있으면 복원한다 (세션은 메인 프로세스가 들고 있다).
  const sess = await api.session()
  if (sess) {
    session = sess
    $('login').classList.add('hide')
    applySession()
    await afterLogin()
  }
})()
