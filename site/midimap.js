// The MIDI map tab: builds midimap.txt for the SD card. Rules read and write the same way as src/app/midimap.cpp
// (parse and text there), so what this makes is what the board reads. Devices come from Web MIDI: each one's short
// name is the part of its name a rule uses ("from nanokontrol2"), and moving a control while learning makes a rule.

const $ = (s, el = document) => el.querySelector(s)

const TYPES = ['note', 'cc', 'pc', 'at', 'pb']
const TYPE_WORDS = { note: ['note', 'notes'], cc: ['CC', 'CCs'], pc: ['program', 'programs'], at: ['pressure', 'pressure'], pb: ['pitch bend', 'pitch bend'] }
const HAS_NUM = { note: true, cc: true, pc: true }
const MAX_RULES = 64 // MAP_RULES
const MAX_LINE = 158 // the board reads lines of up to 159 bytes, newline included
const NAME_LEN = 23 // MapRule's from/to

// fm1buddy's touch controller, as it is out of the box (page_touch.cpp)
const TOUCH = [
  ['Finger 1: X', 'cc', 74],
  ['Finger 1: Y', 'cc', 71],
  ['Finger 2: X', 'cc', 1],
  ['Finger 2: Y', 'cc', 11],
  ['Pads', 'note', '36-51']
]

const STORE = 'fm1buddy-midimap'

// ------------------------------------------------------------------------------------------- rules ---
const newRule = (o = {}) => ({ t: 'rule', comment: '', gap: false, from: '', inCh: 0, inType: '', inNum: '', inHi: '', drop: false, to: '', outCh: 0, outType: '', outNum: '', transpose: 0, vel: '', ...o })

// words, lowercase; '...' and "..." keep their spaces (words() in midimap.cpp)
function words(s) {
  const w = []
  let i = 0
  while (i < s.length) {
    while (i < s.length && /\s/.test(s[i])) i++
    if (i >= s.length) break
    const q = s[i] === '"' || s[i] === "'" ? s[i++] : ''
    let k = ''
    while (i < s.length && (q ? s[i] !== q : !/\s/.test(s[i]))) k += s[i++]
    if (q && i < s.length) i++
    w.push(k.toLowerCase().slice(0, NAME_LEN))
  }
  return w
}

const num = (w, max) => (/^\d+$/.test(w) && +w <= max ? +w : null)

// [from|to NAME] [ch N] [TYPE [NUM[-NUM]]] (and on the right +N, vel N): the words used, or throws why
function side(w, out, r) {
  let i = 0
  if (w[i] === (out ? 'to' : 'from')) {
    if (i + 1 >= w.length) throw new Error('a name after from/to')
    r[out ? 'to' : 'from'] = w[i + 1]
    i += 2
  }
  if (w[i] === 'ch') {
    const ch = num(w[i + 1] || '', 16)
    if (!ch) throw new Error('ch is 1-16')
    r[out ? 'outCh' : 'inCh'] = ch
    i += 2
  }
  if (TYPES.includes(w[i])) {
    r[out ? 'outType' : 'inType'] = w[i++]
    if (i < w.length && /^\d/.test(w[i])) {
      const [a, b] = out ? [w[i]] : w[i].split(/-(.*)/s)
      const lo = num(a, 127)
      const hi = b === undefined ? lo : num(b, 127)
      if (lo === null || hi === null) throw new Error('numbers are 0-127')
      if (hi < lo) throw new Error('a range goes low-high')
      if (out) {
        r.outNum = lo
      } else {
        r.inNum = lo
        r.inHi = hi === lo ? '' : hi
      }
      i++
    }
  }
  while (out && i < w.length) {
    if (/^[+-]./.test(w[i])) {
      const v = num(w[i].slice(1), 127)
      if (v === null) throw new Error('+N or -N moves notes N semitones')
      r.transpose = w[i][0] === '-' ? -v : v
      i++
    } else if (w[i] === 'vel') {
      const s = w[i + 1] || ''
      const pct = s.endsWith('%')
      const v = num(pct ? s.slice(0, -1) : s, pct ? 400 : 127)
      if (!v) throw new Error('vel is 1-127, or 1-400%')
      r.vel = pct ? `${v}%` : String(v)
      i += 2
    } else {
      break
    }
  }
  return i
}

