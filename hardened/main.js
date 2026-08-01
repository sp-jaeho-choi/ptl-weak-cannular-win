// CANnula Clinical Workstation v0.0.2 (강화) — Electron main process.
//
// v0.0.1(취약)의 같은 파일과 기능은 같지만 다음을 지킨다.
//   * 렌더러에 Node 를 노출하지 않는다 (contextIsolation + sandbox + preload).
//   * 게이트웨이 경로는 코드에 고정하고, 인자는 형식을 검사한 값으로만 만든다.
//   * 게이트웨이 제어 포트에 기동 시 생성한 토큰으로 인증한다.
//   * 인증·권한 판정을 메인 프로세스에서 하고 IPC 핸들러마다 강제한다.
//   * 장치가 준 문자열을 파일 경로에 쓰지 않는다.
//   * 진단 도구는 허용 목록 + execFile(셸 없음)로만 실행한다.
//   * 플러그인과 업데이트는 서명을 검증한 뒤에만 쓴다.
//   * 로그에 개인정보와 자격증명을 남기지 않는다.
const { app, BrowserWindow, ipcMain, session } = require('electron')
const net = require('net')
const https = require('https')
const path = require('path')
const fs = require('fs')
const crypto = require('crypto')
const { spawn, execFile } = require('child_process')

const APP_VERSION = '0.0.2'

// --- 설치 폴더 자원 ---------------------------------------------------------
const RUNTIME_ROOT = app.isPackaged ? process.resourcesPath : path.join(__dirname, 'runtime')
const CONFIG_PATH = path.join(RUNTIME_ROOT, 'config', 'cannula.ini')
const KEY_PATH = path.join(RUNTIME_ROOT, 'config', 'cannula.key')
const TRUST_PATH = path.join(RUNTIME_ROOT, 'config', 'trusted-keys.json')
const PLUGIN_DIR = path.join(RUNTIME_ROOT, 'plugins')
const LOG_DIR = path.join(RUNTIME_ROOT, 'logs')
const LOG_PATH = path.join(LOG_DIR, 'cannula.log')
const BIN_DIR = path.join(RUNTIME_ROOT, 'bin')

// 게이트웨이 실행 파일 이름은 코드에 고정한다. 설정으로 바꿀 수 없다.
const BRIDGE_EXE = process.platform === 'win32' ? 'CANnulaBridge.exe' : 'CANnulaBridge'
const BRIDGE_PATH = path.join(BIN_DIR, BRIDGE_EXE)

// --- INI -------------------------------------------------------------------
function parseIni(text) {
  const out = {}
  let section = ''
  for (const rawLine of text.split(/\r?\n/)) {
    const line = rawLine.trim()
    if (!line || line.startsWith(';') || line.startsWith('#')) continue
    const sec = line.match(/^\[(.+)\]$/)
    if (sec) { section = sec[1].trim(); out[section] = out[section] || {}; continue }
    const eq = line.indexOf('=')
    if (eq < 0) continue
    const key = line.slice(0, eq).trim()
    const val = line.slice(eq + 1).trim()
    if (section) (out[section] = out[section] || {})[key] = val
    else out[key] = val
  }
  return out
}

let appConfig = {}
function loadConfig() {
  try { appConfig = parseIni(fs.readFileSync(CONFIG_PATH, 'utf8')) }
  catch (e) { appConfig = {} }
  return appConfig
}

// --- 로그 -------------------------------------------------------------------
// 개인정보와 자격증명은 남기지 않는다. 환자는 가명 식별자로만 기록한다.
const PII_PATTERNS = [
  /(password|passwd|pwd|token|secret|key|code)\s*[=:]\s*\S+/gi,
  /\b\d{6}-\d{7}\b/g                      // 주민등록번호 형태
]

function redact(msg) {
  let s = String(msg)
  for (const re of PII_PATTERNS) s = s.replace(re, (m) => m.split(/[=:]/)[0] + '=[삭제]')
  return s.length > 400 ? s.slice(0, 400) + '…' : s
}

function logLine(msg) {
  const level = ((appConfig.logging || {}).level || 'info').toLowerCase()
  if (level === 'off') return
  try {
    fs.mkdirSync(LOG_DIR, { recursive: true, mode: 0o700 })
    fs.appendFileSync(LOG_PATH, `${new Date().toISOString()}  ${redact(msg)}\n`, { mode: 0o600 })
  } catch (e) { /* 로그 실패가 앱 동작을 막지는 않는다 */ }
}

