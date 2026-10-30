// The fm1buddy installer page. The firmware is the latest release's, copied next to the page by the pages
// workflow (fw/): GitHub's release downloads can't be fetched from another site, its API can.

import { init as initMap } from './midimap.js'

const REPO = 'konsumer/fm1buddy'
const FW = {
  s3: 'fw/fm1buddy-cyd-s3.bin',
  manifest: 'fw/manifest.json',
  version: 'fw/version.json'
}

const $ = (s) => document.querySelector(s)
const $$ = (s) => [...document.querySelectorAll(s)]

// ------------------------------------------------------------------------------------------- release ---
async function json(url) {
  const r = await fetch(url, { cache: 'no-cache' })
  if (!r.ok) {
    throw new Error(r.status)
  }
  return r.json()
}

function noFirmware(sel = '.fw-s3 button, .fw-s3-bin') {
  for (const el of $$(sel)) {
    el.setAttribute('aria-disabled', 'true')
    el.disabled = true
    el.removeAttribute('href')
  }
}

async function release() {
  const el = $('#release')
  let here = null
  try {
    const v = await json(FW.version)
    here = v.tag
    cases(v.cases || [])
  } catch (_) {
    cases([])
  }
  let latest = null
  try {
    latest = await json(`https://api.github.com/repos/${REPO}/releases/latest`)
  } catch (_) {}

  if (!here) {
    noFirmware()
    el.innerHTML = latest ? `The latest release, <a href="${latest.html_url}">${latest.tag_name}</a>, isn't on this page yet: get the files from there.` : `There's no release yet: <a href="https://github.com/${REPO}">build it from the source</a>.`
    return
  }
  $('.fw-s3-bin').href = FW.s3
  $('.fw-s3').setAttribute('manifest', FW.manifest)

  const date = latest && latest.tag_name === here ? ` · ${new Date(latest.published_at).toLocaleDateString()}` : ''
  const url = `https://github.com/${REPO}/releases/tag/${encodeURIComponent(here)}`
  el.innerHTML = `Version <a href="${url}">${here}</a>${date}`
  if (latest && latest.tag_name !== here) {
    el.innerHTML += ` · a newer one, <a href="${latest.html_url}">${latest.tag_name}</a>, is on its way here`
  }
}

// ------------------------------------------------------------------------------------------- cases ---
const SCAD = `https://github.com/${REPO}/blob/main/case/fm1buddy-case.scad`

function cases(files) {
  for (const box of $$('.case-links')) {
    const config = box.dataset.case || box.closest('.case').dataset.case
    const has = (name) => files.includes(`fm1buddy-case-${name}.stl`)
    const link = (name, label) => `<a class="button${label.startsWith('With') ? ' secondary' : ''}" href="fw/fm1buddy-case-${name}.stl" download>${label}</a>`
    if (!has(`${config}-base`)) {
      box.innerHTML = `<span class="note">The STLs come with the next release; or make them from <a href="${SCAD}">the OpenSCAD file</a>.</span>`
      continue
    }
    box.innerHTML = [link(`${config}-base`, 'Base'), link(`${config}-lid`, 'Lid')].join(' ')
  }
}

// ------------------------------------------------------------------------------------------- tabs ---
function tab() {
  const name = location.hash === '#midimap' ? 'map' : 'install'
  for (const el of $$('[data-page]')) {
    el.hidden = el.dataset.page !== name
  }
  for (const a of $$('.tabs a')) {
    a.classList.toggle('on', a.dataset.tab === name)
  }
  document.body.dataset.tab = name
}

window.addEventListener('hashchange', tab)
tab()
initMap()
release()
