/* The transfer dock: one corner panel, one titled section per module
 * (uploads, Store). Modules own their section's rows; this owns the shell,
 * the collapse affordances, and hiding the whole dock when no module has
 * anything to show. */

let root = null;
let body = null;
const sects = new Map();   // id -> { el, info, list, caret, visible }

function ensureDock() {
  if (root) return;
  root = document.getElementById('transfer-dock');
  root.innerHTML =
    '<div class="dock-head" id="dock-head">' +
      '<span>Transfers</span>' +
      '<span class="dock-caret" id="dock-caret">▾</span></div>' +
    '<div class="dock-body" id="dock-body"></div>';
  document.getElementById('dock-head').onclick = () => {
    root.classList.toggle('collapsed');
    document.getElementById('dock-caret').textContent =
      root.classList.contains('collapsed') ? '▸' : '▾';
  };
  body = document.getElementById('dock-body');
}

/** Register (or fetch) a dock section; returns { list, setInfo }.
 *  `list` is the <ul> the module fills with .up-row items, `setInfo`
 *  updates the small right-aligned text in the section head. */
export function section(id, label) {
  ensureDock();
  let s = sects.get(id);
  if (s) return api(s);
  const el = document.createElement('div');
  el.className = 'dock-sect';
  el.dataset.sect = id;
  el.hidden = true;
  el.innerHTML =
    '<div class="dock-sect-head">' +
      '<span>' + label + '</span>' +
      '<span class="dock-sect-info"></span>' +
      '<span class="dock-caret">▾</span></div>' +
    '<div class="dock-sect-body"><ul class="up-list"></ul></div>';
  const caret = el.querySelector('.dock-caret');
  el.querySelector('.dock-sect-head').onclick = () => {
    el.classList.toggle('collapsed');
    caret.textContent = el.classList.contains('collapsed') ? '▸' : '▾';
  };
  body.appendChild(el);
  s = {
    el,
    info: el.querySelector('.dock-sect-info'),
    list: el.querySelector('ul'),
    visible: false,
  };
  sects.set(id, s);
  return api(s);
}

function api(s) {
  return {
    list: s.list,
    setInfo(text) { s.info.textContent = text || ''; },
  };
}

/** Show or hide one section; the dock itself follows, so it is only ever
 *  on screen while at least one module has activity to report. */
export function setVisible(id, on) {
  const s = sects.get(id);
  if (!s) return;
  s.visible = !!on;
  s.el.hidden = !on;
  root.hidden = ![...sects.values()].some((v) => v.visible);
}