// --- 서명 검증 ---------------------------------------------------------------
// 신뢰 목록은 설치 시 배치된 공개키(Ed25519, SPKI PEM)다.
let trustedKeys = []
function loadTrustedKeys() {
  trustedKeys = []
  try {
    const j = JSON.parse(fs.readFileSync(TRUST_PATH, 'utf8'))
    for (const entry of j.keys || []) {
      try {
        trustedKeys.push({
          id: entry.id,
          key: crypto.createPublicKey({ key: entry.pem, format: 'pem', type: 'spki' })
        })
      } catch (e) { logLine(`신뢰 키 무효: ${entry.id}`) }
    }
  } catch (e) { /* 신뢰 목록이 없으면 아무 것도 신뢰하지 않는다 */ }
  logLine(`신뢰 공개키 ${trustedKeys.length}개 적재`)
}

// data 를 sigB64 로 검증한다. 신뢰 목록의 어느 키로든 맞으면 그 키 id 를 돌려준다.
function verifySignature(data, sigB64) {
  if (!sigB64) return null
  let sig
  try { sig = Buffer.from(sigB64, 'base64') } catch (e) { return null }
  for (const t of trustedKeys) {
    try {
      if (crypto.verify(null, data, t.key, sig)) return t.id
    } catch (e) { /* 다음 키 */ }
  }
  return null
}

// --- 플러그인: 서명을 검증한 것만 불러온다 -----------------------------------
let plugins = []
let drugLabels = {}
let alarmLabels = {}

function loadPlugins() {
  plugins = []
  drugLabels = {}
  alarmLabels = {}
  let files = []
  try {
    files = fs.readdirSync(PLUGIN_DIR).filter((f) => f.toLowerCase().endsWith('.js'))
  } catch (e) { return }

  for (const f of files) {
    const full = path.join(PLUGIN_DIR, f)
    const sigPath = full + '.sig'
    const entry = { file: f, name: f, version: '-', ok: false }

    let body, sigB64
    try { body = fs.readFileSync(full) } catch (e) { entry.error = '읽기 실패'; plugins.push(entry); continue }
    try { sigB64 = fs.readFileSync(sigPath, 'utf8').trim() } catch (e) { sigB64 = null }

    const signer = verifySignature(body, sigB64)
    if (!signer) {
      entry.error = '서명 없음 또는 검증 실패 — 불러오지 않았다'
      plugins.push(entry)
      logLine(`plugin rejected: ${f} (서명 검증 실패)`)
      continue
    }

    // 서명이 확인된 확장만 평가한다. 라벨 등록 외에는 아무 것도 주지 않는다.
    try {
      const sandboxCtx = {
        appVersion: APP_VERSION,
        registerDrugLabels: (map) => {
          for (const [k, v] of Object.entries(map || {}))
            if (/^\d+$/.test(String(k))) drugLabels[k] = String(v).slice(0, 60)
        },
        registerAlarmLabels: (map) => {
          for (const [k, v] of Object.entries(map || {}))
            if (/^\d+$/.test(String(k))) alarmLabels[k] = String(v).slice(0, 80)
        }
      }
      const mod = require(full)
      if (typeof mod.init === 'function') mod.init(sandboxCtx)
      entry.name = mod.name || f
      entry.version = mod.version || '-'
      entry.ok = true
      entry.signer = signer
      logLine(`plugin loaded: ${f} (서명자 ${signer})`)
    } catch (e) {
      entry.error = '초기화 실패'
      logLine(`plugin failed: ${f}`)
    }
    plugins.push(entry)
  }
}

// --- CAN 인증 키 -------------------------------------------------------------
// 없으면 만든다. 게이트웨이가 같은 파일을 읽는다.
function ensureCanKey() {
  try {
    const cur = fs.readFileSync(KEY_PATH, 'utf8').trim()
    if (/^[0-9a-fA-F]{64}$/.test(cur)) return true
  } catch (e) { /* 아래에서 만든다 */ }
  try {
    fs.mkdirSync(path.dirname(KEY_PATH), { recursive: true, mode: 0o700 })
    fs.writeFileSync(KEY_PATH, crypto.randomBytes(32).toString('hex'), { mode: 0o600 })
    logLine('CAN 인증 키를 새로 생성했다')
    return true
  } catch (e) {
    logLine('CAN 인증 키 생성 실패')
    return false
  }
}

// --- 게이트웨이 -------------------------------------------------------------
let bridgeProc = null
let bridgeSock = null
let bridgeBuf = ''
let bridgeUp = false
let bridgeAuthed = false
let mainWin = null
let lastState = {}

// 기동할 때마다 새로 만드는 제어 포트 토큰. 디스크에 남기지 않는다.
const BRIDGE_TOKEN = crypto.randomBytes(24).toString('hex')
// 설정을 읽은 뒤에 정한다 (loadConfig 이후 resolveBridgePort 호출).
let BRIDGE_PORT = 47100
function resolveBridgePort() {
  const p = Number((appConfig.bridge || {}).port)
  BRIDGE_PORT = Number.isInteger(p) && p >= 1024 && p <= 65535 ? p : 47100
}

