/* The detail drawer: what an item is, and what you can do to it.
 * Manage actions call the share's /api/item endpoints and adopt the merged
 * items array each response carries, so the grid and sidebar are current
 * before the next frame. */

import {
  state, itemById, isFav, toggleFav, markViewed, esc, fmtSize, toast,
  coverHTML, platformOf, gameDirOf, licenseInfo, availableRaps, extOf,
} from './state.js';
import {
  deleteItem, renameItem, moveItem, setItemMeta, uploadFile,
  uploadCover, extractCover, clearCover,
} from './api.js';

let deps = null;        // { onItems(items), refreshGrid() }
let form = '';          // '' | 'rename' | 'move' | 'meta'
let deleteArmed = false;

export function init(d) { deps = d; }

export function isOpen() { return !document.getElementById('drawer').hidden; }

export function open(id) {
  const item = itemById(id);
  if (!item) return;
  state.selectedId = id;
  form = '';
  deleteArmed = false;
  markViewed(item);
  render();
}

export function close() {
  state.selectedId = null;
  form = '';
  deleteArmed = false;
  const drawer = document.getElementById('drawer');
  drawer.hidden = true;
  drawer.innerHTML = '';
  if (deps) deps.refreshGrid();
}

/** Re-render after a grid change (selection ring, favorites) or mutation. */
export function refresh() {
  if (isOpen()) render();
}

function fileName(item) {
  if (item.name) return item.name;
  const u = String(item.url || '');
  return u.slice(u.lastIndexOf('/') + 1);
}

/** Existing folder names, for the Move form's datalist suggestions. */
function categoryNames() {
  const seen = {};
  state.items.forEach((it) => {
    const cat = it.category;
    if (cat && cat !== 'Uncategorized') seen[cat] = true;
  });
  return Object.keys(seen).sort();
}

function render() {
  const drawer = document.getElementById('drawer');
  const item = itemById(state.selectedId);
  if (!item) { close(); return; }
  const fav = isFav(item.id);
  drawer.hidden = false;
  drawer.innerHTML =
    '<div class="drawer-inner">' +
      coverHTML(item, 'drawer-cover') +
      '<div class="drawer-title">' + esc(item.title || 'Untitled') +
        (item.platform ? '<span class="chip">' + esc(item.platform) + '</span>' : '') +
      '</div>' +
      '<div class="drawer-sub">' + esc(item.category || 'Uncategorized') +
        ' · ' + fmtSize(item.size) + '</div>' +
      (item.description ? '<div>' + esc(item.description) + '</div>' : '') +
      '<dl class="kv">' +
        '<dt>Version</dt><dd>' + esc(item.version || '—') + '</dd>' +
        '<dt>File</dt><dd>' + esc(fileName(item)) + '</dd>' +
        '<dt>ID</dt><dd>' + esc(item.id || '—') + '</dd>' +
      '</dl>' +
      (form ? '' : coverSection(item) + licenseSection(item)) +
      (form ? renderForm(item) : renderActions(item, fav)) +
    '</div>';

  if (!form) {
    bindLicense(item);
    bindCover(item);
    drawer.querySelector('.a-fav').onclick = () => {
      toggleFav(item.id);
      render();
      deps.refreshGrid();
    };
    drawer.querySelector('.a-rename').onclick = () => { form = 'rename'; render(); };
    drawer.querySelector('.a-move').onclick = () => { form = 'move'; render(); };
    drawer.querySelector('.a-meta').onclick = () => { form = 'meta'; render(); };
    const del = drawer.querySelector('.a-del');
    del.onclick = () => {
      if (!deleteArmed) {
        deleteArmed = true;
        del.textContent = 'Really delete? Click again';
        setTimeout(() => {
          deleteArmed = false;
          if (isOpen() && !form) render();
        }, 4000);
        return;
      }
      del.disabled = true;
      deleteItem(item.url).then((data) => {
        deps.onItems(data.items);
        toast('Deleted ' + fileName(item), 'ok');
        close();
      }).catch((err) => {
        deleteArmed = false;
        del.disabled = false;
        toast('Delete failed: ' + err.message, 'err');
      });
    };
  } else {
    bindForm(item);
  }
}