// a line: a rule, null for a blank line or a comment, or throws why it isn't one
export function parse(line) {
  const s = line.split('#')[0]
  const arrow = s.indexOf('->')
  if (arrow < 0) {
    if (words(s).length) throw new Error('a rule needs ->')
    return null
  }
  const r = newRule()
  let w = words(s.slice(0, arrow))
  if (side(w, false, r) < w.length) throw new Error('something left over before ->')
  w = words(s.slice(arrow + 2))
  if (w.length === 1 && w[0] === 'drop') {
    r.drop = true
    return r
  }
  if (side(w, true, r) < w.length) throw new Error('something left over after ->')
  return r
}

const word = (s) => (s.includes(' ') ? `"${s}"` : s)
const range = (r) => (r.inNum === '' ? '' : r.inHi !== '' && +r.inHi !== +r.inNum ? `${r.inNum}-${r.inHi}` : `${r.inNum}`)
const noteOut = (r) => r.outType === 'note' || (!r.outType && (r.inType === 'note' || !r.inType))

// a rule as a line of the file (text() in midimap.cpp)
export function text(r) {
  const s = []
  if (r.from) s.push('from', word(r.from))
  if (r.inCh) s.push('ch', r.inCh)
  if (r.inType) {
    s.push(r.inType)
    if (HAS_NUM[r.inType] && r.inNum !== '') s.push(range(r))
  }
  s.push('->')
  if (r.drop) return [...s, 'drop'].join(' ')
  if (r.to) s.push('to', word(r.to))
  if (r.outCh) s.push('ch', r.outCh)
  if (r.outType) {
    s.push(r.outType)
    if (HAS_NUM[r.outType] && r.outNum !== '') s.push(r.outNum)
  }
  if (noteOut(r)) {
    if (r.transpose) s.push((r.transpose > 0 ? '+' : '-') + Math.abs(r.transpose))
    if (r.vel) s.push('vel', r.vel)
  }
  return s.join(' ')
}

// in plain words (describe() in midimap.cpp)
function describe(r) {
  const what = (type, n, hi) => {
    if (!type) return ''
    if (!HAS_NUM[type] || n === '') return TYPE_WORDS[type][1]
    return hi !== '' && +hi !== +n ? `${TYPE_WORDS[type][1]} ${n}-${hi}` : `${TYPE_WORDS[type][0]} ${n}`
  }
  const join = (...a) => a.filter(Boolean).join(', ')
  const input = join((r.from || 'anything') + (r.inCh ? ` ch ${r.inCh}` : ''), what(r.inType, r.inNum, r.inHi))
  if (r.drop) return `${input} → dropped`
  const extra = noteOut(r) ? [r.transpose ? (r.transpose > 0 ? '+' : '') + r.transpose + ' semitones' : '', r.vel ? `velocity ${r.vel}` : ''] : []
  return `${input} → ${join((r.to || 'the others') + (r.outCh ? ` ch ${r.outCh}` : ''), what(r.outType, r.outNum, ''), ...extra)}`
}

