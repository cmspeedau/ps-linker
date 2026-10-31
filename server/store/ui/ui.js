/* PS Linker — web library shell (direction B: modern dashboard).
 *
 * Entry module: boot, sidebar/toolbar/grid rendering, keyboard, settings.
 * State lives in state.js; the API in api.js; the Add-Games queue in
 * uploads.js; the Store queue and activity in store.js (both report into
 * the shared dock in dock.js); the detail drawer and manage actions in
 * manage.js. No dependencies, no build step — the Docker image is a file
 * copy. */

import {
  state, isFav, toggleFav, esc, fmtSize, toast, coverHTML, platformOf,
  sections, currentItems, saveSettings, saveFavorites, saveRecent,
  extOf, licenseInfo,
} from './state.js';
import { fetchCatalog } from './api.js';
import * as uploads from './uploads.js';
import * as store from './store.js';
import * as manage from './manage.js';

const SORTS = [
  ['title', 'Title A–Z'],
  ['title-desc', 'Title Z–A'],
  ['size-desc', 'Largest first'],
  ['size-asc', 'Smallest first'],
];

/* ---------------------------------------------------------------- boot -- */

document.addEventListener('DOMContentLoaded', () => {
  uploads.init({ onItems });
  store.init({ onItems });
  manage.init({ onItems, refreshGrid: renderLibrary });
  bindChrome();
  bindKeys();
  applyTheme();
  renderSort();
  renderSidebar();          // counts appear once items land; shell shows now
  renderLibrary(true);
  fetchCatalog()
    .then((items) => { state.items = items; refresh(); })
    .catch((err) => showError('Failed to load catalog: ' + err.message));
});

/** Mutations (uploads, rename, meta, delete) all hand back the merged
 * catalog; adopt it and repaint everything that reads it. */
function onItems(items) {
  state.items = items;
  refresh();
}
function refresh() {
  renderSidebar();
  renderLibrary();
  manage.refresh();
}

/* ------------------------------------------------------------- chrome -- */

function bindChrome() {
  document.getElementById('add-btn').onclick = () => uploads.openAdd();
  document.getElementById('settings-btn').onclick = openSettings;
  document.getElementById('search').addEventListener('input', (e) => {
    state.query = e.target.value;
    renderLibrary();
  });
  document.getElementById('sort').addEventListener('change', (e) => {
    state.sort = e.target.value;
    state.settings.sort = e.target.value;
    saveSettings();
    renderLibrary();
  });
  uploads.bindWindowDrop();

  // Mobile sidebar.
  const menu = document.getElementById('menu-btn');
  const closeB = document.getElementById('sidebar-close');
  const scrim = document.getElementById('sidebar-scrim');
  const sidebar = document.getElementById('sidebar');
  const setOpen = (on) => {
    sidebar.classList.toggle('open', on);
    scrim.hidden = !on;
    closeB.hidden = !on;
  };
  menu.onclick = () => setOpen(true);
  closeB.onclick = () => setOpen(false);
  scrim.onclick = () => setOpen(false);
  const mq = matchMedia('(max-width: 860px)');
  const syncMq = () => { menu.hidden = !mq.matches; if (!mq.matches) setOpen(false); };
  mq.addEventListener('change', syncMq);
  syncMq();

  matchMedia('(prefers-color-scheme: light)').addEventListener('change', applyTheme);
}

function bindKeys() {
  document.addEventListener('keydown', (e) => {
    const inField = /^(INPUT|SELECT|TEXTAREA)$/.test(e.target.tagName);
    if (e.key === '/' && !inField) {
      e.preventDefault();
      document.getElementById('search').focus();
      return;
    }
    if (e.key === 'Escape') {
      if (store.isModalOpen()) { store.closeStore(); return; }
      if (uploads.isModalOpen()) { uploads.closeAdd(); return; }
      if (document.querySelector('.modal-scrim.settings')) { closeSettings(); return; }
      if (manage.isOpen()) {
        manage.close();
        renderLibrary();
        return;
      }
      const search = document.getElementById('search');
      if (inField && e.target === search) {
        search.value = ''; state.query = ''; renderLibrary(); search.blur();
      }
      return;
    }
    if (inField) return;
    const card = e.target.closest ? e.target.closest('.card') : null;
    if (!card) return;
    if (e.key === 'ArrowRight' || e.key === 'ArrowLeft' ||
        e.key === 'ArrowUp' || e.key === 'ArrowDown') {
      e.preventDefault();
      moveFocus(card, e.key);
    } else if ((e.key === 'f' || e.key === 'F') && card.dataset.id) {
      toggleFav(card.dataset.id);
      refresh();
    }
  });
}