function bridgeArgs() {
  const cfg = appConfig.bridge || {}
  const mode = cfg.transport === 'slcan' ? 'slcan' : 'sim'
  const bitrate = [125, 250, 500, 800, 1000].includes(Number(cfg.bitrate))
    ? Number(cfg.bitrate) : 500

  const args = [
    '--port', String(BRIDGE_PORT),
    '--token', BRIDGE_TOKEN,
    '--key-file', KEY_PATH,
    '--transport', mode,
    '--bitrate', String(bitrate)
  ]
  // 시리얼 포트 이름은 형식을 확인한 값만 넘긴다.
  if (mode === 'slcan' && /^(COM\d{1,3}|\/dev\/tty[A-Za-z0-9]{1,12})$/.test(cfg.serial || ''))
    args.push('--serial', cfg.serial)
  return args
}

function startBridge() {
  if (!fs.existsSync(BRIDGE_PATH)) {
    logLine(`bridge missing: ${BRIDGE_EXE}`)
    sendToUi('bridge:log', '게이트웨이 실행 파일을 찾을 수 없습니다.')
    return
  }
  const args = bridgeArgs()
  logLine(`bridge spawn (${args.filter((a) => a !== BRIDGE_TOKEN).join(' ')})`)
  bridgeProc = spawn(BRIDGE_PATH, args, { cwd: BIN_DIR, windowsHide: true })
  bridgeProc.stdout.on('data', (d) => {
    const s = String(d).trim()
    if (s) { logLine(`[bridge] ${s}`); sendToUi('bridge:log', s) }
  })
  bridgeProc.stderr.on('data', (d) => {
    const s = String(d).trim()
    if (s) logLine(`[bridge:err] ${s}`)
  })
  bridgeProc.on('exit', (code) => {
    logLine(`bridge exit ${code}`)
    sendToUi('bridge:log', `게이트웨이가 종료되었습니다 (코드 ${code})`)
    bridgeProc = null
  })
  bridgeProc.on('error', () => sendToUi('bridge:log', '게이트웨이 실행 실패'))
}

let bridgeTarget = { up: false, port: BRIDGE_PORT }

function connectBridge() {
  if (bridgeSock) { try { bridgeSock.destroy() } catch (e) {} }
  bridgeAuthed = false
  bridgeTarget = { up: false, port: BRIDGE_PORT }

  bridgeSock = net.createConnection({ host: '127.0.0.1', port: BRIDGE_PORT }, () => {
    bridgeUp = true
    bridgeBuf = ''
    logLine(`bridge connected 127.0.0.1:${BRIDGE_PORT}`)
    // 접속 직후 토큰을 제시한다.
    bridgeWrite({ op: 'auth', token: BRIDGE_TOKEN })
  })
  bridgeSock.setEncoding('utf8')
  bridgeSock.on('data', (chunk) => {
    bridgeBuf += chunk
    if (bridgeBuf.length > 1 << 20) { bridgeBuf = ''; return }
    let nl
    while ((nl = bridgeBuf.indexOf('\n')) >= 0) {
      const line = bridgeBuf.slice(0, nl)
      bridgeBuf = bridgeBuf.slice(nl + 1)
      if (!line.trim()) continue
      let msg
      try { msg = JSON.parse(line) } catch (e) { continue }
      handleBridgeEvent(msg)
    }
  })
  bridgeSock.on('error', (e) => {
    bridgeUp = false
    bridgeAuthed = false
    bridgeTarget = { up: false, port: BRIDGE_PORT, error: e.message }
    sendToUi('bridge:status', bridgeTarget)
  })
  bridgeSock.on('close', () => {
    bridgeUp = false
    bridgeAuthed = false
    bridgeTarget = { up: false, port: BRIDGE_PORT }
    sendToUi('bridge:status', bridgeTarget)
  })
}

function bridgeWrite(obj) {
  if (!bridgeSock || !bridgeUp) return false
  try { bridgeSock.write(JSON.stringify(obj) + '\n'); return true } catch (e) { return false }
}

// 인증이 끝난 뒤에만 명령을 보낸다.
function bridgeCmd(obj) {
  if (!bridgeAuthed) return false
  return bridgeWrite(obj)
}

function handleBridgeEvent(msg) {
  if (msg.ev === 'hello') {
    bridgeAuthed = true
    bridgeTarget = { up: true, port: BRIDGE_PORT }
    sendToUi('bridge:status', bridgeTarget)
    sendToUi('bridge:hello', msg)
    // 현재 세션 권한을 게이트웨이에 알린다.
    bridgeCmd({ op: 'session', level: currentSession ? currentSession.level : 0, sid: sessionCounter })
  } else if (msg.ev === 'authFailed') {
    logLine('bridge auth failed')
    sendToUi('bridge:log', '게이트웨이 인증에 실패했습니다.')
  } else if (msg.ev === 'state') {
    lastState = msg.s || {}
    sendToUi('bridge:state', lastState)
  } else if (msg.ev === 'frame') {
    sendToUi('bridge:frame', msg)
  } else if (msg.ev === 'log') {
    logLine(`[bus] ${msg.msg}`)
    sendToUi('bridge:log', msg.msg)
  }
}

