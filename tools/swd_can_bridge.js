#!/usr/bin/env node
/**
 * swd_can_bridge.js — CAN over the debug port.
 *
 * Bridges the workstation gateway's udp transport to a CANnula pump that has no
 * USB-CAN adapter and no transceiver attached. Frames travel through a RAM ring
 * buffer in the STM32, reached over SWD via OpenOCD.
 *
 *   Workstation  --UDP 14-byte frames-->  this bridge  --SWD-->  g_SwdMailbox
 *
 * The firmware dispatches injected frames through the same handlers it uses for
 * frames off the wire, so the whole message set behaves normally.
 *
 *   node tools/swd_can_bridge.js [--verbose]
 *
 * Everything is discovered at run time: openocd is looked up on PATH and in the
 * usual xPack install, and the mailbox is found by scanning target RAM for its
 * magic word, so a rebuild that moves the symbol needs no new arguments.
 *
 * Educational lab tooling. Not for use with real medical devices.
 */

const { spawn } = require('child_process')
const net = require('net')
const dgram = require('dgram')
const fs = require('fs')
const path = require('path')
const os = require('os')

// --- args -------------------------------------------------------------------
function arg(name, dflt) {
  const i = process.argv.indexOf('--' + name)
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : dflt
}
const VERBOSE = process.argv.includes('--verbose')
const TCL_PORT = parseInt(arg('tcl', '6666'), 10)
const LISTEN = parseInt(arg('listen', '47102'), 10)
const [PEER_HOST, PEER_PORT] = String(arg('peer', '127.0.0.1:47101')).split(':')
const POLL_MS = parseInt(arg('poll', '20'), 10)

// RAM window to sweep for the mailbox. Covers every STM32F1 SRAM size.
const RAM_BASE = parseInt(arg('ram', '0x20000000'), 16)
const RAM_SIZE = parseInt(arg('ramsize', '0x10000'), 16)
const MB_MAGIC = 0x434e4d4c

// --- locate openocd ---------------------------------------------------------
function findOpenocd() {
  const explicit = arg('openocd', process.env.OPENOCD_EXE || '')
  if (explicit) return explicit
  const exe = process.platform === 'win32' ? 'openocd.exe' : 'openocd'
  const roots = [
    path.join(process.env.LOCALAPPDATA || '', 'arm-toolchain'),
    path.join(os.homedir(), '.local', 'xPacks'),
    '/usr/local', '/usr'
  ]
  for (const root of roots) {
    try {
      for (const d of fs.readdirSync(root)) {
        const p = path.join(root, d, 'bin', exe)
        if (fs.existsSync(p)) return p
      }
    } catch (e) { /* root absent */ }
  }
  return exe   // fall back to PATH
}
function findScripts(ocdPath) {
  const explicit = arg('scripts', '')
  if (explicit) return explicit
  const base = path.dirname(path.dirname(ocdPath))
  for (const cand of [path.join(base, 'openocd', 'scripts'),
                      path.join(base, 'scripts'),
                      path.join(base, 'share', 'openocd', 'scripts')]) {
    if (fs.existsSync(cand)) return cand
  }
  return ''
}
const OPENOCD = findOpenocd()
const SCRIPTS = findScripts(OPENOCD)

const SLOTS = 4
const FRAME_SZ = 16
const HDR_WORDS = 6
let MB_ADDR = parseInt(arg('mailbox', '0'), 16)   // optional override
let RX_BASE = 0
let TX_BASE = 0

const log = (...a) => console.log(new Date().toISOString().slice(11, 23), ...a)