function moveFocus(card, key) {
  const grid = card.parentElement;
  const cards = Array.prototype.slice.call(grid.querySelectorAll('.card'));
  const i = cards.indexOf(card);
  let j = i;
  if (key === 'ArrowRight') j = Math.min(i + 1, cards.length - 1);
  else if (key === 'ArrowLeft') j = Math.max(i - 1, 0);
  else {
    const cols = getComputedStyle(grid).gridTemplateColumns.split(' ').length || 1;
    j = key === 'ArrowDown' ? Math.min(i + cols, cards.length - 1)
                            : Math.max(i - cols, 0);
  }
  if (cards[j]) cards[j].focus();
}

/* ------------------------------------------------------------ sidebar -- */

function renderSidebar() {
  const nav = document.getElementById('section-nav');
  nav.innerHTML = '';
  let group = '';
  sections().forEach((sec) => {
    if (sec.group !== group) {
      group = sec.group;
      const h = document.createElement('div');
      h.className = 'nav-group';
      h.textContent = group;
      nav.appendChild(h);
    }
    const b = document.createElement('button');
    b.type = 'button';
    b.className = 'nav-item' + (state.section === sec.id ? ' active' : '');
    b.innerHTML = '<span>' + esc(sec.label) + '</span>' +
      '<span class="nav-count">' + sec.count + '</span>';
    b.onclick = () => {
      state.section = sec.id;
      renderSidebar();
      renderLibrary();
      const search = document.getElementById('search');
      search.value = ''; state.query = '';
    };
    nav.appendChild(b);
  });
}

/* ----------------------------------------------------------- toolbar -- */

function renderSort() {
  const sel = document.getElementById('sort');
  sel.innerHTML = SORTS.map(([v, label]) =>
    '<option value="' + v + '"' + (v === state.sort ? ' selected' : '') + '>' +
    label + '</option>').join('');
}

/* -------------------------------------------------------------- grid -- */

function renderLibrary(loading) {
  const main = document.getElementById('library');
  if (loading) {
    main.innerHTML = '<div class="state-box">Loading library…</div>';
    return;
  }
  const list = currentItems();
  if (!list.length) {
    const searching = state.query.trim() !== '';
    main.innerHTML =
      '<div class="state-box"><div class="state-big">' +
        (searching ? 'No matches' :
         state.items.length ? 'Nothing in this view' : 'Your library is empty') +
        '</div>' + (searching ? '' :
          'Use ＋ Add Games to upload an ISO, PBP, or zip — or drop files anywhere on this page.') +
      '</div>';
    return;
  }
  const grid = document.createElement('div');
  grid.className = 'grid';
  grid.setAttribute('role', 'list');
  list.forEach((it) => grid.appendChild(cardEl(it)));
  main.innerHTML = '';
  main.appendChild(grid);
}

