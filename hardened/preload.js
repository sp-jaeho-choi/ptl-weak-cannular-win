// CANnula Clinical Workstation v0.0.2 (강화) — 프리로드
//
// 렌더러에는 Node 를 노출하지 않는다. 여기서 열어 주는 좁은 API 만 쓸 수 있다.
// 각 채널은 메인 프로세스에서 인자 형식과 권한을 다시 검사한다.
const { contextBridge, ipcRenderer } = require('electron')

// 렌더러가 등록한 구독을 한곳에서 관리해 임의 채널 수신을 막는다.
const SUB_CHANNELS = ['bridge:state', 'bridge:status', 'bridge:hello', 'bridge:log', 'bridge:frame']

contextBridge.exposeInMainWorld('cannula', {
  // --- 조회 -----------------------------------------------------------------
  runtime: () => ipcRenderer.invoke('app:runtime'),
  bridgeStatus: () => ipcRenderer.invoke('bridge:up'),
  bridgeState: () => ipcRenderer.invoke('bridge:state'),

  // --- 인증 -----------------------------------------------------------------
  // 비밀번호는 메인 프로세스로만 간다. 렌더러는 계정 목록도 해시도 볼 수 없다.
  login: (user, password) => ipcRenderer.invoke('auth:login', { user, password }),
  logout: () => ipcRenderer.invoke('auth:logout'),
  session: () => ipcRenderer.invoke('auth:session'),

  // --- 임상 동작 ------------------------------------------------------------
  setRate: (rate, vtbi, drug, weight) =>
    ipcRenderer.invoke('pump:setrate', { rate, vtbi, drug, weight }),
  control: (cmd, arg) => ipcRenderer.invoke('pump:control', { cmd, arg }),
  ackAlarm: (alarm) => ipcRenderer.invoke('pump:ack', { alarm }),
  drugs: () => ipcRenderer.invoke('pump:drugs'),

  // --- 서비스 (기사 권한) ----------------------------------------------------
  serviceCommands: () => ipcRenderer.invoke('service:list'),
  serviceRun: (cmd) => ipcRenderer.invoke('service:run', { cmd }),
  diagTools: () => ipcRenderer.invoke('diag:list'),
  diagRun: (tool) => ipcRenderer.invoke('diag:run', { tool }),
  transport: (mode, serial, bitrate) =>
    ipcRenderer.invoke('bridge:transport', { mode, serial, bitrate }),

  // --- 환자 · 로그 ----------------------------------------------------------
  patientSave: (p) => ipcRenderer.invoke('patient:save', p),
  patientLoad: () => ipcRenderer.invoke('patient:load'),
  logExport: (rows) => ipcRenderer.invoke('log:export', { rows }),
  logList: () => ipcRenderer.invoke('log:list'),
  logEvent: (kind, text) => ipcRenderer.invoke('log:event', { kind, text }),

  // --- 업데이트 -------------------------------------------------------------
  updateCheck: () => ipcRenderer.invoke('update:check'),
  updateApply: () => ipcRenderer.invoke('update:apply'),

  // --- 구독 -----------------------------------------------------------------
  on: (channel, handler) => {
    if (!SUB_CHANNELS.includes(channel)) return () => {}
    if (typeof handler !== 'function') return () => {}
    const wrapped = (_event, payload) => handler(payload)
    ipcRenderer.on(channel, wrapped)
    return () => ipcRenderer.removeListener(channel, wrapped)
  }
})