// --- OpenOCD TCL RPC --------------------------------------------------------
// Commands are terminated by 0x1a in both directions.
class Tcl {
  constructor(port) {
    this.port = port
    this.sock = null
    this.buf = ''
    this.queue = []
  }
  connect() {
    return new Promise((resolve, reject) => {
      const s = net.connect(this.port, '127.0.0.1')
      s.setNoDelay(true)
      s.on('connect', () => { this.sock = s; resolve() })
      s.on('error', reject)
      s.on('data', (d) => {
        this.buf += d.toString('binary')
        let i
        while ((i = this.buf.indexOf('\x1a')) >= 0) {
          const msg = this.buf.slice(0, i)
          this.buf = this.buf.slice(i + 1)
          const p = this.queue.shift()
          if (p) p.resolve(msg)
        }
      })
      s.on('close', () => { this.sock = null })
    })
  }
  cmd(line) {
    if (!this.sock) return Promise.reject(new Error('tcl not connected'))
    return new Promise((resolve, reject) => {
      this.queue.push({ resolve, reject })
      this.sock.write(line + '\x1a')
    })
  }
}

// read_memory/write_memory return and take plain lists, unlike mdw/mww output.
async function readWords(tcl, addr, count) {
  const out = await tcl.cmd(`read_memory 0x${addr.toString(16)} 32 ${count}`)
  const toks = out.trim().split(/\s+/).filter(Boolean)
  return toks.map((t) => (parseInt(t, 16) >>> 0))
}
async function writeWords(tcl, addr, words) {
  const list = words.map((w) => '0x' + (w >>> 0).toString(16)).join(' ')
  await tcl.cmd(`write_memory 0x${addr.toString(16)} 32 {${list}}`)
}

// Sweep RAM for the mailbox magic. The symbol moves whenever the firmware is
// rebuilt, so finding it beats passing an address that goes stale.
async function findMailbox(tcl) {
  const CHUNK = 256   // words per read
  for (let off = 0; off < RAM_SIZE; off += CHUNK * 4) {
    let words
    try {
      words = await readWords(tcl, RAM_BASE + off, CHUNK)
    } catch (e) {
      break   // ran past the end of physical SRAM
    }
    if (!words.length) break
    for (let i = 0; i < words.length; i++) {
      // magic, then version, then four ring indices we can sanity-check
      if (words[i] === MB_MAGIC && words[i + 1] === 1) {
        const addr = RAM_BASE + off + i * 4
        log(`mailbox found by RAM scan at 0x${addr.toString(16)}`)
        return addr
      }
    }
  }
  return 0
}

// --- frame <-> words --------------------------------------------------------
function frameToWords(id, dlc, data) {
  const b = Buffer.alloc(FRAME_SZ)
  b.writeUInt32LE(id >>> 0, 0)
  b.writeUInt8(dlc & 0xff, 4)
  b.writeUInt8(0, 5)
  data.copy(b, 6, 0, Math.min(8, data.length))
  return [b.readUInt32LE(0), b.readUInt32LE(4), b.readUInt32LE(8), b.readUInt32LE(12)]
}
function wordsToFrame(words) {
  const b = Buffer.alloc(FRAME_SZ)
  words.forEach((w, i) => b.writeUInt32LE(w >>> 0, i * 4))
  return { id: b.readUInt32LE(0), dlc: b.readUInt8(4), data: b.slice(6, 14) }
}

// --- UDP wire format: u32 id (LE), u8 dlc, u8 rsv, u8 data[8] ---------------
function packUdp(id, dlc, data) {
  const p = Buffer.alloc(14)
  p.writeUInt32LE(id >>> 0, 0)
  p.writeUInt8(dlc & 0xff, 4)
  p.writeUInt8(0, 5)
  data.copy(p, 6, 0, 8)
  return p
}