function cardEl(item) {
  // A div, not a <button>: the favorite star is a real button inside the
  // card, and a button inside a button is illegal HTML the parser reflows.
  const card = document.createElement('div');
  // A .rap with no game referencing it is still worth a card — it is a
  // license waiting to be attached. Attached ones have none (state.js
  // browseItems), so only unattached raps reach here as `isLicense`.
  const isLicense = extOf(item.url || item.name || '') === 'rap';
  card.className = 'card' + (isLicense ? ' is-license' : '') +
    (item.id === state.selectedId ? ' is-selected' : '');
  card.dataset.id = item.id;
  card.tabIndex = 0;
  card.setAttribute('role', 'listitem');
  const plat = platformOf(item);
  const cat = item.category || 'Uncategorized';
  // "PSP · PSP · 1.1 GB" when the folder already says the platform.
  const platSeg = plat && cat.toUpperCase() !== plat.toUpperCase()
    ? ' · ' + plat.toUpperCase() : '';
  // Key badge: whose license is on record, red-tinted if the file is gone.
  let keyBadge = '';
  if (!isLicense && item.rap) {
    const lic = licenseInfo(item);
    keyBadge = '<span class="badge-key' + (lic && lic.missing ? ' is-broken' : '') +
      '" title="' + esc(lic && lic.missing
        ? 'License file missing: ' + lic.name
        : 'License: ' + (item.rap)) + '">🔑</span>';
  }
  card.innerHTML =
    coverHTML(item, 'card-cover') +
    keyBadge +
    '<button class="fav-star' + (isFav(item.id) ? ' on' : '') + '" type="button" ' +
      'aria-label="Toggle favorite">' + (isFav(item.id) ? '★' : '☆') + '</button>' +
    '<div class="card-body">' +
      '<div class="card-title">' + esc(item.title || 'Untitled') + '</div>' +
      '<div class="card-meta">' +
        (isLicense ? 'License' : esc(cat) + platSeg) +
        ' · ' + fmtSize(item.size) +
      '</div>' +
    '</div>';
  card.querySelector('.fav-star').onclick = (e) => {
    e.stopPropagation();
    toggleFav(item.id);
    refresh();
  };
  card.onclick = () => { manage.open(item.id); refresh(); };
  card.onkeydown = (e) => {
    if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      manage.open(item.id);
      refresh();
    }
  };
  return card;
}

function showError(msg) {
  document.getElementById('library').innerHTML =
    '<div class="state-box err">' + esc(msg) + '</div>';
}

/* ---------------------------------------------------------- settings -- */

function applyTheme() {
  const t = state.settings.theme;
  const light = t === 'light' ||
    (t !== 'dark' && matchMedia('(prefers-color-scheme: light)').matches);
  document.documentElement.dataset.theme = light ? 'light' : 'dark';
}

function openSettings() {
  const opts = SORTS.map(([v, l]) =>
    '<option value="' + v + '"' + (v === state.settings.sort ? ' selected' : '') +
    '>' + l + '</option>').join('');
  const themeOpt = (v, l) =>
    '<option value="' + v + '"' + ((state.settings.theme || 'auto') === v ? ' selected' : '') +
    '>' + l + '</option>';
  const overlay = document.createElement('div');
  overlay.className = 'modal-scrim settings';
  overlay.innerHTML =
    '<div class="modal" role="dialog" aria-modal="true" aria-label="Settings">' +
      '<button class="icon-btn close-x" id="set-close" aria-label="Close">✕</button>' +
      '<h3>Settings</h3>' +
      '<div class="field"><label for="set-theme">Theme</label>' +
        '<select id="set-theme">' +
          themeOpt('auto', 'Follow system') + themeOpt('dark', 'Dark') +
          themeOpt('light', 'Light') +
        '</select></div>' +
      '<div class="field"><label for="set-sort">Default sort</label>' +
        '<select id="set-sort">' + opts + '</select></div>' +
      '<div class="settings-kv">' +
        '<div>Catalog: <code>GET /api/catalog</code> — ' + state.items.length + ' entries</div>' +
        '<div>Local data: ' + state.favorites.length + ' favorites, ' +
          Object.keys(state.recent).length + ' recently viewed' +
        ' (kept in this browser only)</div>' +
        '<div><button class="btn btn-ghost" id="set-reset" type="button">Reset local data</button></div>' +
      '</div>' +
    '</div>';
  document.body.appendChild(overlay);

  overlay.querySelector('#set-close').onclick = closeSettings;
  overlay.onclick = (e) => { if (e.target === overlay) closeSettings(); };
  overlay.querySelector('#set-theme').onchange = (e) => {
    state.settings.theme = e.target.value;
    saveSettings();
    applyTheme();
  };
  overlay.querySelector('#set-sort').onchange = (e) => {
    state.settings.sort = e.target.value;
    state.sort = e.target.value;
    saveSettings();
    renderSort();
    renderLibrary();
  };
  overlay.querySelector('#set-reset').onclick = () => {
    state.favorites = [];
    state.recent = {};
    saveFavorites();
    saveRecent();
    toast('Local favorites and history cleared');
    refresh();
    closeSettings();
  };
}

function closeSettings() {
  const el = document.querySelector('.modal-scrim.settings');
  if (el) el.remove();
}
