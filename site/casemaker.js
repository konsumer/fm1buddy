// A battery case base for any battery: the case's OpenSCAD file, run in the browser (OpenSCAD in WebAssembly,
// loaded only when it's needed), with the battery's size. The file is the one the release's STLs were made from,
// copied next to the page by the pages workflow (fw/), or main's

const OPENSCAD = 'https://cdn.jsdelivr.net/npm/openscad-wasm-prebuilt@1.2.0/dist/openscad.js'
const SCAD = ['fw/fm1buddy-case.scad', 'https://raw.githubusercontent.com/konsumer/fm1buddy/main/case/fm1buddy-case.scad']
const ROOM = [2, 2, 0.5] // added to the cell's size, for its tolerance and its wires

// the cells' own sizes (length, width, thickness): common and cheap
const PRESETS = [
  { name: 'No battery', size: null },
  { name: '503035 · 500 mAh', size: [35, 30, 5] },
  { name: '603450 · 1200 mAh', size: [50, 34, 6] },
  { name: '103450 · 2000 mAh', size: [50, 34, 10] },
  { name: 'MakerFocus 3000 mAh', size: [65, 36, 10] },
  { name: '105060 · 3000 mAh', size: [60, 50, 10] }
]

let openscad // the module, once loaded
let scadText
let url // the last base made (a blob: URL)

async function scad() {
  if (!scadText) {
    for (const u of SCAD) {
      const r = await fetch(u).catch(() => null)
      if (r && r.ok) {
        scadText = await r.text()
        break
      }
    }
    if (!scadText) {
      throw new Error("couldn't get the case's OpenSCAD file")
    }
  }
  return scadText
}

// one run of OpenSCAD: a new instance each time (its main isn't made to run twice)
async function render(defines) {
  if (!openscad) {
    openscad = await import(OPENSCAD)
  }
  const log = []
  const o = await openscad.createOpenSCAD({ noInitialRun: true, print: (s) => log.push(s), printErr: (s) => log.push(s) })
  const m = o.getInstance()
  m.FS.writeFile('/case.scad', await scad())
  const args = ['/case.scad', '--backend=manifold', '--export-format=binstl', '-o', '/out.stl']
  for (const [k, v] of Object.entries(defines)) {
    args.push('-D', `${k}=${v}`)
  }
  let code
  try {
    code = m.callMain(args)
  } catch (e) {
    code = e?.status ?? 1
  }
  let stl
  try {
    stl = m.FS.readFile('/out.stl')
  } catch {}
  if (code || !stl?.length) {
    const why = log.find((l) => /ERROR|Assertion/.test(l)) || log.at(-1) || 'OpenSCAD failed'
    throw new Error(why.replace(/^ERROR:\s*/, '').replace(/ in file .*$/, ''))
  }
  return stl
}

export function init() {
  const box = document.getElementById('maker')
  if (!box) {
    return
  }
  const $ = (s) => box.querySelector(s)
  const inputs = ['x', 'y', 'z'].map((n) => $(`[name=${n}]`))
  const sw = $('[name=sw]')
  const go = $('.maker-go')
  const out = $('.maker-out')
  const status = $('.maker-status')
  let none = false

  const show = () => {
    for (const i of inputs) {
      i.disabled = none
    }
    sw.disabled = none
    for (const b of box.querySelectorAll('.maker-presets button')) {
      const p = PRESETS[b.dataset.i]
      b.classList.toggle('on', p.size ? !none && p.size.every((v, k) => +inputs[k].value === v) : none)
    }
    out.innerHTML = ''
    status.textContent = ''
  }
  const pick = (p) => {
    none = !p.size
    if (p.size) {
      p.size.forEach((v, k) => (inputs[k].value = v))
    }
    show()
  }
  $('.maker-presets').innerHTML = PRESETS.map((p, i) => `<button type="button" data-i="${i}">${p.name}</button>`).join('')
  for (const b of box.querySelectorAll('.maker-presets button')) {
    b.onclick = () => pick(PRESETS[b.dataset.i])
  }
  for (const i of [...inputs, sw]) {
    i.oninput = show
  }
  pick(PRESETS[4])

  go.onclick = async () => {
    const size = inputs.map((i) => parseFloat(i.value))
    if (!none && size.some((v) => !(v > 0))) {
      status.textContent = 'Fill in the battery’s length, width and thickness.'
      status.className = 'note maker-status bad'
      return
    }
    const bat = size.map((v, k) => +(v + ROOM[k]).toFixed(2))
    if (!none && (bat[0] > 72 || bat[1] > 53)) {
      status.textContent = `That’s too big: a battery up to ${72 - ROOM[0]} × ${53 - ROOM[1]} mm fits (any thickness).`
      status.className = 'note maker-status bad'
      return
    }
    go.disabled = true
    out.innerHTML = ''
    status.className = 'note maker-status'
    status.textContent = openscad ? 'Making it…' : 'Getting OpenSCAD (about 11 MB, once) and making it…'
    try {
      const defines = none ? { battery: false, part: '"base"' } : { battery: true, bat: `[${bat}]`, switch_hole: sw.checked ? '[9, 4.2]' : '[0, 0]', part: '"base"' }
      const stl = await render(defines)
      if (url) {
        URL.revokeObjectURL(url)
      }
      url = URL.createObjectURL(new Blob([stl], { type: 'model/stl' }))
      const name = none ? 'fm1buddy-case-s3-base.stl' : `fm1buddy-case-s3-battery-${size.join('x')}${sw.checked ? '-switch' : ''}-base.stl`
      out.innerHTML = `<a class="button" href="${url}" download="${name}">Base</a> <a class="button secondary" href="fw/fm1buddy-case-s3-lid.stl" download>Lid</a>`
      status.textContent = none ? 'The case without a battery.' : `Room for ${bat.join(' × ')} mm: the case is ${(bat[2] + 0.7).toFixed(1)} mm taller than without a battery.`
    } catch (e) {
      status.textContent = `It didn’t work: ${e.message}`
      status.className = 'note maker-status bad'
    } finally {
      go.disabled = false
    }
  }
}