function renderActions(item, fav) {
  return '<div class="drawer-actions">' +
    '<button class="btn a-fav" type="button">' +
      (fav ? '★ Favorited' : '☆ Favorite') + '</button>' +
    '<a class="btn" href="' + esc(item.url) + '" download>⭳ Download</a>' +
    '<button class="btn btn-ghost a-rename" type="button">✎ Rename file</button>' +
    '<button class="btn btn-ghost a-move" type="button">⇄ Move to folder</button>' +
    '<button class="btn btn-ghost a-meta" type="button" style="grid-column:1/-1">' +
      '✎ Metadata</button>' +
    '<button class="btn btn-danger a-del" type="button" style="grid-column:1/-1">' +
      '🗑 Delete from library</button>' +
  '</div>';
}

/* ---- License (RAP) section --------------------------------------------
 * PS3 titles need a .rap license for released content the user owns. The
 * pairing is explicit (drawer → attach): the file uploads beside the game
 * (`dir=`) and its name lands in the game's sidecar as `rap`, which
 * generate_catalog.py passes through like title/platform. A license whose
 * file went missing is reported, not hidden — a silent badge would lie. */

function licenseSection(item) {
  if (extOf(fileName(item)) === 'rap') {
    // A .rap's own drawer (reachable from history/favorites only — attached
    // raps have no card): say what uses it, offer the file.
    const dir = gameDirOf(item);
    const base = fileName(item);
    const owner = state.items.find((it) =>
      it.rap === base && gameDirOf(it) === dir && it.id !== item.id);
    return '<div class="drawer-form lic-row"><h4>License file (.rap)</h4>' +
      '<div class="lic-state">' + (owner
        ? '🔑 Attached to ' + esc(owner.title || owner.name || owner.id)
        : 'Not attached to any game') + '</div>' +
      '<div class="form-row"><a class="btn" href="' + esc(item.url) +
        '" download>⭳ Download license</a></div></div>';
  }
  if (platformOf(item) !== 'ps3' && !item.rap) return '';
  const lic = licenseInfo(item);
  let stateLine;
  if (!lic) stateLine = '<div class="lic-state">No license attached</div>';
  else if (lic.missing)
    stateLine = '<div class="lic-state missing">⚠ License file missing: ' +
      esc(lic.name) + '</div>';
  else
    stateLine = '<div class="lic-state">🔑 ' + esc(lic.name) +
      ' <a href="' + esc(lic.url) + '" download>download</a></div>';
  // A .rap uploaded beside this game (the usual Add Games flow: game and
  // license together, both to the root) can attach without a re-upload.
  const spare = availableRaps(item);
  const spareRow = spare.length
    ? '<div class="lic-spare">' + spare.map((r) =>
        esc(fileName(r)) + ' <button class="btn btn-ghost a-lic-use" type="button"' +
        ' data-name="' + esc(fileName(r)) + '">Use</button>').join(' ') + '</div>'
    : '';
  return '<div class="drawer-form lic-row"><h4>License (RAP)</h4>' +
    stateLine + spareRow +
    '<div class="form-row">' +
      '<button class="btn btn-ghost a-lic" type="button">' +
        (lic && !lic.missing ? 'Change license' : 'Attach license') + '</button>' +
      (lic ? '<button class="btn btn-ghost a-lic-rm" type="button">Remove</button>' : '') +
    '</div>' +
    '<input type="file" id="lic-file" accept=".rap" hidden />' +
  '</div>';
}

/** The classic footgun is a .rap for a different title ID. Warn, and let
 *  the user proceed — stem conventions vary (region digits, suffixes). */
function confirmMismatch(item, rapName) {
  const stem = rapName.replace(/\.rap$/i, '').toUpperCase();
  return !stem || fileName(item).toUpperCase().indexOf(stem) !== -1 ||
    window.confirm(rapName + ' does not look like a license for ' +
                   fileName(item) + '.\nAttach it anyway?');
}