function sendToUi(channel, payload) {
  if (mainWin && !mainWin.isDestroyed()) mainWin.webContents.send(channel, payload)
}

// --- 창 --------------------------------------------------------------------
function createWindow() {
  mainWin = new BrowserWindow({
    width: 1280,
    height: 860,
    title: 'CANnula Clinical Workstation',
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      contextIsolation: true,
      nodeIntegration: false,
      sandbox: true,
      webSecurity: true,
      allowRunningInsecureContent: false,
      spellcheck: false
    }
  })

  // 외부 탐색과 새 창을 막는다.
  mainWin.webContents.setWindowOpenHandler(() => ({ action: 'deny' }))
  mainWin.webContents.on('will-navigate', (e) => e.preventDefault())

  mainWin.loadFile(path.join(__dirname, 'renderer', 'index.html'))
}

// --- 인증 · 권한 -------------------------------------------------------------
// 계정은 scrypt 해시로 보관한다. 평문 비밀번호는 설정 파일에 없다.
//   형식: user = scrypt$<N>$<r>$<p>$<salt-b64>$<hash-b64>$<role>
let currentSession = null
let sessionCounter = 0
const loginFails = new Map()

function parseAccount(spec) {
  const parts = String(spec).split('$')
  if (parts.length !== 7 || parts[0] !== 'scrypt') return null
  return {
    N: Number(parts[1]), r: Number(parts[2]), p: Number(parts[3]),
    salt: Buffer.from(parts[4], 'base64'),
    hash: Buffer.from(parts[5], 'base64'),
    role: parts[6]
  }
}

const ROLE_LEVEL = { nurse: 1, tech: 2, admin: 2 }

function verifyPassword(acct, password) {
  return new Promise((resolve) => {
    crypto.scrypt(password, acct.salt, acct.hash.length,
      { N: acct.N, r: acct.r, p: acct.p, maxmem: 128 * 1024 * 1024 },
      (err, derived) => {
        if (err) return resolve(false)
        // 길이가 같을 때만 상수 시간 비교가 성립한다.
        resolve(derived.length === acct.hash.length &&
                crypto.timingSafeEqual(derived, acct.hash))
      })
  })
}

ipcMain.handle('auth:login', async (_e, { user, password }) => {
  if (typeof user !== 'string' || typeof password !== 'string') return { error: '잘못된 요청' }
  if (user.length > 64 || password.length > 256) return { error: '잘못된 요청' }

  const rec = loginFails.get(user) || { count: 0, until: 0 }
  if (Date.now() < rec.until) {
    return { error: '시도가 너무 많습니다. 잠시 후 다시 시도하십시오.' }
  }

  const acct = parseAccount(((appConfig.users || {})[user]) || '')
  // 계정이 없어도 같은 시간이 걸리도록 더미 검증을 한다.
  const target = acct || {
    N: 16384, r: 8, p: 1,
    salt: Buffer.alloc(16), hash: Buffer.alloc(32), role: 'nurse'
  }
  const ok = await verifyPassword(target, password) && !!acct

  if (!ok) {
    rec.count++
    if (rec.count >= 5) { rec.until = Date.now() + 60000; rec.count = 0 }
    loginFails.set(user, rec)
    logLine(`login failed: ${user}`)
    return { error: '사용자 또는 비밀번호가 올바르지 않습니다.' }
  }

  loginFails.delete(user)
  sessionCounter++
  currentSession = {
    user,
    role: acct.role,
    level: ROLE_LEVEL[acct.role] || 1,
    id: sessionCounter,
    started: Date.now()
  }
  logLine(`login ok: ${user} (${acct.role})`)
  bridgeCmd({ op: 'session', level: currentSession.level, sid: sessionCounter })
  return { user, role: acct.role, level: currentSession.level }
})

ipcMain.handle('auth:logout', () => {
  if (currentSession) logLine(`logout: ${currentSession.user}`)
  currentSession = null
  bridgeCmd({ op: 'session', level: 0, sid: 0 })
  return true
})

ipcMain.handle('auth:session', () => currentSession
  ? { user: currentSession.user, role: currentSession.role, level: currentSession.level }
  : null)

// 권한을 요구하는 핸들러용 보조. 부족하면 문자열 사유를 돌려준다.
function requireLevel(min) {
  if (!currentSession) return '로그인이 필요합니다.'
  if (currentSession.level < min) return '권한이 부족합니다.'
  return null
}

