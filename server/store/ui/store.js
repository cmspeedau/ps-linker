/* Store: the sidebar entry (with a pending-count badge), the queue modal,
 * and the Store section of the transfer dock.
 *
 * All data comes from the /api/store/* endpoints (see http_server.py): the
 * status file answers "what is downloading right now and what happened
 * lately", the queue file is the editable pending list. The modal and the
 * dock render from the same last-polled snapshot; polling is fast (2 s)
 * while anything is live or the modal is open, slow otherwise. */

import { state, esc, fmtSize, toast } from './state.js';
import { fetchCatalog, storeAdd, storeQueue, storeRemove, storeRun, storeStatus } from './api.js';
import * as dock from './dock.js';

const FAST_MS = 2000;
const SLOW_MS = 60000;
const COOLDOWN_MS = 120000;     // keep the dock section up after a run ends
const DOCK_HISTORY = 3;

let deps = null;                // { onItems } — injected by ui.js
let sect = null;                // dock section handle
let timer = null;
let snap = { phase: 'idle', pending: 0, processing: false, history: [] };
let queue = [];                 // pending list, fetched when the modal is open
let running = null;             // claimed list, same fetch (null = no claim)
let lastTop = null;             // newest history stamp already seen
let catalogSig = null;          // ids+sizes of the catalog we last adopted
let firstPoll = true;
let finishedAt = 0;             // when a live run transitioned to idle

export function init(d) {
  deps = d;
  sect = dock.section('store', 'Store');
  document.getElementById('store-btn').onclick = openStore;
  poll();
}

/* ---------------- polling ---------------- */

function live(s) {
  return !!(s.processing || s.pending || (s.phase && s.phase !== 'idle'));
}

function poll() {
  const p = [storeStatus()];
  if (isModalOpen()) p.push(storeQueue());
  Promise.all(p)
    .then(([st, q]) => {
      const wasLive = live(snap);
      const prevTop = lastTop;
      applyStatus(st);
      if (q) applyQueue(q);
      renderBadge();
      renderDock();
      if (isModalOpen()) renderModal();
      // New library files should show without a reload. The fetcher
      // regenerates catalog.json right after a drain, but the claim (and
      // the synthesized idle) can land a moment before that rewrite, so
      // refetch through the dock cooldown; refreshCatalog only adopts when
      // the catalog actually changed.
      const top = (st.history || [])[0];
      const isNewOk = !firstPoll && top && top.status === 'ok' &&
                      top.finished !== prevTop && prevTop !== null;
      if (top) lastTop = top.finished;
      if (wasLive && !live(snap)) finishedAt = Date.now();
      if (isNewOk || (!live(snap) && Date.now() - finishedAt < COOLDOWN_MS)) {
        refreshCatalog();
      }
    })
    .catch(() => {})                       // share briefly down: next tick
    .then(() => { firstPoll = false; schedule(); });
}

function schedule() {
  clearTimeout(timer);
  timer = setTimeout(poll, isModalOpen() || live(snap) ? FAST_MS : SLOW_MS);
}

/* Adopt the catalog only when it really changed (ids + sizes). The fetcher
 * regenerates catalog.json after a drain, but the idle/claim clearing can
 * be observed a beat before that rewrite, so polls race it harmlessly. */
function refreshCatalog() {
  fetchCatalog()
    .then((items) => {
      const sig = items.map((it) => it.id + ':' + it.size).join(',');
      if (sig === catalogSig) return;
      catalogSig = sig;
      if (deps) deps.onItems(items);
    })
    .catch(() => {});
}

function applyStatus(st) {
  snap = {
    phase: st.phase || 'idle',
    current: st.current || null,
    pending: st.pending || 0,
    processing: !!st.processing,
    history: Array.isArray(st.history) ? st.history : [],
  };
}
function applyQueue(q) {
  queue = Array.isArray(q.queue) ? q.queue : [];
  running = Array.isArray(q.running) ? q.running : null;
}

/* ---------------- sidebar badge ---------------- */

function renderBadge() {
  const b = document.getElementById('store-badge');
  if (!b) return;
  const n = snap.pending;
  b.hidden = !n;
  b.textContent = n;
  document.getElementById('store-btn').title =
    live(snap) ? 'Store — download in progress' : 'Store — download queue';
}

/* ---------------- dock section ---------------- */