async function main() {
  const ocdArgs = []
  if (SCRIPTS) ocdArgs.push('-s', SCRIPTS)
  ocdArgs.push(
    '-f', 'interface/stlink.cfg',
    '-f', 'target/stm32f1x.cfg',
    '-c', 'gdb port disabled',
    '-c', 'telnet port disabled',
    '-c', `tcl port ${TCL_PORT}`,
    '-c', 'init'
  )
  log('starting openocd:', OPENOCD, ocdArgs.join(' '))
  const ocd = spawn(OPENOCD, ocdArgs, { stdio: ['ignore', 'pipe', 'pipe'] })
  ocd.stdout.on('data', (d) => VERBOSE && process.stdout.write('[ocd] ' + d))
  ocd.stderr.on('data', (d) => VERBOSE && process.stdout.write('[ocd] ' + d))
  ocd.on('exit', (c) => { log('openocd exited', c); process.exit(1) })

  const tcl = new Tcl(TCL_PORT)
  for (let i = 0; i < 40; i++) {
    try { await tcl.connect(); break } catch (e) { await new Promise((r) => setTimeout(r, 250)) }
  }
  if (!tcl.sock) { console.error('could not reach openocd tcl port'); process.exit(1) }
  log('openocd tcl connected on', TCL_PORT)

  if (!MB_ADDR) MB_ADDR = await findMailbox(tcl)
  if (!MB_ADDR) {
    console.error('mailbox not found in target RAM — is the firmware with SwdMb_Init() flashed?')
    process.exit(1)
  }
  RX_BASE = MB_ADDR + HDR_WORDS * 4
  TX_BASE = RX_BASE + SLOTS * FRAME_SZ

  const hdr = await readWords(tcl, MB_ADDR, HDR_WORDS)
  if (hdr[0] !== MB_MAGIC) {
    console.error(`mailbox magic mismatch at 0x${MB_ADDR.toString(16)}: got 0x${hdr[0].toString(16)}`)
    process.exit(1)
  }
  log(`mailbox @0x${MB_ADDR.toString(16)}  version=${hdr[1]}  rx@0x${RX_BASE.toString(16)} tx@0x${TX_BASE.toString(16)}`)

  // Adopt whatever the firmware has already queued rather than replaying it.
  let hostTail = hdr[4] >>> 0
  let hostHead = hdr[2] >>> 0
  await writeWords(tcl, MB_ADDR + 5 * 4, [hostTail])

  const udp = dgram.createSocket('udp4')
  const pending = []
  udp.on('message', (msg) => {
    if (msg.length < 6) return
    const id = msg.readUInt32LE(0)
    const dlc = msg.readUInt8(4)
    const data = Buffer.alloc(8)
    msg.copy(data, 0, 6, Math.min(msg.length, 14))
    pending.push({ id, dlc, data })
  })
  udp.bind(LISTEN, () => log(`udp listening on ${LISTEN}, sending to ${PEER_HOST}:${PEER_PORT}`))

  let rx = 0, tx = 0
  async function tick() {
    try {
      const h = await readWords(tcl, MB_ADDR, HDR_WORDS)
      const fwHead = h[4] >>> 0
      const fwTail = h[3] >>> 0
      hostHead = h[2] >>> 0

      // pump -> workstation
      while (hostTail !== fwHead) {
        const slot = hostTail & (SLOTS - 1)
        const words = await readWords(tcl, TX_BASE + slot * FRAME_SZ, 4)
        const f = wordsToFrame(words)
        hostTail = (hostTail + 1) >>> 0
        await writeWords(tcl, MB_ADDR + 5 * 4, [hostTail])
        udp.send(packUdp(f.id, f.dlc, f.data), Number(PEER_PORT), PEER_HOST)
        tx++
        if (VERBOSE) log(`pump->ws  0x${f.id.toString(16)} [${f.dlc}] ${f.data.toString('hex')}`)
      }

      // workstation -> pump
      while (pending.length && (hostHead - fwTail) >>> 0 < SLOTS) {
        const f = pending.shift()
        const slot = hostHead & (SLOTS - 1)
        await writeWords(tcl, RX_BASE + slot * FRAME_SZ, frameToWords(f.id, f.dlc, f.data))
        hostHead = (hostHead + 1) >>> 0
        await writeWords(tcl, MB_ADDR + 2 * 4, [hostHead])
        rx++
        if (VERBOSE) log(`ws->pump  0x${f.id.toString(16)} [${f.dlc}] ${f.data.toString('hex')}`)
      }
    } catch (e) {
      log('tick error:', e.message)
    }
    setTimeout(tick, POLL_MS)
  }
  tick()

  setInterval(() => log(`frames  pump->ws=${tx}  ws->pump=${rx}`), 5000)
}

main().catch((e) => { console.error(e); process.exit(1) })