// --- 런타임 정보 (비밀은 빼고 준다) -----------------------------------------
ipcMain.handle('app:runtime', () => ({
  version: APP_VERSION,
  platform: process.platform,
  site: appConfig.site || {},
  // 계정 목록·해시·키·토큰은 렌더러에 주지 않는다.
  users: Object.keys(appConfig.users || {}).length,
  plugins: plugins.map((p) => ({
    file: p.file, name: p.name, version: p.version,
    ok: p.ok, signer: p.signer || null, error: p.error || null
  })),
  drugLabels,
  alarmLabels,
  transport: (appConfig.bridge || {}).transport === 'slcan' ? 'slcan' : 'sim',
  logDir: LOG_DIR,
  updateBase: (appConfig.update || {}).base_url || '',
  trustedKeys: trustedKeys.map((t) => t.id)
}))

ipcMain.handle('bridge:up', () => bridgeTarget)
ipcMain.handle('bridge:state', () => lastState)

// --- 임상 동작 (권한 강제 + 형식 검사) --------------------------------------
// 약물 라이브러리는 메인 프로세스가 준다 (게이트웨이 한계와 같은 값).
const DRUGS = [
  { id: 1, name: 'Morphine',    concMcgPerMl: 1000,    min: 1,   max: 50 },
  { id: 2, name: 'Fentanyl',    concMcgPerMl: 50,      min: 10,  max: 200 },
  { id: 3, name: 'Insulin',     concMcgPerMl: 1000,    min: 1,   max: 10 },
  { id: 4, name: 'Heparin',     concMcgPerMl: 1000000, min: 100, max: 1000 },
  { id: 5, name: 'Dopamine',    concMcgPerMl: 400000,  min: 2,   max: 20 },
  { id: 6, name: 'Calibration', concMcgPerMl: 1000,    min: 1,   max: 20 }
]
const LIMITS = { hardMax: 500, softMax: 200, hardMin: 1, maxVtbi: 9999, maxBolus: 20 }

function intInRange(v, lo, hi) {
  return Number.isInteger(v) && v >= lo && v <= hi
}

// 게이트웨이·엔진과 같은 한계로 여기서도 판정한다. 최종 강제는 엔진이 하지만,
// 임상 사용자에게 결과를 정확히 알려 주려면 차단 사유를 여기서 알아야 한다.
function dersVerdict(rate, vtbi, drug, weight) {
  const d = DRUGS.find((x) => x.id === drug)
  if (!d) return { blocked: true, text: '차단: 등록되지 않은 약물' }
  if (weight < 1 || weight > 250) return { blocked: true, text: '차단: 체중이 허용 범위 밖' }
  if (vtbi < 1 || vtbi > LIMITS.maxVtbi) return { blocked: true, text: '차단: 총 주입량이 허용 범위 밖' }
  const hardMax = Math.min(d.max, LIMITS.hardMax)
  const hardMin = Math.max(d.min, LIMITS.hardMin)
  if (rate > hardMax) return { blocked: true, text: `차단: 절대 상한 ${hardMax} mL/h 초과` }
  if (rate < hardMin) return { blocked: true, text: `차단: 하한 ${hardMin} mL/h 미달` }
  if (rate > LIMITS.softMax) return { blocked: false, warn: true, text: '경고 상한 초과 — 처방을 재확인하십시오.' }
  return { blocked: false, text: '통과' }
}

ipcMain.handle('pump:setrate', (_e, { rate, vtbi, drug, weight }) => {
  const deny = requireLevel(1)
  if (deny) return { error: deny }
  if (!intInRange(rate, 0, 65535) || !intInRange(vtbi, 0, 65535) ||
      !intInRange(drug, 0, 65535) || !intInRange(weight, 0, 255))
    return { error: '값이 허용 범위를 벗어났습니다.' }

  const v = dersVerdict(rate, vtbi, drug, weight)
  if (v.blocked) {
    logLine(`setrate blocked (${v.text})`)
    return { error: `가드레일에 걸려 전송하지 않았습니다 — ${v.text}` }
  }

  // 엔진이 다시 판정하고, 통과하지 못하면 프레임을 내보내지 않는다.
  const sent = bridgeCmd({ op: 'setrate', rate, vtbi, drug, weight })
  logLine(`setrate rate=${rate} vtbi=${vtbi} drug=${drug} weight=${weight}`)
  if (!sent) return { error: '게이트웨이에 연결되어 있지 않습니다.' }
  return { ok: true, verdict: v.text, warn: !!v.warn }
})

const ALLOWED_CMDS = new Set([1, 2, 3])   // start / stop / bolus