function fmtAgo(iso) {
  const t = Date.parse(iso || '');
  if (!isFinite(t)) return '';
  const m = Math.round((Date.now() - t) / 60000);
  if (m < 1) return 'just now';
  if (m < 60) return m + ' min ago';
  const h = Math.round(m / 60);
  return h < 24 ? h + ' h ago' : Math.round(h / 24) + ' d ago';
}

function pctOf(cur) {
  if (snap.phase === 'extracting') return 100;
  if (!cur.total) return 0;
  return Math.min(99, Math.round(cur.bytes / cur.total * 100));
}

function renderDock() {
  const up = live(snap) || Date.now() - finishedAt < COOLDOWN_MS;
  dock.setVisible('store', up);
  if (!up) return;
  sect.setInfo(snap.pending ? snap.pending + ' queued' : '');
  const html = [];
  const cur = snap.current;
  if (cur) {
    html.push(
      '<li class="up-row is-active"><div>' +
        '<div class="up-name"><span class="spin"></span>' + esc(cur.title) +
        '</div><div class="up-size">' + esc(cur.category) + ' · ' +
        fmtSize(cur.bytes) + (cur.total ? ' / ' + fmtSize(cur.total) : '') +
        '</div></div>' +
      '<div class="up-bar"><div class="up-fill" style="width:' +
        pctOf(cur) + '%"></div></div>' +
      '<div class="up-status">' +
        (snap.phase === 'extracting' ? 'Extracting' : 'Downloading') +
        (cur.total ? ' ' + pctOf(cur) + '%' : '') + '</div></li>');
  }
  (snap.history || []).slice(0, DOCK_HISTORY).forEach((h) => {
    html.push(
      '<li class="up-row ' + (h.status === 'ok' ? 'is-ok' : 'is-err') + '"><div>' +
        '<div class="up-name">' + esc(h.title) + '</div>' +
        '<div class="up-size">' + esc(h.category) + ' · ' + fmtSize(h.bytes) +
        ' · ' + fmtAgo(h.finished) + '</div></div><div></div>' +
      '<div class="up-status">' +
        (h.status === 'ok' ? '✓ ' + h.seconds + 's' : esc(h.error || 'failed')) +
        '</div></li>');
  });
  if (!html.length) {
    html.push('<li class="dock-note">Download finished.</li>');
  }
  sect.list.innerHTML = html.join('');
}

/* ---------------- modal ---------------- */

export function isModalOpen() { return !!document.getElementById('store-scrim'); }

export function openStore() {
  if (isModalOpen()) return;
  const overlay = document.createElement('div');
  overlay.className = 'modal-scrim store';
  overlay.id = 'store-scrim';
  overlay.innerHTML =
    '<div class="modal" role="dialog" aria-modal="true" aria-label="Store">' +
      '<button class="icon-btn close-x" id="store-close" aria-label="Close">✕</button>' +
      '<h3>Store</h3>' +
      '<div id="store-now"></div>' +
      '<div class="form-row"><button class="btn btn-primary" id="store-run-btn" ' +
        'type="button">⬇ Download now</button></div>' +
      '<div class="q-add">' +
        '<div class="field"><label for="q-url">Download URL</label>' +
          '<input id="q-url" type="url" placeholder="https://example.com/game.zip" />' +
        '</div>' +
        '<div class="form-row">' +
          '<div class="field"><label for="q-title">Title (optional)</label>' +
            '<input id="q-title" type="text" maxlength="200" /></div>' +
          '<div class="field"><label for="q-cat">Category</label>' +
            '<input id="q-cat" type="text" maxlength="64" value="Homebrew" ' +
              'list="q-cats" /></div>' +
        '</div>' +
        '<datalist id="q-cats"></datalist>' +
        '<div class="form-row"><button class="btn btn-primary" id="q-add-btn" ' +
          'type="button">＋ Add to queue</button></div>' +
      '</div>' +
      '<h4 class="q-head">Queue</h4>' +
      '<ul class="up-list q-list" id="store-queue"></ul>' +
      '<h4 class="q-head">Recent downloads</h4>' +
      '<ul class="up-list q-list" id="store-history"></ul>' +
    '</div>';
  document.body.appendChild(overlay);

  overlay.onclick = (e) => { if (e.target === overlay) closeStore(); };
  document.getElementById('store-close').onclick = closeStore;
  document.getElementById('store-run-btn').onclick = () => {
    storeRun()
      .then(() => toast('Download started', 'ok'))
      .catch((err) => toast(err.message, 'err'))
      .then(poll);
  };
  document.getElementById('q-add-btn').onclick = addToQueue;
  document.getElementById('q-url').onkeydown = (e) => {
    if (e.key === 'Enter') addToQueue();
  };
  const cats = new Set(state.items.map((it) => it.category || 'Uncategorized'));
  cats.add('Homebrew');
  document.getElementById('q-cats').innerHTML = [...cats].sort()
    .map((c) => '<option value="' + esc(c) + '">').join('');
  storeQueue().then(applyQueue).catch(() => {});
  renderModal();
  poll();
}

