// CANnula Clinical Workstation (WEAK) — Electron main process.
//
// ⚠  교육·시연용 예시 앱입니다. 실제 의료기기나 환자와 함께 사용하지 마세요.
//
// 이 앱은 침투 테스트 실습의 "대상 제품"입니다. 공격을 대신 수행해 주는 버튼이나
// 미리 채워 둔 페이로드는 없습니다. 아래 구현이 그대로 공격 표면입니다.
//   * 렌더러가 Node 권한을 그대로 갖는 설정으로 창을 띄운다.
//   * CAN 게이트웨이(CANnulaBridge)를 설정 파일이 가리키는 경로에서 실행한다.
//   * 게이트웨이가 올려 주는 장치 문자열을 그대로 화면과 파일 이름에 쓴다.
//   * 업데이트는 서버가 알려 준 URL의 아티팩트를 받아 그대로 실행한다.
const { app, BrowserWindow, ipcMain, dialog } = require('electron')
const net = require('net')
const http = require('http')
const https = require('https')
const os = require('os')
const path = require('path')
const fs = require('fs')
const { spawn, spawnSync, exec } = require('child_process')

// 릴리스 버전. 정상 배포본은 0.0.1 로 고정한다.
const APP_VERSION = '0.0.1'

// --- 설치 폴더 자원(설정 / 플러그인 / 로그 / 네이티브 게이트웨이) ------------
// 설치본에서는 …\resources\ 아래, 개발 중에는 프로젝트의 runtime\ 아래를 본다.
const RUNTIME_ROOT = app.isPackaged ? process.resourcesPath : path.join(__dirname, 'runtime')
const CONFIG_PATH = path.join(RUNTIME_ROOT, 'config', 'cannula.ini')
const PLUGIN_DIR = path.join(RUNTIME_ROOT, 'plugins')
const LOG_DIR = path.join(RUNTIME_ROOT, 'logs')
const LOG_PATH = path.join(LOG_DIR, 'cannula.log')
const BIN_DIR = path.join(RUNTIME_ROOT, 'bin')
const FW_DIR = path.join(RUNTIME_ROOT, 'firmware')

// 아주 단순한 INI 파서(섹션 + key=value, ; 또는 # 주석).
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
  try {
    appConfig = parseIni(fs.readFileSync(CONFIG_PATH, 'utf8'))
  } catch (e) {
    appConfig = {}
  }
  return appConfig
}

function logLine(msg) {
  const level = ((appConfig.logging || {}).level || 'info').toLowerCase()
  if (level === 'off') return
  try {
    fs.mkdirSync(LOG_DIR, { recursive: true })
    fs.appendFileSync(LOG_PATH, `${new Date().toISOString()}  ${msg}\n`)
  } catch (e) { /* 로그 실패가 앱 동작을 막지는 않는다 */ }
}

// 설치 폴더의 플러그인(.js)을 시작 시 전부 불러온다. 서명·출처 검증은 없다.
// 사이트별 약물 라벨 팩이나 알람 문구 확장을 이 경로로 배포한다.
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
    try {
      const mod = require(full)
      const ctx = {
        appVersion: APP_VERSION,
        log: (m) => logLine(`[plugin:${mod.name || f}] ${m}`),
        registerDrugLabels: (map) => Object.assign(drugLabels, map || {}),
        registerAlarmLabels: (map) => Object.assign(alarmLabels, map || {})
      }
      if (typeof mod.init === 'function') mod.init(ctx)
      plugins.push({ file: f, name: mod.name || f, version: mod.version || '-', ok: true })
      logLine(`plugin loaded: ${f} (${mod.name || '-'} ${mod.version || '-'})`)
    } catch (e) {
      plugins.push({ file: f, name: f, version: '-', ok: false, error: e.message })
      logLine(`plugin failed: ${f} — ${e.message}`)
    }
  }
}