ipcMain.handle('pump:control', (_e, { cmd, arg }) => {
  const deny = requireLevel(1)
  if (deny) return { error: deny }
  if (!ALLOWED_CMDS.has(cmd)) return { error: '허용되지 않은 명령입니다.' }
  if (!intInRange(arg || 0, 0, 65535)) return { error: '값이 허용 범위를 벗어났습니다.' }
  if (cmd === 3 && (arg < 1 || arg > LIMITS.maxBolus))
    return { error: `볼루스는 1–${LIMITS.maxBolus} mL 범위여야 합니다.` }

  const sent = bridgeCmd({ op: 'control', cmd, arg: arg || 0 })
  logLine(`control cmd=${cmd} arg=${arg || 0}`)
  return sent ? { ok: true } : { error: '게이트웨이에 연결되어 있지 않습니다.' }
})

ipcMain.handle('pump:ack', (_e, { alarm }) => {
  const deny = requireLevel(1)
  if (deny) return { error: deny }
  if (!intInRange(alarm || 0, 0, 255)) return { error: '값이 허용 범위를 벗어났습니다.' }
  const sent = bridgeCmd({ op: 'ack', alarm: alarm || 0 })
  logLine('alarm ack')
  return sent ? { ok: true } : { error: '게이트웨이에 연결되어 있지 않습니다.' }
})

ipcMain.handle('pump:drugs', () => ({ drugs: DRUGS, limits: LIMITS }))

// --- 서비스 명령 (기사 권한, 허용 목록) --------------------------------------
const SERVICE_ALLOW = ['id', 'ver', 'status', 'selftest']

ipcMain.handle('service:list', () => {
  const deny = requireLevel(2)
  return deny ? { error: deny } : { commands: SERVICE_ALLOW }
})

ipcMain.handle('service:run', (_e, { cmd }) => {
  const deny = requireLevel(2)
  if (deny) return { error: deny }
  if (!SERVICE_ALLOW.includes(cmd)) return { error: '허용되지 않은 명령입니다.' }
  const sent = bridgeCmd({ op: 'service', cmd })
  logLine(`service ${cmd}`)
  return sent ? { ok: true } : { error: '게이트웨이에 연결되어 있지 않습니다.' }
})

// --- 진단 도구 (허용 목록 + 인자 배열 + 셸 없음) -----------------------------
// 설정 파일이 명령줄을 정하지 않는다. 도구는 코드에 고정한다.
const DIAG_TOOLS = {
  selftest: { exe: () => BRIDGE_PATH, args: ['--selftest'] },
  loopback: {
    exe: () => (process.platform === 'win32'
      ? path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'ping.exe')
      : '/bin/ping'),
    args: process.platform === 'win32' ? ['-n', '2', '127.0.0.1'] : ['-c', '2', '127.0.0.1']
  }
}

ipcMain.handle('diag:list', () => {
  const deny = requireLevel(2)
  return deny ? { error: deny } : { tools: Object.keys(DIAG_TOOLS) }
})

ipcMain.handle('diag:run', (_e, { tool }) => {
  const deny = requireLevel(2)
  if (deny) return Promise.resolve({ error: deny })
  const t = DIAG_TOOLS[tool]
  if (!t) return Promise.resolve({ error: '등록되지 않은 도구입니다.' })

  return new Promise((resolve) => {
    // execFile: 셸을 거치지 않는다. 인자는 배열로만 전달된다.
    execFile(t.exe(), t.args, { timeout: 15000, windowsHide: true, maxBuffer: 256 * 1024 },
      (err, stdout, stderr) => {
        logLine(`diag ${tool} → ${err ? 'error' : 'ok'}`)
        resolve({
          tool,
          exitCode: err ? (err.code === undefined ? -1 : err.code) : 0,
          output: `${stdout || ''}${stderr || ''}`.trim().slice(0, 8000)
        })
      })
  })
})

// --- 전송 방식 (허용된 값만) -------------------------------------------------
ipcMain.handle('bridge:transport', (_e, { mode, serial, bitrate }) => {
  const deny = requireLevel(2)
  if (deny) return { error: deny }
  if (mode !== 'sim' && mode !== 'slcan') return { error: '허용되지 않은 전송 방식입니다.' }
  const br = [125, 250, 500, 800, 1000].includes(Number(bitrate)) ? Number(bitrate) : 500
  let port = ''
  if (mode === 'slcan') {
    if (!/^(COM\d{1,3}|\/dev\/tty[A-Za-z0-9]{1,12})$/.test(serial || ''))
      return { error: '시리얼 포트 이름 형식이 올바르지 않습니다.' }
    port = serial
  }
  const sent = bridgeCmd({ op: 'transport', mode, serial: port, bitrate: br })
  logLine(`transport ${mode} ${br}`)
  return sent ? { ok: true } : { error: '게이트웨이에 연결되어 있지 않습니다.' }
})