export function closeStore() {
  const el = document.getElementById('store-scrim');
  if (el) el.remove();
  schedule();
}

function addToQueue() {
  const urlEl = document.getElementById('q-url');
  const url = urlEl.value.trim();
  if (!/^https?:\/\/.+/i.test(url)) { toast('Enter an http(s) URL', 'err'); return; }
  const body = {
    url,
    title: document.getElementById('q-title').value.trim(),
    category: document.getElementById('q-cat').value.trim() || 'Homebrew',
  };
  storeAdd(body)
    .then((data) => {
      toast('Queued ' + (body.title || url), 'ok');
      urlEl.value = '';
      document.getElementById('q-title').value = '';
      applyQueue({ queue: data.queue, running });
      renderModal();
      poll();
    })
    .catch((err) => toast(err.message, 'err'));
}

function removeItem(url) {
  storeRemove(url)
    .then((data) => {
      toast('Removed from queue');
      applyQueue({ queue: data.queue, running });
      renderModal();
      poll();
    })
    .catch((err) => toast(err.message, 'err'));   // e.g. claimed: 409
}

function renderModal() {
  const now = document.getElementById('store-now');
  if (!now) return;
  const cur = snap.current;
  if (cur) {
    now.innerHTML =
      '<div class="up-row is-active q-live"><div>' +
        '<div class="up-name"><span class="spin"></span>' + esc(cur.title) + '</div>' +
        '<div class="up-size">' + esc(cur.category) + ' · ' + fmtSize(cur.bytes) +
        (cur.total ? ' / ' + fmtSize(cur.total) : '') + '</div></div>' +
      '<div class="up-bar"><div class="up-fill" style="width:' + pctOf(cur) +
        '%"></div></div><div class="up-status">' +
        (snap.phase === 'extracting' ? 'Extracting' : 'Downloading') +
        (cur.total ? ' ' + pctOf(cur) + '%' : '') + '</div></div>';
  } else {
    now.innerHTML = '<div class="dock-note">' +
      (snap.pending ? snap.pending + ' item(s) queued, fetcher idle' :
       'Idle — nothing downloading') + '</div>';
  }

  const ql = document.getElementById('store-queue');
  const rows = [];
  (running || []).forEach((it) => {
    rows.push('<li class="up-row is-active"><div>' +
      '<div class="up-name"><span class="spin"></span>' + esc(it.title || it.url) +
      '</div><div class="up-size">' + esc(it.category || '') + ' · ' +
      esc(it.url) + '</div></div><div></div>' +
      '<div class="up-status">Downloading</div></li>');
  });
  queue.forEach((it) => {
    rows.push('<li class="up-row q-row"><div>' +
      '<div class="up-name">' + esc(it.title || it.url) + '</div>' +
      '<div class="up-size">' + esc(it.category || '') + ' · ' + esc(it.url) +
      '</div></div><div></div>' +
      '<div class="q-actions"><span class="up-status">Queued</span>' +
      '<button class="icon-btn" data-del="' + esc(it.url) +
      '" aria-label="Remove from queue" type="button">✕</button></div></li>');
  });
  ql.innerHTML = rows.length ? rows.join('')
    : '<li class="dock-note">Queue is empty.</li>';
  ql.querySelectorAll('[data-del]').forEach((b) => {
    b.onclick = () => removeItem(b.dataset.del);
  });

  const hl = document.getElementById('store-history');
  const hh = snap.history.slice(0, 10);
  hl.innerHTML = hh.length ? hh.map((h) =>
    '<li class="up-row ' + (h.status === 'ok' ? 'is-ok' : 'is-err') + '"><div>' +
      '<div class="up-name">' + esc(h.title) + '</div><div class="up-size">' +
      esc(h.category) + ' · ' + fmtSize(h.bytes) + ' · ' + fmtAgo(h.finished) +
      '</div></div><div></div><div class="up-status">' +
      (h.status === 'ok' ? '✓ ' + h.seconds + 's' : esc(h.error || 'failed')) +
      '</div></li>').join('')
    : '<li class="dock-note">Nothing downloaded yet.</li>';
}