// --- CAN 게이트웨이 프로세스 -------------------------------------------------
// 설정 파일이 알려 주는 실행 파일을 그대로 띄운다. 서명이나 해시는 확인하지 않는다.
let bridgeProc = null
// 띄워 둔 게이트웨이의 pid. spawn 직후 exit 이벤트가 먼저 오는 경우가 있어
// bridgeProc 하나만 믿으면 실제로 살아 있는 프로세스를 놓친다. 놓친 프로세스는
// 시리얼 포트와 47100 을 계속 물고 있어서 다음 게이트웨이가 붙지 못한다.
const bridgePids = new Set()
let bridgeSock = null
let bridgeBuf = ''
let bridgeUp = false
let mainWin = null

function bridgeExeName() {
  const cfg = appConfig.bridge || {}
  const fallback = process.platform === 'win32' ? 'CANnulaBridge.exe' : 'CANnulaBridge'
  if (!cfg.exe) return fallback
  // 설정이 가리키는 파일이 실제로 없으면 플랫폼 기본 이름으로 돌아간다.
  const p = path.isAbsolute(cfg.exe) ? cfg.exe : path.join(BIN_DIR, cfg.exe)
  return fs.existsSync(p) ? cfg.exe : fallback
}

// --- USB-CAN 어댑터 자동 탐지 ----------------------------------------------
// 설정의 serial 이 비어 있거나 auto 면 붙어 있는 어댑터를 찾아 쓴다. 포트 번호는
// 꽂는 자리에 따라 바뀌므로 설정 파일에 적어 두지 않는 편이 낫다.
const CAN_ADAPTER_IDS = [
  { vid: '16D0', pid: '117E', name: 'CANable (slcan)' },
  { vid: '1D50', pid: '606F', name: 'candleLight / CANable' },
  { vid: '0483', pid: '5740', name: 'STM32 가상 COM' },
  { vid: '1CBE', pid: '0267', name: 'TI Tiva CAN' },
  { vid: '10C4', pid: 'EA60', name: 'CP210x 계열 어댑터' },
  { vid: '1A86', pid: '7523', name: 'CH340 계열 어댑터' }
]

function listSerialPorts() {
  try {
    if (process.platform === 'win32') {
      const script =
        "Get-CimInstance Win32_PnPEntity | Where-Object { $_.Name -match '\\(COM\\d+\\)' } | " +
        "ForEach-Object { if ($_.Name -match '\\((COM\\d+)\\)') { $matches[1] + '|' + $_.PnPDeviceID + '|' + $_.Name } }"
      const r = spawnSync('powershell.exe', ['-NoProfile', '-NonInteractive', '-Command', script],
                          { encoding: 'utf8', timeout: 15000, windowsHide: true })
      return String(r.stdout || '').split(/\r?\n/).filter(Boolean).map((l) => {
        const [port, id, name] = l.split('|')
        return { port, id: id || '', name: name || '' }
      })
    }
    // 리눅스/맥: by-id 링크가 있으면 그것이 제조사 정보를 담고 있다.
    const dir = '/dev/serial/by-id'
    if (fs.existsSync(dir)) {
      return fs.readdirSync(dir).map((f) => ({
        port: fs.realpathSync(path.join(dir, f)), id: f, name: f
      }))
    }
    return fs.readdirSync('/dev')
      .filter((f) => /^tty(ACM|USB)\d+$/.test(f))
      .map((f) => ({ port: '/dev/' + f, id: f, name: f }))
  } catch (e) {
    logLine(`serial scan 실패: ${e.message}`)
    return []
  }
}

function detectSerialPort() {
  const ports = listSerialPorts()
  if (!ports.length) {
    logLine('serial auto: 직렬 포트를 찾지 못했습니다')
    return ''
  }
  for (const a of CAN_ADAPTER_IDS) {
    const hit = ports.find((p) => {
      const s = (p.id + ' ' + p.name).toUpperCase()
      return s.includes('VID_' + a.vid) && s.includes('PID_' + a.pid)
    })
    if (hit) { logLine(`serial auto: ${hit.port} (${a.name})`); return hit.port }
  }
  if (ports.length === 1) {
    logLine(`serial auto: ${ports[0].port} (유일한 직렬 포트, ${ports[0].name})`)
    return ports[0].port
  }
  logLine(`serial auto: 어댑터를 특정하지 못했습니다 — 후보 ${ports.map((p) => p.port).join(', ')}`)
  return ''
}

