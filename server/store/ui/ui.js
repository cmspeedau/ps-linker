/* PS Linker — Cover Flow UI
 * Pure-browser Cover Flow + grid view with category menu bar.
 * No dependencies. Plain JS.
 */
(function () {
  'use strict';

  var CATALOG_URL = '/api/catalog';   /* absolute: the shell is served under /gui/ */
  var LS_FAVS = 'pslinker.favorites.v1';

  var state = {
    items: [],
    categories: [],
    active: 'All Games',          // current menu filter
    selected: 0,                  // selected index within filtered list
    view: 'cover',                  // 'cover' | 'grid'
    favorites: loadFavs(),
    lastViewedAt: {}
  };

  // Map of menu items to predicates/heuristics.
  // Keeps menu simple and decoupled from any backend taxonomy.
  function matchesFilter(item, filter) {
    switch (filter) {
      case 'All Games':
        return item.category !== 'Utilities';
      case 'PSP Games':
        return /PSP/i.test(item.title || '') ||
               (item.platform || '').toUpperCase() === 'PSP';
      case 'PS3 Games':
        return /PS3/i.test(item.title || '') ||
               (item.platform || '').toUpperCase() === 'PS3';
      case 'Emulators':
        return item.category === 'Emulators';
      case 'Demos':
        return item.category === 'Demos';
      case 'Utilities':
        return item.category === 'Utilities';
      case 'Recently Added':
        return !!state.lastViewedAt[item.id];
      case 'Favorites':
        return state.favorites.indexOf(item.id) !== -1;
      case 'Downloads':
        // No download state yet — placeholder menu entry.
        return false;
      case 'Settings':
        return false;
      default:
        return true;
    }
  }

  function loadFavs() {
    try {
      var raw = localStorage.getItem(LS_FAVS);
      return raw ? JSON.parse(raw) : [];
    } catch (e) { return []; }
  }
  function saveFavs() {
    try { localStorage.setItem(LS_FAVS, JSON.stringify(state.favorites)); }
    catch (e) { /* ignore quota errors */ }
  }
  function toggleFav(id) {
    var i = state.favorites.indexOf(id);
    if (i === -1) state.favorites.push(id);
    else state.favorites.splice(i, 1);
    saveFavs();
  }

  // ---- Boot ----
  document.addEventListener('DOMContentLoaded', function () {
    fetch(CATALOG_URL, { cache: 'no-cache' })
      .then(function (r) {
        if (!r.ok) throw new Error('HTTP ' + r.status);
        return r.json();
      })
      .then(function (data) {
        state.items = data.items || [];
        if (Array.isArray(data.categories)) {
          state.categories = data.categories;
        }
        renderMenu();
        renderView();
        bindGlobalKeys();
      })
      .catch(function (err) {
        showError('Failed to load catalog: ' + err.message);
      });
  });

  function showError(msg) {
    var main = document.getElementById('main-view');
    if (!main) return;
    main.innerHTML = '<div class="error">' + escapeHtml(msg) + '</div>';
  }

  // ---- Menu ----
  var MENU_ITEMS = [
    'All Games', 'PSP Games', 'PS3 Games',
    'Emulators', 'Demos', 'Utilities',
    'Recently Added', 'Favorites', 'Downloads',
    'Settings'
  ];

  function renderMenu() {
    var bar = document.getElementById('menu-bar');
    if (!bar) return;
    bar.innerHTML = '';
    MENU_ITEMS.forEach(function (label) {
      var btn = document.createElement('button');
      btn.type = 'button';
      btn.className = 'menu-item' + (state.active === label ? ' active' : '');
      btn.textContent = label;
      btn.addEventListener('click', function () {
        if (label === 'Settings') {
          state.active = label;
          renderMenu();
          openSettings();
          return;
        }
        state.active = label;
        state.selected = 0;
        renderMenu();
        renderView();
      });
      bar.appendChild(btn);
    });

    var toggle = document.getElementById('view-toggle');
    if (toggle) {
      toggle.textContent = state.view === 'cover' ? 'Grid' : 'Cover';
      toggle.onclick = function () {
        state.view = state.view === 'cover' ? 'grid' : 'cover';
        renderMenu();
        renderView();
      };
    }
  }

  function openSettings() {
    var main = document.getElementById('main-view');
    if (!main) return;
    main.innerHTML =
      '<div class="settings-panel">' +
        '<h2>Settings</h2>' +
        '<div class="row">Catalog URL: <b>' + escapeHtml(CATALOG_URL) + '</b></div>' +
        '<div class="row">Favorites stored locally in your browser.</div>' +
        '<div class="row">Press <b>g</b> to toggle Cover/Grid view.</div>' +
      '</div>';
    var meta = document.getElementById('meta-panel');
    if (meta) meta.innerHTML = '';
  }

  // ---- View rendering ----
  function currentList() {
    if (state.active === 'Settings') return [];
    return state.items.filter(function (it) { return matchesFilter(it, state.active); });
  }

  function renderView() {
    var main = document.getElementById('main-view');
    var meta = document.getElementById('meta-panel');
    if (!main) return;

    if (state.active === 'Settings') { openSettings(); return; }

    var list = currentList();
    if (!list.length) {
      main.className = 'main-view';
      main.innerHTML = '<div class="empty">No items in ' + escapeHtml(state.active) + '.</div>';
      if (meta) meta.innerHTML = '';
      return;
    }
    if (state.selected >= list.length) state.selected = list.length - 1;
    if (state.selected < 0) state.selected = 0;

    if (state.view === 'cover') {
      main.className = 'main-view cover-mode';
      main.innerHTML = renderCoverFlowHTML(list);
      bindCoverFlow(list);
    } else {
      main.className = 'main-view grid-mode';
      main.innerHTML = renderGridHTML(list);
      bindGrid(list);
    }
    renderMeta(list[state.selected]);
  }

  // ---- Cover Flow ----
  function renderCoverFlowHTML(list) {
    var html = '<div class="coverflow-stage" tabindex="0">' +
               '<button class="cf-arrow left"  aria-label="Previous">‹</button>' +
               '<div class="coverflow-track">';
    for (var i = 0; i < list.length; i++) {
      var it = list[i];
      var offset = i - state.selected;
      var cls = 'cover';
      if (Math.abs(offset) > 4) cls += ' is-hidden';
      else if (offset === 0) cls += ' is-center';
      else if (Math.abs(offset) === 1) cls += ' is-near';
      else cls += ' is-far';
      html +=
        '<div class="' + cls + '" style="--off:' + offset + '">' +
          coverArtHTML(it) +
          '<div class="cover-title">' + escapeHtml(it.title || 'Untitled') + '</div>' +
        '</div>';
    }
    html += '</div>' +
            '<button class="cf-arrow right" aria-label="Next">›</button>' +
            '</div>';
    return html;
  }

  function coverArtHTML(it) {
    if (it.icon) {
      return '<div class="cover-art" style="background-image:url(\'' + escapeAttr(it.icon) + '\')"></div>';
    }
    var label = (it.title || '?').slice(0, 2).toUpperCase();
    return '<div class="cover-art placeholder" data-label="' + escapeAttr(label) + '"></div>';
  }

  function bindCoverFlow(list) {
    var stage = document.querySelector('.coverflow-stage');
    if (!stage) return;
    stage.focus();
    var covers = stage.querySelectorAll('.cover');
    covers.forEach(function (el, idx) {
      el.addEventListener('click', function () {
        if (idx === state.selected) {
          // Toggle favorite on the highlighted cover.
          toggleFav(list[state.selected].id);
          renderView();
        } else {
          state.selected = idx;
          renderView();
        }
      });
    });
    var left = stage.querySelector('.cf-arrow.left');
    var right = stage.querySelector('.cf-arrow.right');
    if (left)  left.onclick  = function () { move(-1); };
    if (right) right.onclick = function () { move(1); };
  }

  function move(delta) {
    var list = currentList();
    if (!list.length) return;
    state.selected = Math.max(0, Math.min(list.length - 1, state.selected + delta));
    renderView();
  }

  // ---- Grid ----
  function renderGridHTML(list) {
    var html = '<div class="grid-stage">';
    list.forEach(function (it, i) {
      var cls = 'grid-card' + (i === state.selected ? ' is-selected' : '');
      html +=
        '<div class="' + cls + '" data-i="' + i + '">' +
          coverArtHTML(it) +
          '<div class="cover-title">' + escapeHtml(it.title || 'Untitled') + '</div>' +
        '</div>';
    });
    html += '</div>';
    return html;
  }

  function bindGrid(list) {
    var stage = document.querySelector('.grid-stage');
    if (!stage) return;
    stage.querySelectorAll('.grid-card').forEach(function (el, idx) {
      el.addEventListener('click', function () {
        state.selected = idx;
        renderView();
      });
      el.addEventListener('dblclick', function () {
        toggleFav(list[state.selected].id);
        renderView();
      });
    });
  }

  // ---- Metadata panel ----
  function renderMeta(it) {
    var meta = document.getElementById('meta-panel');
    if (!meta || !it) return;
    var fav = state.favorites.indexOf(it.id) !== -1;
    meta.innerHTML =
      '<div class="meta-inner">' +
        '<div class="meta-title">' + escapeHtml(it.title || 'Untitled') + '</div>' +
        '<div class="meta-row"><b>Category:</b> ' + escapeHtml(it.category || '—') + '</div>' +
        '<div class="meta-row"><b>Platform:</b> ' + escapeHtml(it.platform || '—') + '</div>' +
        '<div class="meta-row"><b>Size:</b> ' + escapeHtml(it.size || '—') + '</div>' +
        '<div class="meta-row"><b>ID:</b> ' + escapeHtml(it.id || '—') + '</div>' +
        '<button class="fav-btn' + (fav ? ' is-fav' : '') + '" type="button">' +
          (fav ? '★ Favorited' : '☆ Add to Favorites') +
        '</button>' +
      '</div>';
    var btn = meta.querySelector('.fav-btn');
    if (btn) btn.onclick = function () {
      toggleFav(it.id);
      renderMeta(it);
      if (state.active === 'Favorites') renderView();
    };
  }

  // ---- Keyboard ----
  function bindGlobalKeys() {
    document.addEventListener('keydown', function (e) {
      if (state.active === 'Settings') return;
      if (state.view !== 'cover') return;
      if (e.key === 'ArrowLeft')  { e.preventDefault(); move(-1); }
      else if (e.key === 'ArrowRight') { e.preventDefault(); move(1); }
      else if (e.key === 'Enter' || e.key === ' ') {
        var list = currentList();
        if (list[state.selected]) {
          e.preventDefault();
          toggleFav(list[state.selected].id);
          renderView();
        }
      } else if (e.key === 'g' || e.key === 'G') {
        state.view = state.view === 'cover' ? 'grid' : 'cover';
        renderMenu(); renderView();
      }
    });
  }

  function escapeHtml(s) {
    return String(s == null ? '' : s)
      .replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;').replace(/'/g, '&#39;');
  }
  function escapeAttr(s) {
    // quote-safe for use inside style="url('…')" attribute.
    return String(s == null ? '' : s).replace(/'/g, "\\'");
  }
})();