// what's wrong with a rule, as the board would say (by reading back what it writes)
function problem(r) {
  const line = text(r)
  if (line.length > MAX_LINE) return 'too long for the board (shorten the names)'
  if (/[#"']|->/.test(r.from + r.to)) return "names can't have # quotes or ->"
  try {
    parse(line)
  } catch (e) {
    return e.message
  }
  return ''
}

// does an earlier rule catch everything this one would? (the first rule that matches decides)
function caughtBy(a, b) {
  if (a.from && !b.from.includes(a.from)) return false
  if (a.inCh && a.inCh !== b.inCh) return false
  if (!a.inType) return true
  if (a.inType !== b.inType) return false
  if (!HAS_NUM[a.inType] || a.inNum === '') return true
  if (b.inNum === '') return false
  const hi = (r) => (r.inHi === '' ? +r.inNum : +r.inHi)
  return +a.inNum <= +b.inNum && hi(b) <= hi(a)
}

// a midimap.txt as items: notes (comment blocks) and rules (each with the comment line just above it)
export function load(src) {
  const items = []
  const problems = []
  let block = []
  let gap = false
  const flush = () => {
    if (block.length) items.push({ t: 'note', gap, text: block.join('\n') })
    block = []
    gap = false
  }
  src.split(/\r?\n/).forEach((line, n) => {
    const t = line.trim()
    if (!t) {
      flush()
      gap = items.length > 0
      return
    }
    if (t.startsWith('#')) {
      block.push(t.replace(/^# ?/, ''))
      return
    }
    let r
    try {
      r = parse(line)
    } catch (e) {
      problems.push(`line ${n + 1}: ${e.message} (kept as a comment)`)
      block.push(t)
      return
    }
    if (!r) return
    r.comment = block.length ? block.pop() : ''
    flush()
    r.gap = gap
    gap = false
    items.push(r)
  })
  flush()
  if (items[0]) items[0].gap = false
  return { items, problems }
}

export function save(items) {
  const out = []
  items.forEach((it, i) => {
    if (i && it.gap) out.push('')
    if (it.t === 'note') {
      for (const l of it.text.split('\n')) out.push(l ? `# ${l}` : '#')
    } else {
      if (it.comment) out.push(`# ${it.comment}`)
      out.push(text(it))
    }
  })
  return out.join('\n') + '\n'
}

// ------------------------------------------------------------------------------------------- devices ---
// the word of a device's name a rule needs: "nanoKONTROL2 SLIDER/KNOB" (KORG INC.) -> nanokontrol2,
// "Arturia KeyStep 32" (Arturia) -> keystep
export function shortName(name, maker = '') {
  const skip = new Set(['usb', 'midi', 'in', 'out', 'input', 'port', 'device', 'the', ...maker.toLowerCase().split(/\W+/)])
  const w = name
    .replace(/^\d+\s*-\s*/, '')
    .replace(/\(.*?\)/g, ' ')
    .toLowerCase()
    .split(/[\s_/,:]+/)
    .map((s) => s.replace(/[^a-z0-9.+-]/g, ''))
    .filter(Boolean)
  return (w.find((s) => !skip.has(s) && !/^\d+$/.test(s)) || w[0] || 'usb').slice(0, NAME_LEN)
}

const clean = (s) =>
  s
    .toLowerCase()
    .replace(/[#"']|->/g, '')
    .trim()
    .slice(0, NAME_LEN)

// ------------------------------------------------------------------------------------------- state ---
let state = { items: [], devices: [] } // devices: { name, short, builtin?, manual? }
const live = new Map() // Web MIDI name -> { port, last, lit }
let learning = null // a rule's index, 'new', or null
let access = null

const BUILTIN = [{ name: 'fm1buddy touch controller', short: 'touch', builtin: true }]

function fresh() {
  return {
    items: [{ t: 'note', gap: false, text: `fm1buddy MIDI mapping, made at ${location.origin}${location.pathname}#midimap` }],
    devices: []
  }
}

function persist() {
  try {
    localStorage.setItem(STORE, JSON.stringify(state))
  } catch (_) {}
}

function restore() {
  try {
    const s = JSON.parse(localStorage.getItem(STORE))
    if (s && Array.isArray(s.items)) return { items: s.items, devices: s.devices || [] }
  } catch (_) {}
  return fresh()
}

const devices = () => [...BUILTIN, ...state.devices]
const rules = () => state.items.filter((it) => it.t === 'rule')

// ------------------------------------------------------------------------------------------- view ---
const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c])

function options(list, value) {
  return list.map(([v, label]) => `<option value="${esc(v)}"${String(v) === String(value) ? ' selected' : ''}>${esc(label)}</option>`).join('')
}

const CHANNELS = (none) => [[0, none], ...Array.from({ length: 16 }, (_, i) => [i + 1, String(i + 1)])]
const field = (label, html, cls = '') => `<label class="f ${cls}"><span>${label}</span>${html}</label>`
const box = (k, v, attrs = '') => `<input data-k="${k}" value="${esc(v)}" ${attrs} />`
const numBox = (k, v, max, min = 0) => box(k, v, `type="number" min="${min}" max="${max}" inputmode="numeric" class="n"`)

function ruleCard(r, i) {
  const inTypes = [['', 'anything'], ...TYPES.map((t) => [t, TYPE_WORDS[t][1]])]
  const outTypes = [['', 'the same'], ...TYPES.map((t) => [t, TYPE_WORDS[t][0]])]
  const left = [field('from', box('from', r.from, 'list="map-devs" placeholder="any device" spellcheck="false"'), 'name'), field('ch', `<select data-k="inCh">${options(CHANNELS('any'), r.inCh)}</select>`), field('what', `<select data-k="inType">${options(inTypes, r.inType)}</select>`)]
  if (HAS_NUM[r.inType]) {
    left.push(field(r.inType === 'note' ? 'notes' : 'number', `${numBox('inNum', r.inNum, 127)}<em>–</em>${numBox('inHi', r.inHi, 127)}`, 'range'))
  }
  let right = [
    field(
      'then',
      `<select data-k="drop">${options(
        [
          ['', 'send it'],
          ['1', 'drop it']
        ],
        r.drop ? '1' : ''
      )}</select>`
    )
  ]
  if (!r.drop) {
    right.push(field('to', box('to', r.to, 'list="map-devs" placeholder="all the others" spellcheck="false"'), 'name'), field('ch', `<select data-k="outCh">${options(CHANNELS('same'), r.outCh)}</select>`), field('as', `<select data-k="outType">${options(outTypes, r.outType)}</select>`))
    if (HAS_NUM[r.outType]) right.push(field('number', numBox('outNum', r.outNum, 127)))
    if (noteOut(r)) {
      const pct = r.vel.endsWith('%')
      right.push(
        field('semitones', numBox('transpose', r.transpose || '', 127, -127)),
        field(
          'velocity',
          `<select data-k="velMode">${options(
            [
              ['', 'as played'],
              ['n', 'fixed'],
              ['%', 'scaled %']
            ],
            r.vel ? (pct ? '%' : 'n') : ''
          )}</select>${r.vel ? numBox('velNum', parseInt(r.vel), pct ? 400 : 127, 1) : ''}`,
          'vel'
        )
      )
    }
  }
  return `<div class="rule${learning === i ? ' learning' : ''}" data-i="${i}">
    <div class="rule-top">
      <input class="comment" data-k="comment" value="${esc(r.comment)}" placeholder="a comment (what it's for)" />
      <span class="tools">
        <button type="button" data-do="learn" title="move a control or play a key on a device here">${learning === i ? 'Listening…' : 'Learn'}</button>
        <button type="button" data-do="up" title="up" aria-label="move up">↑</button>
        <button type="button" data-do="down" title="down" aria-label="move down">↓</button>
        <button type="button" data-do="del" title="delete" aria-label="delete">✕</button>
      </span>
    </div>
    <div class="side">${left.join('')}</div>
    <div class="side"><b class="arrow">→</b>${right.join('')}</div>
    <p class="said"></p>
  </div>`
}

function noteCard(n, i) {
  return `<div class="rule note-item" data-i="${i}">
    <div class="rule-top">
      <textarea class="comment" data-k="text" rows="${Math.max(1, n.text.split('\n').length)}" placeholder="a comment">${esc(n.text)}</textarea>
      <span class="tools">
        <button type="button" data-do="up" aria-label="move up">↑</button>
        <button type="button" data-do="down" aria-label="move down">↓</button>
        <button type="button" data-do="del" aria-label="delete">✕</button>
      </span>
    </div>
  </div>`
}

function renderDevices() {
  const list = devices()
  $('#map-devs').innerHTML = [...new Set([...list.map((d) => d.short), 'fm-1'])].map((s) => `<option value="${esc(s)}"></option>`).join('')
  $('#devices').innerHTML = list
    .map((d, i) => {
      const l = live.get(d.name)
      const others = list.filter((o) => o !== d && o.name.toLowerCase().includes(d.short) && o.short !== d.short)
      const extra = d.builtin ? `<span class="chips">${TOUCH.map(([label, type, n], k) => `<button type="button" data-do="touch" data-k="${k}">${label} <small>${type} ${n}</small></button>`).join('')}</span>` : ''
      return `<li class="${d.builtin || l ? '' : 'gone'}" data-d="${i}">
        <span class="light${l && l.lit ? ' on' : ''}"></span>
        <input class="short" value="${esc(d.short)}" aria-label="short name" spellcheck="false" ${d.builtin ? 'readonly' : ''} />
        <span class="full">${esc(d.name)}${d.builtin ? '' : l ? '' : d.manual ? ' · added by hand' : ' · not plugged in'}<span class="last">${l && l.last ? ` · ${esc(l.last)}` : ''}</span>${others.length ? `<span class="bad"> · also matches ${others.map((o) => esc(o.name)).join(', ')}</span>` : ''}</span>
        <span class="tools">
          <button type="button" data-do="own">Onto a channel</button>
          <button type="button" data-do="rest">Drop the rest</button>
          <button type="button" data-do="rule">Add a rule</button>
          ${d.builtin ? '' : '<button type="button" data-do="forget" aria-label="forget">✕</button>'}
        </span>
        ${extra}
      </li>`
    })
    .join('')
}

function render() {
  const list = rules()
  $('#rules').innerHTML = state.items.map((it, i) => (it.t === 'note' ? noteCard(it, i) : ruleCard(it, i))).join('') || '<p class="note">No rules yet: without any, everything any device plays goes to all the others.</p>'
  $('#learn-new').textContent = learning === 'new' ? 'Listening… (move a control)' : 'Learn a rule'
  $('#learn-new').classList.toggle('on', learning === 'new')
  renderDevices()
  refresh()
}

// what changes as you type: each rule's plain words and problems, and the file
function refresh() {
  const seen = []
  for (const card of document.querySelectorAll('#rules .rule[data-i]')) {
    const r = state.items[+card.dataset.i]
    if (r.t !== 'rule') continue
    const why = problem(r)
    const by = seen.findIndex((a) => caughtBy(a, r))
    const said = $('.said', card)
    said.innerHTML = `<code>${esc(text(r))}</code> ${esc(describe(r))}` + (why ? `<span class="bad"> · ${esc(why)}</span>` : by >= 0 ? `<span class="bad"> · never used: rule ${by + 1} above catches these first</span>` : '')
    seen.push(r)
  }
  const n = rules().length
  const txt = save(state.items)
  $('#map-text').textContent = txt
  $('#map-problem').textContent = n > MAX_RULES ? `${n} rules: the board reads the first ${MAX_RULES}.` : ''
  persist()
}

// ------------------------------------------------------------------------------------------- MIDI ---
const STATUS_TYPE = { 9: 'note', 11: 'cc', 12: 'pc', 13: 'at', 14: 'pb' }

function said(d) {
  const k = d[0] >> 4
  const ch = (d[0] & 15) + 1
  if (k === 9 && d[2]) return `note ${d[1]} ch ${ch}`
  if (k === 11) return `cc ${d[1]} = ${d[2]} ch ${ch}`
  if (k === 12) return `program ${d[1]} ch ${ch}`
  if (k === 13) return `pressure ${d[1]} ch ${ch}`
  if (k === 14) return `pitch bend ch ${ch}`
  return ''
}

function heard(name, e) {
  const d = e.data
  if (!d.length || d[0] >= 0xf0) return // clock, sysex: the board passes those on as they are
  const k = d[0] >> 4
  const l = live.get(name)
  const msg = said(d)
  if (!msg) return
  if (l) {
    l.last = msg
    l.lit = true
    clearTimeout(l.timer)
    l.timer = setTimeout(() => {
      l.lit = false
      lightUp(name)
    }, 150)
    lightUp(name)
  }
  if (learning === null || !STATUS_TYPE[k]) return
  const dev = state.devices.find((x) => x.name === name)
  const got = { from: dev ? dev.short : '', inCh: (d[0] & 15) + 1, inType: STATUS_TYPE[k], inNum: k === 9 || k === 11 ? d[1] : '', inHi: '' }
  if (learning === 'new') {
    state.items.push(newRule({ ...got, comment: `${dev ? dev.short : ''} ${msg.replace(/ ch \d+$/, '').replace(/ = \d+/, '')}`.trim() }))
  } else {
    Object.assign(state.items[learning], got)
  }
  learning = null
  render()
}

function lightUp(name) {
  const i = devices().findIndex((d) => d.name === name)
  const li = document.querySelector(`#devices li[data-d="${i}"]`)
  if (!li) return
  const l = live.get(name)
  $('.light', li).classList.toggle('on', l.lit)
  $('.last', li).textContent = l.last ? ` · ${l.last}` : ''
}

function scan() {
  const seen = new Set()
  for (const port of access.inputs.values()) {
    const name = port.name || 'USB MIDI'
    if (seen.has(name)) continue
    seen.add(name)
    if (!state.devices.find((d) => d.name === name)) {
      state.devices.push({ name, short: name.toLowerCase().startsWith('fm1buddy') ? 'touch' : shortName(name, port.manufacturer || '') })
    }
    if (!live.has(name)) live.set(name, { port })
    port.onmidimessage = (e) => heard(name, e)
  }
  for (const name of [...live.keys()]) {
    if (!seen.has(name)) live.delete(name)
  }
  const n = seen.size
  $('#midi-status').textContent = n ? `${n} device${n > 1 ? 's' : ''} here. Move a control to see what it sends.` : 'None here yet: plug one in.'
  render()
}

async function connect() {
  if (!navigator.requestMIDIAccess) {
    $('#midi-status').textContent = 'This browser has no Web MIDI (use Chrome, Edge or Firefox): add devices by name instead.'
    return
  }
  $('#midi-status').textContent = 'Asking the browser (allow MIDI at the top of the window)…'
  try {
    access = await navigator.requestMIDIAccess()
  } catch (e) {
    $('#midi-status').textContent = `No MIDI: ${e.message || 'not allowed'}.`
    return
  }
  access.onstatechange = scan
  $('#midi-connect').textContent = 'Look again'
  scan()
}

// ------------------------------------------------------------------------------------------- input ---
function move(i, by) {
  const j = i + by
  if (j < 0 || j >= state.items.length) return
  ;[state.items[i], state.items[j]] = [state.items[j], state.items[i]]
  if (learning === i) learning = j
  render()
}

function onRules(e) {
  const card = e.target.closest('[data-i]')
  if (!card) return
  const i = +card.dataset.i
  const it = state.items[i]
  const act = e.target.dataset.do
  if (e.type === 'click') {
    if (act === 'del') {
      state.items.splice(i, 1)
      learning = null
      render()
    } else if (act === 'up' || act === 'down') {
      move(i, act === 'up' ? -1 : 1)
    } else if (act === 'learn') {
      learning = learning === i ? null : i
      render()
    }
    return
  }
  const k = e.target.dataset.k
  if (!k) return
  const v = e.target.value
  const n = (x) => (x === '' ? '' : Math.max(0, Math.min(127, parseInt(x, 10) || 0)))
  if (k === 'from' || k === 'to') it[k] = clean(v)
  else if (k === 'comment') it.comment = v.replace(/\n/g, ' ')
  else if (k === 'text') it.text = v
  else if (k === 'inCh' || k === 'outCh') it[k] = +v
  else if (k === 'inNum' || k === 'inHi' || k === 'outNum') it[k] = n(v)
  else if (k === 'transpose') it.transpose = Math.max(-127, Math.min(127, parseInt(v, 10) || 0))
  else if (k === 'drop') it.drop = !!v
  else if (k === 'inType' || k === 'outType') it[k] = v
  else if (k === 'velMode') it.vel = v === 'n' ? '100' : v === '%' ? '100%' : ''
  else if (k === 'velNum') {
    const pct = it.vel.endsWith('%')
    const x = Math.max(1, Math.min(pct ? 400 : 127, parseInt(v, 10) || 1))
    it.vel = pct ? `${x}%` : String(x)
  }
  // a select changes which fields there are; typing only changes the words
  if (e.type === 'change' && e.target.tagName === 'SELECT') render()
  else refresh()
}

function onDevices(e) {
  const li = e.target.closest('[data-d]')
  if (!li) return
  const d = devices()[+li.dataset.d]
  if (e.type === 'change' && e.target.classList.contains('short')) {
    const was = d.short
    d.short = clean(e.target.value) || was
    // the rules that named it follow
    for (const r of rules()) {
      if (r.from === was) r.from = d.short
      if (r.to === was) r.to = d.short
    }
    render()
    return
  }
  if (e.type !== 'click') return
  const act = e.target.closest('button')?.dataset.do
  const used = new Set(rules().map((r) => r.outCh))
  const ch = Array.from({ length: 16 }, (_, k) => k + 1).find((c) => !used.has(c)) || 1
  if (act === 'own') state.items.push(newRule({ from: d.short, outCh: ch, comment: `${d.short} plays channel ${ch}` }))
  else if (act === 'rest') state.items.push(newRule({ from: d.short, drop: true, comment: `everything else from ${d.short}: dropped` }))
  else if (act === 'rule') state.items.push(newRule({ from: d.short }))
  else if (act === 'touch') {
    const [label, type, n] = TOUCH[+e.target.closest('button').dataset.k]
    const [lo, hi] = String(n).split('-')
    state.items.push(newRule({ from: 'touch', inType: type, inNum: +lo, inHi: hi ? +hi : '', comment: `touch ${label.toLowerCase()}` }))
  } else if (act === 'forget') {
    state.devices.splice(state.devices.indexOf(d), 1)
  } else return
  render()
}

function download() {
  const a = document.createElement('a')
  a.href = URL.createObjectURL(new Blob([save(state.items)], { type: 'text/plain' }))
  a.download = 'midimap.txt'
  a.click()
  setTimeout(() => URL.revokeObjectURL(a.href), 1000)
}

async function open(file) {
  const { items, problems } = load(await file.text())
  state.items = items
  // the names its rules use, as devices to pick from
  for (const r of rules()) {
    for (const s of [r.from, r.to]) {
      if (s && s !== 'fm-1' && !devices().some((d) => d.short === s)) state.devices.push({ name: s, short: s, manual: true })
    }
  }
  learning = null
  render()
  $('#map-problem').textContent = problems.join(' · ')
}

export function init() {
  if (!$('#map')) return
  state = restore()
  $('#midi-connect').onclick = connect
  $('#rules').addEventListener('click', onRules)
  $('#rules').addEventListener('input', onRules)
  $('#rules').addEventListener('change', onRules)
  $('#devices').addEventListener('click', onDevices)
  $('#devices').addEventListener('change', onDevices)
  $('#rule-add').onclick = () => {
    state.items.push(newRule())
    render()
  }
  $('#note-add').onclick = () => {
    state.items.push({ t: 'note', gap: true, text: '' })
    render()
  }
  $('#learn-new').onclick = () => {
    if (!access) connect()
    learning = learning === 'new' ? null : 'new'
    render()
  }
  $('#dev-add').onsubmit = (e) => {
    e.preventDefault()
    const input = $('input', e.target)
    const short = clean(input.value)
    if (short && !devices().some((d) => d.short === short)) state.devices.push({ name: input.value.trim(), short, manual: true })
    input.value = ''
    render()
  }
  $('#map-save').onclick = download
  $('#map-copy').onclick = async () => {
    try {
      await navigator.clipboard.writeText(save(state.items))
      $('#map-copy').textContent = 'Copied'
      setTimeout(() => ($('#map-copy').textContent = 'Copy'), 1200)
    } catch (_) {}
  }
  $('#map-open').onchange = (e) => e.target.files[0] && open(e.target.files[0]).then(() => (e.target.value = ''))
  $('#map-clear').onclick = () => {
    state = fresh()
    learning = null
    render()
  }
  render()
  // it asks for MIDI only when it's had it before, or when you press the button
  navigator.permissions
    ?.query({ name: 'midi' })
    .then((p) => p.state === 'granted' && connect())
    .catch(() => {})
}