// --- 환자 정보 (앱 데이터 폴더, 권한 제한) -----------------------------------
const PATIENT_PATH = () => path.join(app.getPath('userData'), 'patient.json')

ipcMain.handle('patient:save', (_e, p) => {
  const deny = requireLevel(1)
  if (deny) return { error: deny }
  const clean = {
    id: String(p && p.id || '').slice(0, 32),
    name: String(p && p.name || '').slice(0, 64),
    bed: String(p && p.bed || '').slice(0, 32),
    weight: Number(p && p.weight) || 0,
    height: Number(p && p.height) || 0,
    allergy: String(p && p.allergy || '').slice(0, 128)
  }
  try {
    fs.writeFileSync(PATIENT_PATH(), JSON.stringify(clean), { mode: 0o600 })
    // 로그에는 가명 식별자만 남긴다.
    const pseudo = crypto.createHash('sha256').update(clean.id).digest('hex').slice(0, 12)
    logLine(`patient assigned (pseudo=${pseudo})`)
    return { ok: true }
  } catch (e) { return { error: '저장하지 못했습니다.' } }
})

ipcMain.handle('patient:load', () => {
  const deny = requireLevel(1)
  if (deny) return { error: deny }
  try { return JSON.parse(fs.readFileSync(PATIENT_PATH(), 'utf8')) }
  catch (e) { return null }
})

// --- 이벤트 로그 ------------------------------------------------------------
// 파일 이름은 앱이 정한다. 장치나 렌더러가 경로에 관여하지 않는다.
function safeLogPath(name) {
  const base = path.basename(String(name))
  if (!/^[A-Za-z0-9._-]{1,64}$/.test(base)) return null
  const dest = path.resolve(LOG_DIR, base)
  const root = path.resolve(LOG_DIR) + path.sep
  return dest.startsWith(root) ? dest : null
}

ipcMain.handle('log:event', (_e, { kind, text }) => {
  const k = String(kind || '').slice(0, 24)
  logLine(`[ui:${k}] ${String(text || '').slice(0, 300)}`)
  return true
})