function bridgeArgs() {
  const cfg = appConfig.bridge || {}
  const args = [
    '--bind', cfg.bind || '0.0.0.0',
    '--port', String(cfg.port || 47100),
    '--transport', cfg.transport || 'sim',
    '--bitrate', String(cfg.bitrate || 500),
    '--udp-listen', String(cfg.udp_listen || 47101)
  ]
  const wantAuto = !cfg.serial || String(cfg.serial).trim().toLowerCase() === 'auto'
  const serial = wantAuto ? detectSerialPort() : cfg.serial
  if (serial) args.push('--serial', serial)
  if (cfg.udp_peer) args.push('--udp-peer', cfg.udp_peer)
  if ((cfg.verbose || '').toLowerCase() === 'true') args.push('--verbose')
  // 설정에 추가 인자가 적혀 있으면 그대로 붙인다.
  if (cfg.extra_args) args.push(...cfg.extra_args.split(/\s+/).filter(Boolean))
  return args
}

function startBridge() {
  const exePath = path.isAbsolute(bridgeExeName())
    ? bridgeExeName()
    : path.join(BIN_DIR, bridgeExeName())

  if (!fs.existsSync(exePath)) {
    logLine(`bridge missing: ${exePath}`)
    sendToUi('bridge:log', `게이트웨이 실행 파일이 없습니다: ${exePath}`)
    return
  }

  const args = bridgeArgs()
  logLine(`bridge spawn: ${exePath} ${args.join(' ')}`)
  const proc = spawn(exePath, args, { cwd: path.dirname(exePath) })
  bridgeProc = proc
  if (proc.pid) bridgePids.add(proc.pid)
  proc.stdout.on('data', (d) => {
    const s = String(d).trim()
    if (s) { logLine(`[bridge] ${s}`); sendToUi('bridge:log', s) }
  })
  proc.stderr.on('data', (d) => {
    const s = String(d).trim()
    if (s) { logLine(`[bridge:err] ${s}`); sendToUi('bridge:log', s) }
  })
  proc.on('exit', (code) => {
    logLine(`bridge exit ${code} (pid ${proc.pid})`)
    sendToUi('bridge:log', `게이트웨이가 종료되었습니다 (코드 ${code})`)
    // exit 를 받아도 pid 는 남겨 둔다. 이 이벤트가 실제 종료보다 먼저 오는 일이
    // 있어서, 여기서 지우면 살아 있는 프로세스를 영영 정리하지 못한다.
    // 실제 정리는 stopBridge() 가 pid 로 확인하며 수행한다.
    if (bridgeProc === proc) bridgeProc = null
  })
  proc.on('error', (e) => {
    logLine(`bridge error ${e.message}`)
    sendToUi('bridge:log', `게이트웨이 실행 실패: ${e.message}`)
  })
}

// 띄워 둔 게이트웨이를 모두 정리한다. 자식이 또 자식을 남기는 경우가 있어
// 윈도우에서는 트리째 끊는다. 이 앱이 직접 띄운 pid 만 건드린다.
function stopBridge() {
  for (const pid of Array.from(bridgePids)) {
    try {
      if (process.platform === 'win32') {
        spawnSync('taskkill', ['/F', '/T', '/PID', String(pid)], { windowsHide: true })
      } else {
        process.kill(pid, 'SIGKILL')
      }
      logLine(`bridge stop pid ${pid}`)
    } catch (e) {
      logLine(`bridge stop failed pid ${pid}: ${e.message}`)
    }
    bridgePids.delete(pid)
  }
  bridgeProc = null
}

let bridgeTarget = { up: false, host: '127.0.0.1', port: 47100 }

