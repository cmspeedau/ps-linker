/* Shared state, persistence, and the small helpers every module needs. */

const LS_FAVS = 'pslinker.favorites.v1';      // kept from the old shell
const LS_RECENT = 'pslinker.recent.v1';
const LS_SETTINGS = 'pslinker.settings.v1';
const RECENT_CAP = 50;

export const state = {
  items: [],
  section: 'all',            // all | psp | ps3 | favorites | recent | folder:<name>
  query: '',
  sort: 'title',
  selectedId: null,
  favorites: [],
  recent: {},
  settings: { theme: 'auto', sort: 'title' },
  uploadPlatform: 'psp',
  uploading: false,
};

function loadLS(key, fallback) {
  try {
    const raw = localStorage.getItem(key);
    const v = raw ? JSON.parse(raw) : null;
    return v === null || typeof v !== 'object' ? fallback : v;
  } catch (e) { return fallback; }
}
function saveLS(key, value) {
  try { localStorage.setItem(key, JSON.stringify(value)); } catch (e) { /* quota */ }
}

state.favorites = Array.isArray(loadLS(LS_FAVS, [])) ? loadLS(LS_FAVS, []) : [];
state.recent = loadLS(LS_RECENT, {});
Object.assign(state.settings, loadLS(LS_SETTINGS, {}));
state.sort = state.settings.sort || 'title';

export function saveFavorites() { saveLS(LS_FAVS, state.favorites); }
export function saveRecent() { saveLS(LS_RECENT, state.recent); }
export function saveSettings() { saveLS(LS_SETTINGS, state.settings); }

export function isFav(id) { return state.favorites.indexOf(id) !== -1; }
export function toggleFav(id) {
  const i = state.favorites.indexOf(id);
  if (i === -1) state.favorites.push(id); else state.favorites.splice(i, 1);
  saveFavorites();
}

export function markViewed(item) {
  if (!item || !item.id) return;
  state.recent[item.id] = Date.now();
  const keys = Object.keys(state.recent);
  if (keys.length > RECENT_CAP) {
    keys.sort((a, b) => state.recent[a] - state.recent[b])
      .slice(0, keys.length - RECENT_CAP)
      .forEach((k) => delete state.recent[k]);
  }
  saveRecent();
}

export function itemById(id) {
  return state.items.find((it) => it.id === id) || null;
}

/** Sidebar sections, with counts, in display order. Counts match the grid,
 *  so attached licenses are not double-counted as library entries. */
export function sections() {
  const items = browseItems(state.items);
  const out = [
    { id: 'all', label: 'All Games', group: 'Library',
      count: items.length },
    { id: 'favorites', label: 'Favorites', group: 'Library',
      count: items.filter((it) => isFav(it.id)).length },
    { id: 'recent', label: 'Recently Viewed', group: 'Library',
      count: Object.keys(state.recent).filter((id) =>
        items.some((it) => it.id === id)).length },
  ];
  if (items.some((it) => platformOf(it) === 'psp')) {
    out.push({ id: 'psp', label: 'PSP', group: 'Platforms',
      count: items.filter((it) => platformOf(it) === 'psp').length });
  }
  if (items.some((it) => platformOf(it) === 'ps3')) {
    out.push({ id: 'ps3', label: 'PS3', group: 'Platforms',
      count: items.filter((it) => platformOf(it) === 'ps3').length });
  }
  const folders = {};
  items.forEach((it) => {
    const cat = it.category || 'Uncategorized';
    folders[cat] = (folders[cat] || 0) + 1;
  });
  Object.keys(folders).sort((a, b) => a.localeCompare(b, undefined, { numeric: true }))
    .forEach((cat) => out.push({ id: 'folder:' + cat, label: cat,
      group: 'Folders', count: folders[cat] }));
  return out;
}

/** 'psp' | 'ps3' | '' — the sidecar field only, never a title regex:
 *  matching on the name misfiles things like "PSP Emulator Frontend",
 *  the same lesson the device module learned (ui/INTEGRATION.md). */
export function platformOf(it) {
  return String(it.platform || '').toLowerCase();
}

/* ---- licenses (.rap) ----
 * An attached .rap is an accessory, not a card: the pairing lives in the
 * game's meta (entry.rap = file name beside the game), so the rap's own
 * library entry is hidden from the grid and reached from the game. */

function relOf(url) { return String(url || '').replace(/^\/+/, ''); }

export function gameDirOf(item) {
  const rel = relOf(item.url);
  return rel.indexOf('/') === -1 ? '' : rel.slice(0, rel.lastIndexOf('/'));
}

export function rapUrlOf(item) {
  if (!item.rap) return '';
  const dir = gameDirOf(item);
  return '/' + (dir ? dir + '/' : '') + item.rap;
}