function bindLicense(item) {
  const drawer = document.getElementById('drawer');
  const btn = drawer.querySelector('.a-lic');
  if (!btn) return;
  const input = drawer.querySelector('#lic-file');
  btn.onclick = () => input.click();
  input.onchange = () => {
    const file = input.files && input.files[0];
    input.value = '';
    if (!file) return;
    if (extOf(file.name) !== 'rap') {
      toast('A license file must end in .rap', 'err');
      return;
    }
    if (!confirmMismatch(item, file.name)) return;
    btn.disabled = true;
    // Beside the game first (the endpoint requires the file to exist
    // there), then record the pairing on the game.
    uploadFile(file, 'ps3', null, gameDirOf(item))
      .then((data) => {
        deps.onItems(data.items);
        return setItemMeta(item.url, { rap: file.name });
      })
      .then((data) => {
        deps.onItems(data.items);
        toast('License attached: ' + file.name, 'ok');
      })
      .catch((err) => toast('License failed: ' + err.message, 'err'))
      .then(() => { btn.disabled = false; render(); deps.refreshGrid(); });
  };
  Array.prototype.forEach.call(
    drawer.querySelectorAll('.a-lic-use'), (use) => {
      use.onclick = () => {
        const name = use.dataset.name;
        if (!confirmMismatch(item, name)) return;
        use.disabled = true;
        setItemMeta(item.url, { rap: name })
          .then((data) => {
            deps.onItems(data.items);
            toast('License attached: ' + name, 'ok');
          })
          .catch((err) => toast('Attach failed: ' + err.message, 'err'))
          .then(() => { render(); deps.refreshGrid(); });
      };
    });
  const rm = drawer.querySelector('.a-lic-rm');
  if (rm) rm.onclick = () => {
    rm.disabled = true;
    setItemMeta(item.url, { rap: null })
      .then((data) => {
        deps.onItems(data.items);
        toast('License detached — the .rap stays in the library', 'ok');
      })
      .catch((err) => toast('Remove failed: ' + err.message, 'err'))
      .then(() => { rm.disabled = false; render(); deps.refreshGrid(); });
  };
}

/* ---- Cover section ------------------------------------------------------
 * Covers are the hidden '<stem>.icon0.<ext>' sidecar: extracted ICON0 or
 * uploaded art, one per game, either way already rendered by coverHTML.
 * This section is control, not preview — the drawer's own cover up top is
 * the preview, so mutations just re-render and refreshGrid(). */

function coverSection(item) {
  if (extOf(fileName(item)) === 'rap') return '';   // licenses get no covers
  const ext = extOf(fileName(item));
  const extractable = ext === 'iso' || ext === 'cso' || ext === 'pbp';
  const stateLine = item.icon
    ? '🖼 Cover art <a href="' + esc(item.icon) +
      '" target="_blank" rel="noopener">view file</a>'
    : '<span class="lic-state">Placeholder tile' +
      (extractable ? ' — no ICON0 found inside the game' : '') +
      '</span>';
  return '<div class="drawer-form lic-row"><h4>Cover</h4>' +
    '<div class="lic-state">' + stateLine + '</div>' +
    '<div class="form-row">' +
      '<button class="btn btn-ghost a-cov-up" type="button">⭱ Upload art</button>' +
      (extractable
        ? '<button class="btn btn-ghost a-cov-ex" type="button">↻ Re-extract</button>'
        : '') +
      (item.icon
        ? '<button class="btn btn-ghost a-cov-rm" type="button">Remove</button>'
        : '') +
    '</div>' +
    '<input type="file" id="cov-file" accept="image/png,image/jpeg" hidden />' +
  '</div>';
}

function bindCover(item) {
  const drawer = document.getElementById('drawer');
  const up = drawer.querySelector('.a-cov-up');
  if (!up) return;
  const settle = (btn, fn) => { btn.disabled = true; fn().then(() => {
    btn.disabled = false; render(); deps.refreshGrid();
  }); };
  const input = drawer.querySelector('#cov-file');
  up.onclick = () => input.click();
  input.onchange = () => {
    const file = input.files && input.files[0];
    input.value = '';
    if (!file) return;
    if (file.size > 8 * 1024 * 1024) {
      toast('Covers are capped at 8 MiB', 'err');
      return;
    }
    settle(up, () => uploadCover(item.url, file)
      .then((data) => { deps.onItems(data.items); toast('Cover updated', 'ok'); })
      .catch((err) => toast('Cover upload failed: ' + err.message, 'err')));
  };
  const ex = drawer.querySelector('.a-cov-ex');
  if (ex) ex.onclick = () =>
    settle(ex, () => extractCover(item.url)
      .then((data) => {
        deps.onItems(data.items);
        toast('Cover re-extracted from the game', 'ok');
      })
      .catch((err) => toast('Extraction failed: ' + err.message, 'err')));
  const rm = drawer.querySelector('.a-cov-rm');
  if (rm) rm.onclick = () =>
    settle(rm, () => clearCover(item.url)
      .then((data) => {
        deps.onItems(data.items);
        toast('Cover removed — tile back to placeholder', 'ok');
      })
      .catch((err) => toast('Cover remove failed: ' + err.message, 'err')));
}