function connectBridge() {
  const cfg = appConfig.bridge || {}
  const host = cfg.connect_host || '127.0.0.1'
  const port = Number(cfg.port || 47100)
  bridgeTarget = { up: false, host, port }

  // 옛 소켓은 리스너를 떼고 닫는다. 떼지 않으면 그 소켓의 close 가 새 소켓이
  // 붙은 뒤에 도착해서 전역 bridgeUp 을 false 로 되돌린다. 그렇게 되면 화면은
  // "끊김" 이 되고 bridgeSend 가 명령을 조용히 버려서, 연결된 것처럼 보이는데
  // 주입 명령이 하나도 나가지 않는 상태가 된다.
  if (bridgeSock) {
    try { bridgeSock.removeAllListeners(); bridgeSock.destroy() } catch (e) {}
  }

  const sock = net.createConnection({ host, port }, () => {
    if (bridgeSock !== sock) { try { sock.destroy() } catch (e) {} ; return }
    bridgeUp = true
    bridgeBuf = ''
    bridgeTarget = { up: true, host, port }
    logLine(`bridge connected ${host}:${port}`)
    sendToUi('bridge:status', bridgeTarget)
    bridgeSend({ op: 'hello' })
  })
  bridgeSock = sock

  sock.setEncoding('utf8')
  sock.on('data', (chunk) => {
    if (bridgeSock !== sock) return
    bridgeBuf += chunk
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
  // 지난 소켓이 늦게 뱉는 error/close 는 현재 연결과 무관하므로 무시한다.
  sock.on('error', (e) => {
    if (bridgeSock !== sock) return
    if (bridgeUp) logLine(`bridge socket error ${e.message}`)
    bridgeUp = false
    bridgeTarget = { up: false, error: e.message, host, port }
    sendToUi('bridge:status', bridgeTarget)
  })
  sock.on('close', () => {
    if (bridgeSock !== sock) return
    bridgeUp = false
    bridgeTarget = { up: false, host, port }
    sendToUi('bridge:status', bridgeTarget)
  })
}

function bridgeSend(obj) {
  if (!bridgeSock || !bridgeUp) return false
  try { bridgeSock.write(JSON.stringify(obj) + '\n'); return true } catch (e) { return false }
}

function sendToUi(channel, payload) {
  if (mainWin && !mainWin.isDestroyed()) mainWin.webContents.send(channel, payload)
}

// 게이트웨이가 올려 주는 장치 상태. 장치가 알려 준 식별자는 그대로 보관해서
// 세션 이벤트 로그 파일 이름에 쓴다.
let lastState = {}
let deviceId = 'unknown-device'

// 주입 중에만 펌프에 생존 신호를 보낸다. 펌프는 프레임이 끊기면 주입을 멈추므로,
// 앱이 닫히거나 버스가 끊어지면 환자에게 약이 계속 들어가는 일이 없다.
// 0x110 CMD_GET_STATUS 는 펌웨어에 부작용이 없는 기존 명령이다.
const WS_HEARTBEAT_MS = 2000
setInterval(() => {
  if (lastState && lastState.pumpState === 1) {
    bridgeSend({ op: 'control', cmd: 0x05, token: 0x01 })
  }
}, WS_HEARTBEAT_MS)

function handleBridgeEvent(msg) {
  if (msg.ev === 'state') {
    lastState = msg.s || {}
    if (lastState.deviceId) deviceId = lastState.deviceId
    sendToUi('bridge:state', lastState)
  } else if (msg.ev === 'frame') {
    sendToUi('bridge:frame', msg)
  } else if (msg.ev === 'log') {
    logLine(`[bus] ${msg.msg}`)
    sendToUi('bridge:log', msg.msg)
  } else if (msg.ev === 'hello') {
    sendToUi('bridge:hello', msg)
  }
}

// --- 창 --------------------------------------------------------------------
function createWindow() {
  mainWin = new BrowserWindow({
    width: 1280,
    height: 860,
    title: 'CANnula Clinical Workstation (WEAK)',
    webPreferences: {
      nodeIntegration: true,
      contextIsolation: false,
      webSecurity: false,
      allowRunningInsecureContent: true
    }
  })
  mainWin.loadFile(path.join(__dirname, 'renderer', 'index.html'))
}

// --- IPC: 런타임 정보 -------------------------------------------------------
ipcMain.handle('app:runtime', () => ({
  version: APP_VERSION,
  root: RUNTIME_ROOT,
  configPath: CONFIG_PATH,
  config: appConfig,
  pluginDir: PLUGIN_DIR,
  plugins,
  drugLabels,
  alarmLabels,
  logDir: LOG_DIR,
  logPath: LOG_PATH,
  binDir: BIN_DIR,
  firmwareDir: FW_DIR,
  bridgeExe: path.join(BIN_DIR, bridgeExeName()),
  bridgeArgs: bridgeArgs(),
  platform: process.platform,
  pid: process.pid
}))

ipcMain.handle('app:log', (_e, msg) => { logLine(String(msg)); return true })

// --- IPC: 게이트웨이 제어 ---------------------------------------------------
// 렌더러가 보내는 명령을 게이트웨이로 그대로 넘긴다.
ipcMain.handle('bridge:cmd', (_e, obj) => bridgeSend(obj))
ipcMain.handle('bridge:state', () => lastState)
ipcMain.handle('bridge:up', () => bridgeTarget)
ipcMain.handle('bridge:reconnect', () => { connectBridge(); return true })
ipcMain.handle('bridge:restart', () => {
  // 먼저 옛 게이트웨이를 확실히 내린다. 남겨 두면 그쪽이 시리얼 포트를 쥔 채로
  // 47100 까지 물고 있어서, 새로 띄운 게이트웨이가 어댑터를 열지 못한다.
  stopBridge()
  // 포트가 실제로 풀릴 때까지 잠깐 기다렸다가 띄운다.
  setTimeout(() => {
    startBridge()
    setTimeout(connectBridge, 700)
  }, 400)
  return true
})

// --- IPC: 진단 도구 실행 ----------------------------------------------------
// 설정 파일의 [service] 항목에 등록된 도구를 실행한다. 서비스 기사가 현장에서
// 옵션을 덧붙여 쓸 수 있도록 추가 인자를 그대로 명령줄에 이어 붙인다.
ipcMain.handle('diag:run', (_e, { tool, extra }) => {
  const tools = appConfig.service || {}
  const cmd = tools[tool]
  if (!cmd) return Promise.resolve({ error: `등록되지 않은 도구: ${tool}` })

  const full = `${cmd} ${extra || ''}`
  logLine(`diag:run ${full}`)
  return new Promise((resolve) => {
    exec(full, { cwd: BIN_DIR, timeout: 20000, windowsHide: true },
      (err, stdout, stderr) => {
        resolve({
          command: full,
          exitCode: err ? (err.code === undefined ? -1 : err.code) : 0,
          output: `${stdout || ''}${stderr || ''}`.trim()
        })
      })
  })
})

// --- IPC: 이벤트 로그 ------------------------------------------------------
// 세션 로그는 장치가 알려 준 식별자로 파일을 나눈다. 병원에서 펌프별로 로그를
// 모아 두기 때문이다.
ipcMain.handle('log:write', (_e, { name, text, append }) => {
  const fname = name || `${deviceId}-events.csv`
  const dest = path.join(LOG_DIR, fname)
  try {
    fs.mkdirSync(path.dirname(dest), { recursive: true })
    if (append) fs.appendFileSync(dest, text)
    else fs.writeFileSync(dest, text)
    logLine(`log:write ${dest} (${text.length} bytes)`)
    return { ok: true, path: dest }
  } catch (e) {
    return { error: e.message, path: dest }
  }
})

ipcMain.handle('log:read', (_e, name) => {
  const dest = path.join(LOG_DIR, name || 'cannula.log')
  try { return { ok: true, path: dest, text: fs.readFileSync(dest, 'utf8') } }
  catch (e) { return { error: e.message, path: dest } }
})

ipcMain.handle('log:list', () => {
  try {
    return fs.readdirSync(LOG_DIR).map((f) => {
      const st = fs.statSync(path.join(LOG_DIR, f))
      return { name: f, size: st.size, mtime: st.mtimeMs }
    })
  } catch (e) { return [] }
})

// --- IPC: 펌웨어 파일 ------------------------------------------------------
ipcMain.handle('fw:pick', async () => {
  const r = await dialog.showOpenDialog(mainWin, {
    title: '펌웨어 이미지 선택',
    defaultPath: FW_DIR,
    filters: [{ name: '펌웨어 이미지', extensions: ['bin', 'hex'] }, { name: '모든 파일', extensions: ['*'] }],
    properties: ['openFile']
  })
  if (r.canceled || !r.filePaths.length) return null
  return readFirmware(r.filePaths[0])
})

ipcMain.handle('fw:list', () => {
  try {
    return fs.readdirSync(FW_DIR).map((f) => {
      const st = fs.statSync(path.join(FW_DIR, f))
      return { name: f, size: st.size }
    })
  } catch (e) { return [] }
})

ipcMain.handle('fw:load', (_e, name) => readFirmware(path.join(FW_DIR, name)))

function readFirmware(p) {
  try {
    const raw = fs.readFileSync(p)
    // .hex 는 Intel HEX 로 보고 데이터 레코드를 이어 붙인다.
    if (p.toLowerCase().endsWith('.hex')) {
      const bytes = []
      for (const line of raw.toString('ascii').split(/\r?\n/)) {
        if (!line.startsWith(':')) continue
        const len = parseInt(line.substr(1, 2), 16)
        const type = parseInt(line.substr(7, 2), 16)
        if (type !== 0) continue
        for (let i = 0; i < len; i++) bytes.push(parseInt(line.substr(9 + i * 2, 2), 16))
      }
      return { path: p, size: bytes.length, dataB64: Buffer.from(bytes).toString('base64'), format: 'hex' }
    }
    return { path: p, size: raw.length, dataB64: raw.toString('base64'), format: 'bin' }
  } catch (e) {
    return { error: e.message, path: p }
  }
}

// --- IPC: 소프트웨어 업데이트 ------------------------------------------------
ipcMain.handle('app:version', () => APP_VERSION)

function httpGet(url) {
  return new Promise((resolve) => {
    let lib
    try { lib = new URL(url).protocol === 'https:' ? https : http }
    catch (e) { return resolve({ error: '잘못된 주소: ' + url }) }
    const opts = { rejectUnauthorized: false }   // 자체서명 랩 서버 대응
    const req = lib.get(url, opts, (res) => {
      const chunks = []
      res.on('data', (c) => chunks.push(c))
      res.on('end', () => resolve({
        ok: res.statusCode >= 200 && res.statusCode < 300,
        status: res.statusCode,
        bodyB64: Buffer.concat(chunks).toString('base64')
      }))
    })
    req.on('error', (e) => resolve({ error: e.message }))
    req.setTimeout(20000, () => req.destroy(new Error('timeout')))
  })
}

ipcMain.handle('update:check', async () => {
  const base = (appConfig.update || {}).base_url || 'http://cannula.special-partners.com'
  const r = await httpGet(`${base}/api/update/manifest?current=${APP_VERSION}`)
  if (r.error) return { error: r.error, base }
  try { return { ...JSON.parse(Buffer.from(r.bodyB64 || '', 'base64').toString()), base } }
  catch (e) { return { error: '매니페스트를 해석할 수 없습니다', base } }
})

// 매니페스트가 가리키는 업데이트 패키지를 내려받아 적용한다.
ipcMain.handle('update:apply', async (_e, artifactUrl) => {
  const r = await httpGet(artifactUrl)
  if (r.error) return { error: r.error }
  const dest = path.join(os.tmpdir(), 'cannula-update-downloaded.js')
  fs.writeFileSync(dest, Buffer.from(r.bodyB64 || '', 'base64'))
  logLine(`update:apply ${artifactUrl} -> ${dest}`)
  return new Promise((resolve) => {
    const child = spawn(process.execPath, [dest], {
      env: { ...process.env, ELECTRON_RUN_AS_NODE: '1' }
    })
    let out = ''
    child.stdout.on('data', (d) => (out += d))
    child.stderr.on('data', (d) => (out += d))
    child.on('error', (e) => resolve({ error: e.message, ran: dest }))
    child.on('close', (code) => resolve({ ran: dest, exitCode: code, output: out.trim() }))
  })
})

// --- 기동 ------------------------------------------------------------------
app.whenReady().then(() => {
  loadConfig()
  loadPlugins()
  logLine(`app start v${APP_VERSION} (runtime: ${RUNTIME_ROOT})`)
  createWindow()

  const autostart = ((appConfig.bridge || {}).autostart || 'true').toLowerCase() !== 'false'
  if (autostart) startBridge()
  setTimeout(connectBridge, autostart ? 900 : 100)
})

app.on('window-all-closed', () => {
  stopBridge()
  app.quit()
})