ipcMain.handle('log:export', (_e, { rows }) => {
  const deny = requireLevel(1)
  if (deny) return { error: deny }
  if (!Array.isArray(rows)) return { error: '잘못된 요청' }

  const stamp = new Date().toISOString().replace(/[:.]/g, '-')
  const name = `events-${stamp}.csv`
  const dest = safeLogPath(name)
  if (!dest) return { error: '파일 이름을 만들 수 없습니다.' }

  const header = `# CANnula Workstation ${APP_VERSION}\n` +
                 `# 세션 ${currentSession ? currentSession.id : '-'}\n` +
                 `시각,구분,내용\n`
  const body = rows.slice(0, 5000).map((r) => {
    const t = String(r && r.t || '').slice(0, 40)
    const k = String(r && r.kind || '').slice(0, 24)
    const x = redact(String(r && r.text || '')).replace(/"/g, '""').slice(0, 300)
    return `${t},${k},"${x}"`
  }).join('\n')

  try {
    fs.mkdirSync(LOG_DIR, { recursive: true, mode: 0o700 })
    fs.writeFileSync(dest, header + body + '\n', { mode: 0o600 })
    logLine(`log exported: ${path.basename(dest)}`)
    return { ok: true, name: path.basename(dest) }
  } catch (e) { return { error: '저장하지 못했습니다.' } }
})

ipcMain.handle('log:list', () => {
  const deny = requireLevel(1)
  if (deny) return { error: deny }
  try {
    return {
      files: fs.readdirSync(LOG_DIR)
        .filter((f) => /^[A-Za-z0-9._-]+$/.test(f))
        .map((f) => ({ name: f, size: fs.statSync(path.join(LOG_DIR, f)).size }))
    }
  } catch (e) { return { files: [] } }
})

// --- 소프트웨어 업데이트 (HTTPS 고정 + 서명 검증) ---------------------------
let pendingUpdate = null

function httpsGet(url) {
  return new Promise((resolve) => {
    let u
    try { u = new URL(url) } catch (e) { return resolve({ error: '잘못된 주소' }) }
    // 평문 HTTP 는 허용하지 않는다.
    if (u.protocol !== 'https:') return resolve({ error: 'HTTPS 만 허용됩니다.' })

    const req = https.get(u, {
      // 인증서를 검증한다 (v0.0.1 은 rejectUnauthorized:false 였다).
      rejectUnauthorized: true,
      minVersion: 'TLSv1.2'
    }, (res) => {
      if (res.statusCode !== 200) {
        res.resume()
        return resolve({ error: `HTTP ${res.statusCode}` })
      }
      const chunks = []
      let total = 0
      res.on('data', (c) => {
        total += c.length
        if (total > 64 * 1024 * 1024) { req.destroy(); return }
        chunks.push(c)
      })
      res.on('end', () => resolve({ ok: true, body: Buffer.concat(chunks) }))
    })
    req.on('error', (e) => resolve({ error: e.message }))
    req.setTimeout(20000, () => req.destroy(new Error('timeout')))
  })
}

ipcMain.handle('update:check', async () => {
  const deny = requireLevel(2)
  if (deny) return { error: deny }

  const base = (appConfig.update || {}).base_url || ''
  if (!base.startsWith('https://')) return { error: '업데이트 서버 주소가 HTTPS 가 아닙니다.' }

  const r = await httpsGet(`${base}/api/update/manifest?current=${APP_VERSION}`)
  if (r.error) return { error: r.error }

  let manifest
  try { manifest = JSON.parse(r.body.toString('utf8')) }
  catch (e) { return { error: '매니페스트를 해석할 수 없습니다.' } }

  // 매니페스트 본문(서명 필드 제외)에 대한 서명을 검증한다.
  const sig = manifest.signature
  delete manifest.signature
  const canonical = Buffer.from(JSON.stringify(manifest), 'utf8')
  const signer = verifySignature(canonical, sig)
  if (!signer) {
    logLine('update manifest 서명 검증 실패')
    return { error: '매니페스트 서명을 확인할 수 없습니다. 업데이트를 중단합니다.' }
  }
  if (!/^https:\/\//.test(manifest.artifactUrl || '')) {
    return { error: '아티팩트 주소가 HTTPS 가 아닙니다.' }
  }
  if (!/^[0-9a-f]{64}$/i.test(manifest.sha256 || '')) {
    return { error: '아티팩트 해시가 매니페스트에 없습니다.' }
  }

  pendingUpdate = { manifest, signer }
  logLine(`update available ${manifest.version} (서명자 ${signer})`)
  return { version: manifest.version, notes: manifest.notes || '', signer, base }
})

ipcMain.handle('update:apply', async () => {
  const deny = requireLevel(2)
  if (deny) return { error: deny }
  if (!pendingUpdate) return { error: '먼저 업데이트를 확인하십시오.' }

  const { manifest } = pendingUpdate
  const r = await httpsGet(manifest.artifactUrl)
  if (r.error) return { error: r.error }

  // 해시를 확인한 뒤에만 디스크에 남긴다.
  const digest = crypto.createHash('sha256').update(r.body).digest('hex')
  if (digest.toLowerCase() !== manifest.sha256.toLowerCase()) {
    logLine('update artifact 해시 불일치 — 폐기')
    return { error: '내려받은 패키지의 해시가 맞지 않습니다. 적용을 중단합니다.' }
  }

  // 아티팩트 자체 서명도 확인한다.
  const artSigner = verifySignature(r.body, manifest.artifactSignature)
  if (!artSigner) {
    logLine('update artifact 서명 검증 실패 — 폐기')
    return { error: '패키지 서명을 확인할 수 없습니다. 적용을 중단합니다.' }
  }

  // 검증을 통과한 설치 파일을 사용자에게 넘긴다. 앱이 직접 실행하지 않는다.
  const dest = path.join(app.getPath('downloads'),
                         `CANnula-Workstation-${manifest.version}-Setup.exe`)
  try {
    fs.writeFileSync(dest, r.body, { mode: 0o600 })
  } catch (e) { return { error: '패키지를 저장하지 못했습니다.' } }

  logLine(`update verified & saved: ${path.basename(dest)} (서명자 ${artSigner})`)
  return {
    ok: true,
    saved: path.basename(dest),
    signer: artSigner,
    message: '서명과 해시를 확인했습니다. 다운로드 폴더의 설치 파일을 실행해 업데이트하십시오.'
  }
})

// --- 기동 ------------------------------------------------------------------
app.whenReady().then(() => {
  loadConfig()
  resolveBridgePort()
  loadTrustedKeys()
  loadPlugins()
  ensureCanKey()

  // 렌더러에 엄격한 CSP 를 강제한다 (HTML 의 meta 와 이중으로).
  session.defaultSession.webRequest.onHeadersReceived((details, cb) => {
    cb({
      responseHeaders: {
        ...details.responseHeaders,
        'Content-Security-Policy': [
          "default-src 'none'; script-src 'self'; style-src 'self'; " +
          "img-src 'self' data:; font-src 'self'; connect-src 'none'; " +
          "form-action 'none'; base-uri 'none'; frame-ancestors 'none'"
        ]
      }
    })
  })

  logLine(`app start v${APP_VERSION}`)
  createWindow()
  startBridge()
  setTimeout(connectBridge, 900)
})

app.on('window-all-closed', () => {
  if (bridgeProc) { try { bridgeProc.kill() } catch (e) {} }
  app.quit()
})
