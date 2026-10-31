/* Add Games: modal + queue, a section of the shared transfer dock for the
 * transfer itself, and a whole-window drop overlay. Rows live in module
 * state so closing the modal never loses an in-flight upload — the dock
 * keeps showing its progress. */

import { state, esc, fmtSize, extOf, toast } from './state.js';
import { uploadFile } from './api.js';
import * as dock from './dock.js';

/* Keep in step with ALLOWED_EXT in server/store/http_server.py — the
 * server stays the authority; this only rejects hopeless rows early. */
const ALLOWED_EXT = ['iso', 'cso', 'pbp', 'zip', '7z', 'rar', 'pkg', 'rap',
                     'bin', 'cue'];

let rows = [];          // {name,size,loaded,status,error,file,el}
let deps = null;        // { onItems(items) } — injected by ui.js
let modalBody = null;   // <ul> in the modal, or null while closed
let upSec = null;       // dock section handle, made on first use

function up() {
  if (!upSec) upSec = dock.section('uploads', 'Uploads');
  return upSec;
}

export function init(d) { deps = d; }

export function openAdd(prefetched) {
  const root = document.getElementById('modal-root');
  root.innerHTML =
    '<div class="modal-scrim" id="add-scrim">' +
      '<div class="modal" role="dialog" aria-modal="true" aria-label="Add games">' +
        '<button class="icon-btn close-x" id="add-close" aria-label="Close">✕</button>' +
        '<h3>Add Games</h3>' +
        '<div class="field"><label for="add-platform">Platform</label>' +
          '<select id="add-platform">' +
            '<option value="psp"' + (state.uploadPlatform === 'psp' ? ' selected' : '') +
              '>PSP</option>' +
            '<option value="ps3"' + (state.uploadPlatform === 'ps3' ? ' selected' : '') +
              '>PS3</option>' +
          '</select></div>' +
        '<div class="drop-zone" id="add-drop" tabindex="0">' +
          'Choose files or drop them here' +
          '<div class="drop-hint">' + ALLOWED_EXT.join(' ') +
            ' — files land in the library root, tagged for the platform you' +
            ' pick, and appear in its tab at once</div>' +
          '<div class="drop-hint hint-ps3" id="add-ps3-hint"' +
            (state.uploadPlatform === 'ps3' ? '' : ' hidden') + '>' +
            'Licenses: upload a .rap together with its game, then attach it' +
            ' from the game’s drawer. Only supply .rap files for games you' +
            ' own.</div>' +
        '</div>' +
        '<input type="file" id="add-input" multiple hidden />' +
        '<ul class="up-list" id="add-list"></ul>' +
      '</div>' +
    '</div>';

  const sel = document.getElementById('add-platform');
  sel.onchange = () => {
    state.uploadPlatform = sel.value;
    const h = document.getElementById('add-ps3-hint');
    if (h) h.hidden = sel.value !== 'ps3';
  };
  const zone = document.getElementById('add-drop');
  const input = document.getElementById('add-input');
  zone.onclick = () => input.click();
  zone.onkeydown = (e) => {
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); input.click(); }
  };
  input.onchange = () => { addFiles(input.files); input.value = ''; };
  zone.ondragover = (e) => { e.preventDefault(); zone.classList.add('is-over'); };
  zone.ondragleave = () => zone.classList.remove('is-over');
  zone.ondrop = (e) => {
    e.preventDefault(); e.stopPropagation();
    zone.classList.remove('is-over');
    addFiles(e.dataTransfer.files);
  };
  document.getElementById('add-close').onclick = closeAdd;
  document.getElementById('add-scrim').onclick = (e) => {
    if (e.target.id === 'add-scrim') closeAdd();
  };

  modalBody = document.getElementById('add-list');
  renderRows();            // keep the dock section in sync while both are open
  if (prefetched && prefetched.length) addFiles(prefetched);
}

export function closeAdd() {
  document.getElementById('modal-root').innerHTML = '';
  modalBody = null;
  if (rows.length) { up(); dock.setVisible('uploads', true); }
}

export function isModalOpen() {
  return !!document.getElementById('add-scrim');
}