/** Library minus the .rap entries referenced by some game. */
function attachedRapUrls(items) {
  const attached = new Set();
  items.forEach((it) => {
    const u = rapUrlOf(it);
    if (u) attached.add(u);
  });
  return attached;
}

export function browseItems(items) {
  const attached = attachedRapUrls(items);
  return items.filter((it) => !attached.has(relOf(it.url) ? '/' + relOf(it.url) : ''));
}

/** Unattached .rap entries sitting in the game's own folder — attachable
 *  without re-uploading (the server accepts any .rap beside the game). */
export function availableRaps(item, items) {
  const pool = items || state.items;
  const attached = attachedRapUrls(pool);
  const dir = gameDirOf(item);
  return pool.filter((it) =>
    extOf(it.url || it.name || '') === 'rap' &&
    gameDirOf(it) === dir &&
    !attached.has('/' + relOf(it.url)));
}

/** {name, url, missing} for item.rap, or null when none is on record. */
export function licenseInfo(item, items) {
  const url = rapUrlOf(item);
  if (!url) return null;
  const pool = items || state.items;
  const want = url.slice(1);
  const found = pool.some((it) => relOf(it.url) === want);
  return { name: item.rap, url: url, missing: !found };
}

export function currentItems() {
  let list = browseItems(state.items).slice();
  const sec = state.section;
  if (sec === 'favorites') {
    list = list.filter((it) => isFav(it.id));
  } else if (sec === 'recent') {
    list = list.filter((it) => state.recent[it.id]);
    list.sort((a, b) => state.recent[b.id] - state.recent[a.id]);
  } else if (sec === 'psp' || sec === 'ps3') {
    list = list.filter((it) => platformOf(it) === sec);
  } else if (sec.indexOf('folder:') === 0) {
    const cat = sec.slice(7);
    list = list.filter((it) => (it.category || 'Uncategorized') === cat);
  }
  // 'recent' keeps its recency order; everything else obeys the sort.
  if (sec !== 'recent') list.sort(bySortKey());
  const q = state.query.trim().toLowerCase();
  if (q) {
    list = list.filter((it) => {
      const hay = (it.title + ' ' + (it.category || '') + ' ' +
                   (it.platform || '')).toLowerCase();
      return hay.indexOf(q) !== -1;
    });
  }
  return list;
}

function bySortKey() {
  const byTitle = (a, b) =>
    String(a.title || '').localeCompare(String(b.title || ''), undefined,
                                         { numeric: true, sensitivity: 'base' });
  switch (state.sort) {
    case 'title-desc': return (a, b) => byTitle(b, a);
    case 'size-desc': return (a, b) => num(b.size) - num(a.size);
    case 'size-asc': return (a, b) => num(a.size) - num(b.size);
    default: return byTitle;
  }
}
function num(v) { const n = Number(v); return isFinite(n) ? n : 0; }

/* ---------------- helpers every module shares ---------------- */

export function esc(s) {
  return String(s == null ? '' : s)
    .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
    .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
}

export function fmtSize(bytes) {
  let n = Number(bytes);
  if (!isFinite(n)) return '—';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let u = 0;
  while (n >= 1024 && u < units.length - 1) { n /= 1024; u++; }
  return (u === 0 ? String(Math.round(n)) : n.toFixed(n < 10 ? 1 : 0))
         + ' ' + units[u];
}

export function extOf(name) {
  const i = String(name).lastIndexOf('.');
  return i === -1 ? '' : String(name).slice(i + 1).toLowerCase();
}

/** Cover-art url or a deterministic placeholder for a tile element. */
export function coverHTML(item, cls) {
  if (item.icon) {
    return '<div class="' + cls + '" style="background-image:url(\'' +
           esc(item.icon).replace(/'/g, "\\'") + '\')"></div>';
  }
  const label = (item.title || '?').trim().slice(0, 2).toUpperCase();
  return '<div class="' + cls + ' placeholder" style="' + placeholderBG(item) +
         '" data-label="' + esc(label) + '"></div>';
}

/** Two hues derived from the id, so a tile keeps its colour across views. */
export function placeholderBG(item) {
  let h = 0;
  const s = String(item.id || item.title || '');
  for (let i = 0; i < s.length; i++) h = (h * 31 + s.charCodeAt(i)) & 0x3fffffff;
  const hue = h % 360;
  return 'background:linear-gradient(160deg,hsl(' + hue + ' 42% 26%),' +
         'hsl(' + ((hue + 40) % 360) + ' 52% 12%))';
}

export function toast(message, kind) {
  const box = document.getElementById('toasts');
  if (!box) return;
  const el = document.createElement('div');
  el.className = 'toast' + (kind ? ' ' + kind : '');
  el.textContent = message;
  el.addEventListener('click', () => el.remove());
  box.appendChild(el);
  setTimeout(() => el.remove(), 4000);
}
