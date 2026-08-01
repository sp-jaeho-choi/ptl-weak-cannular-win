#!/usr/bin/env node
// 배포 서명 도구 (강화 빌드)
//
//   node deploy/sign.js <파일>...              분리 서명(.sig)을 만든다
//   node deploy/sign.js --manifest <json>      업데이트 매니페스트에 서명을 넣는다
//   node deploy/sign.js --verify <파일>        신뢰 목록으로 검증만 해 본다
//
// 비공개키는 deploy/signing/release-private.pem 에 있다. 이 폴더는 배포본에
// 포함되지 않는다 (electron-builder extraResources 에서 제외).
'use strict'

const crypto = require('crypto')
const fs = require('fs')
const path = require('path')

const ROOT = path.join(__dirname, '..')
const PRIV = path.join(__dirname, 'signing', 'release-private.pem')
const TRUST = path.join(ROOT, 'runtime', 'config', 'trusted-keys.json')

function privKey() {
  return crypto.createPrivateKey(fs.readFileSync(PRIV, 'utf8'))
}

function trustedKeys() {
  const j = JSON.parse(fs.readFileSync(TRUST, 'utf8'))
  return (j.keys || []).map((e) => ({
    id: e.id,
    key: crypto.createPublicKey({ key: e.pem, format: 'pem', type: 'spki' })
  }))
}

function signFile(p) {
  const data = fs.readFileSync(p)
  const sig = crypto.sign(null, data, privKey())
  fs.writeFileSync(p + '.sig', sig.toString('base64') + '\n')
  console.log(`서명: ${path.relative(ROOT, p)} → ${path.basename(p)}.sig`)
}

function verifyFile(p) {
  const data = fs.readFileSync(p)
  let sig
  try { sig = Buffer.from(fs.readFileSync(p + '.sig', 'utf8').trim(), 'base64') }
  catch (e) { console.log(`검증 실패: ${path.basename(p)} — .sig 없음`); return false }
  for (const t of trustedKeys()) {
    if (crypto.verify(null, data, t.key, sig)) {
      console.log(`검증 통과: ${path.basename(p)} (서명자 ${t.id})`)
      return true
    }
  }
  console.log(`검증 실패: ${path.basename(p)} — 신뢰 목록의 어느 키와도 맞지 않음`)
  return false
}

// 업데이트 매니페스트: signature 필드를 뺀 본문에 서명하고 다시 넣는다.
// (main.js 의 update:check 가 같은 방식으로 검증한다)
function signManifest(p) {
  const m = JSON.parse(fs.readFileSync(p, 'utf8'))
  delete m.signature
  const canonical = Buffer.from(JSON.stringify(m), 'utf8')
  m.signature = crypto.sign(null, canonical, privKey()).toString('base64')
  fs.writeFileSync(p, JSON.stringify(m, null, 2) + '\n')
  console.log(`매니페스트 서명: ${path.basename(p)}`)
}

const args = process.argv.slice(2)
if (!args.length) {
  console.log('사용법: node deploy/sign.js <파일>... | --manifest <json> | --verify <파일>')
  process.exit(1)
}

if (args[0] === '--manifest') {
  signManifest(path.resolve(args[1]))
} else if (args[0] === '--verify') {
  const ok = args.slice(1).every((f) => verifyFile(path.resolve(f)))
  process.exit(ok ? 0 : 1)
} else {
  for (const f of args) signFile(path.resolve(f))
}