/** Whole-window drop: any file dragged onto the page opens the panel. */
export function bindWindowDrop() {
  const overlay = document.getElementById('drop-overlay');
  let depth = 0;
  const hasFiles = (e) => e.dataTransfer &&
    Array.prototype.indexOf.call(e.dataTransfer.types || [], 'Files') !== -1;
  window.addEventListener('dragenter', (e) => {
    if (!hasFiles(e)) return;
    depth++;
    overlay.hidden = false;
  });
  window.addEventListener('dragover', (e) => {
    if (hasFiles(e)) e.preventDefault();   // needed to own the drop
  });
  window.addEventListener('dragleave', () => {
    depth = Math.max(0, depth - 1);
    if (!depth) overlay.hidden = true;
  });
  window.addEventListener('drop', (e) => {
    if (!hasFiles(e)) return;
    e.preventDefault();
    depth = 0;
    overlay.hidden = true;
    if (isModalOpen()) return;   // the modal's own handler took it
    openAdd(e.dataTransfer.files);
  });
}

/* ---------------- queue ---------------- */

function addFiles(files) {
  Array.prototype.forEach.call(files, (f) => {
    const row = { name: f.name, size: f.size, loaded: 0,
                  status: 'queued', error: '', file: f };
    if (ALLOWED_EXT.indexOf(extOf(f.name)) === -1) {
      row.status = 'err'; row.error = 'type not allowed';
    } else if (!f.size) {
      row.status = 'err'; row.error = 'empty file';
    }
    rows.push(row);
  });
  renderRows();
  pump();
}

/* One upload at a time: sequential NAS writes, honest progress bars. */
function pump() {
  if (state.uploading) return;
  const row = rows.find((r) => r.status === 'queued');
  if (!row) return;
  up();
  dock.setVisible('uploads', true);
  state.uploading = true;
  row.status = 'uploading';
  renderRows();
  uploadFile(row.file, state.uploadPlatform, (loaded) => {
    row.loaded = loaded;
    renderRows();
  }).then((data) => {
    row.loaded = row.size;
    row.status = 'ok';
    if (deps) deps.onItems(data.items);   // merged catalog, authoritative
    toast('Added ' + row.name, 'ok');
  }).catch((err) => {
    row.status = 'err';
    row.error = err.message;
    toast(row.name + ': ' + err.message, 'err');
  }).then(() => {
    state.uploading = false;
    renderRows();
    pump();
  });
}

/* ---------------- rows / dock ----------------
 * Rows re-render wholesale into every open container (modal and dock show
 * the same queue); a row is just data, so either view can be closed and
 * reopened at any point mid-transfer. The dock's shell, its sections and
 * the collapse handling belong to dock.js — this module only fills its
 * section's <ul> and updates the mini-head's count text. */

function rowPct(row) {
  if (row.status === 'ok') return 100;
  if ((row.status === 'uploading' || row.status === 'err') && row.size > 0) {
    return Math.min(99, Math.round(row.loaded / row.size * 100));
  }
  return 0;
}
function rowStatus(row) {
  switch (row.status) {
    case 'queued': return 'Waiting';
    case 'uploading': return rowPct(row) + '%';
    case 'ok': return '✓ Added';
    case 'err': return row.error || 'Failed';
    default: return '';
  }
}

function rowEl(row) {
  const li = document.createElement('li');
  li.className = 'up-row' +
    (row.status === 'ok' ? ' is-ok' : row.status === 'err' ? ' is-err' : '');
  li.innerHTML =
    '<div><div class="up-name">' + esc(row.name) + '</div>' +
      '<div class="up-size">' + fmtSize(row.size) + '</div></div>' +
    '<div class="up-bar"><div class="up-fill" style="width:' +
      rowPct(row) + '%"></div></div>' +
    '<div class="up-status">' + esc(rowStatus(row)) + '</div>';
  return li;
}

function renderRows() {
  if (modalBody) {
    modalBody.innerHTML = '';
    rows.forEach((r) => modalBody.appendChild(rowEl(r)));
  }
  const list = up().list;
  list.innerHTML = '';
  rows.forEach((r) => list.appendChild(rowEl(r)));
  const left = rows.filter((r) => r.status === 'queued' ||
                                 r.status === 'uploading').length;
  up().setInfo(left ? left + ' to go' : 'done');
}