function renderForm(item) {
  if (form === 'rename') {
    return '<div class="drawer-form"><h4>Rename file</h4>' +
      '<div class="field"><input id="f-name" value="' + esc(fileName(item)) +
        '" aria-label="New file name" /></div>' +
      '<div class="form-row"><button class="btn btn-primary f-save" type="button">' +
        'Rename</button>' +
      '<button class="btn btn-ghost f-cancel" type="button">Cancel</button></div></div>';
  }
  if (form === 'move') {
    const cats = categoryNames();
    const cur = item.category && item.category !== 'Uncategorized'
      ? item.category : '';
    return '<div class="drawer-form"><h4>Move to folder</h4>' +
      '<div class="field"><label for="f-cat">Category folder</label>' +
        '<input id="f-cat" list="f-cat-list" value="' + esc(cur) +
        '" placeholder="Existing or new folder" /></div>' +
      '<datalist id="f-cat-list">' + cats.map((c) =>
        '<option value="' + esc(c) + '"></option>').join('') + '</datalist>' +
      '<div class="lic-state">Sidecars travel with the game: metadata, ' +
        'checksum, cover — and its license, when nothing else still needs ' +
        'it.</div>' +
      '<div class="form-row"><button class="btn btn-primary f-save" type="button">' +
        'Move</button>' +
      '<button class="btn btn-ghost f-cancel" type="button">Cancel</button></div></div>';
  }
  const opt = (v, cur, label) =>
    '<option value="' + v + '"' + (v === cur ? ' selected' : '') + '>' + label + '</option>';
  const plat = String(item.platform || '').toLowerCase();
  return '<div class="drawer-form"><h4>Edit metadata</h4>' +
    '<div class="field"><label for="f-title">Display title</label>' +
      '<input id="f-title" value="' + esc(item.title || '') + '" /></div>' +
    '<div class="field"><label for="f-version">Version</label>' +
      '<input id="f-version" value="' + esc(item.version || '') + '" /></div>' +
    '<div class="field"><label for="f-desc">Description</label>' +
      '<textarea id="f-desc">' + esc(item.description || '') + '</textarea></div>' +
    '<div class="field"><label for="f-platform">Platform tag</label>' +
      '<select id="f-platform">' +
        opt('', plat, '— none —') + opt('psp', plat, 'PSP') + opt('ps3', plat, 'PS3') +
      '</select></div>' +
    '<div class="form-row"><button class="btn btn-primary f-save" type="button">' +
      'Save</button>' +
    '<button class="btn btn-ghost f-cancel" type="button">Cancel</button></div></div>';
}

function bindForm(item) {
  const drawer = document.getElementById('drawer');
  drawer.querySelector('.f-cancel').onclick = () => { form = ''; render(); };
  const save = drawer.querySelector('.f-save');
  const fail = (err) => { save.disabled = false; toast(err.message, 'err'); };

  if (form === 'rename') {
    save.onclick = () => {
      // Read the input at click time — binding earlier captured the old value.
      const name = drawer.querySelector('#f-name').value.trim();
      if (!name || name === fileName(item)) { form = ''; render(); return; }
      save.disabled = true;
      renameItem(item.url, name).then((data) => {
        deps.onItems(data.items);
        state.selectedId = data.renamed.id;   // the id changed with the file
        markViewed(itemById(state.selectedId));
        form = '';
        toast('Renamed to ' + name, 'ok');
        render();
      }).catch(fail);
    };
  } else if (form === 'move') {
    save.onclick = () => {
      const cat = drawer.querySelector('#f-cat').value.trim();
      if (!cat) { toast('Pick or type a folder name', 'err'); return; }
      save.disabled = true;
      moveItem(item.url, cat).then((data) => {
        deps.onItems(data.items);
        state.selectedId = data.moved.id;     // ids carry the path
        markViewed(itemById(state.selectedId));
        form = '';
        toast('Moved to ' + cat, 'ok');
        render();
        deps.refreshGrid();
      }).catch(fail);
    };
  } else {
    save.onclick = () => {
      save.disabled = true;
      const patch = {
        title: drawer.querySelector('#f-title').value.trim(),
        version: drawer.querySelector('#f-version').value.trim(),
        description: drawer.querySelector('#f-desc').value.trim(),
        platform: drawer.querySelector('#f-platform').value,
      };
      setItemMeta(item.url, patch).then((data) => {
        deps.onItems(data.items);
        form = '';
        toast('Metadata saved', 'ok');
        render();
      }).catch(fail);
    };
  }
}
