#!/usr/bin/env python3
"""Build docs/web-gui-reference.html: a static, self-contained reference of
the PS Linker web GUI (server/store/ui) - real ui.css inlined, real markup
reproduced from ui.js/manage.js/uploads.js/store.js/dock.js, real catalog
data from the running test server. Re-run to refresh after UI changes."""
import json, os, subprocess, urllib.request

HOME = os.path.expanduser("~")
UI = "server/store/ui"
css = open(os.path.join(UI, "ui.css")).read()
try:
    cat = json.load(urllib.request.urlopen("http://127.0.0.1:18000/catalog.json", timeout=5))
except Exception:
    cat = json.load(open(os.path.join(HOME, "ps-linker-test/games/catalog.json")))
items = cat["entries"]

def esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))

def fmt_size(b):
    n = float(b or 0); u = 0
    for unit in ["B", "KB", "MB", "GB", "TB"]:
        if n < 1024 or u == 4:
            return (str(round(n)) if u == 0 else f"{n:.1f}" if n < 10 else f"{n:.0f}") + " " + unit
        n /= 1024; u += 1

def hue_of(item):  # state.js placeholderBG
    h = 0; s = str(item.get("id") or item.get("title") or "")
    for ch in s: h = (h * 31 + ord(ch)) & 0x3FFFFFFF
    hue = h % 360
    return (f"background:linear-gradient(160deg,hsl({hue} 42% 26%),"
            f"hsl({(hue + 40) % 360} 52% 12%))")

def cover(item, cls):
    label = (item.get("title") or "?").strip()[:2].upper()
    return (f'<div class="{cls} placeholder" style="{hue_of(item)}" '
            f'data-label="{esc(label)}"></div>')

def plat_of(it):
    p = (it.get("platform") or "").lower()
    return p if p in ("psp", "ps3") else None

FAVS = {"Midnight_Club_3_DUB_Edition_v2_USA_READNFO_PSP-Googlecus.iso"}  # favourite one card
def is_fav(it): return (it.get("id") or it.get("title")) in FAVS
# one card also shown selected (accent ring) in the library mock
def is_sel(it): return it.get("id", "").startswith("Midnight")

def card(it, selected=None):
    if selected is None: selected = is_sel(it)
    plat = plat_of(it)
    catg = it.get("category") or "Uncategorized"
    seg = (" \u00b7 " + plat.upper()) if plat and catg.upper() != plat.upper() else ""
    fav = is_fav(it)
    return (
        f'<div class="card{" is-selected" if selected else ""}" tabindex="0" role="listitem">'
        + cover(it, "card-cover")
        + f'<button class="fav-star{" on" if fav else ""}" type="button" aria-label="Toggle favorite">'
        + ("\u2605" if fav else "\u2606") + "</button>"
        + f'<div class="card-body"><div class="card-title">{esc(it.get("title"))}</div>'
        + f'<div class="card-meta">{esc(catg)}{seg} \u00b7 {fmt_size(it.get("size"))}</div></div></div>')

browse = [i for i in items]
folders = {}
for i in browse: folders[i.get("category") or "Uncategorized"] = folders.get(i.get("category") or "Uncategorized", 0) + 1
n_psp = sum(1 for i in browse if plat_of(i) == "psp")
n_ps3 = sum(1 for i in browse if plat_of(i) == "ps3")
n_fav = sum(1 for i in browse if is_fav(i))

def nav_item(label, count, active=False):
    return (f'<button class="nav-item{" active" if active else ""}" type="button">'
            f'<span>{esc(label)}</span><span class="nav-count">{count}</span></button>')

def sidebar(active="all", grid=None):
    nav = ['<div class="nav-group">Library</div>',
           nav_item("All Games", len(browse), active == "all"),
           nav_item("Favorites", n_fav, active == "favorites"),
           nav_item("Recently Viewed", 2, active == "recent")]
    if n_psp:
        nav += ['<div class="nav-group">Platforms</div>', nav_item("PSP", n_psp, active == "psp")]
    if n_ps3:
        nav += [nav_item("PS3", n_ps3, active == "ps3")]
    nav += ['<div class="nav-group">Folders</div>']
    nav += [nav_item(c, n, active == "folder:" + c) for c, n in sorted(folders.items())]
    return (
        '<aside class="sidebar"><div class="brand"><span class="brand-mark">PS Linker</span>'
        '<button class="icon-btn" aria-label="Close menu" hidden>\u2715</button></div>'
        f'<nav class="nav-scroll">{"".join(nav)}</nav>'
        '<div class="sidebar-foot">'
        '<button class="nav-item" type="button" title="Store \u2014 download queue">\u2b07 Store '
        '<span class="nav-count" id="store-badge">1</span></button>'
        '<button class="nav-item" type="button">\u2699 Settings</button></div></aside>')

def toolbar():
    sorts = ["title", "title-desc", "size-desc", "size-asc"]
    labs = {"title": "Title A\u2013Z", "title-desc": "Title Z\u2013A",
            "size-desc": "Largest first", "size-asc": "Smallest first"}
    return ('<header class="toolbar"><button class="icon-btn" aria-label="Open menu" hidden>\u2630</button>'
            '<input class="search" type="search" placeholder="Search library" aria-label="Search library" />'
            '<select class="select" aria-label="Sort by">'
            + "".join(f'<option value="{v}"{" selected" if v=="title" else ""}>{labs[v]}</option>' for v in sorts)
            + '</select><button class="btn btn-primary" type="button">\uff0b Add Games</button></header>')

def grid_html(items_):
    return '<div class="grid" role="list">' + "".join(card(i) for i in items_) + "</div>"

def lib_grid():
    return f'<main class="library" aria-label="Games">{grid_html(browse)}</main>'

def screen(body, cls=""):
    return f'<div class="screen {cls}" >{body}</div>'

# ---- detail drawer (manage.js render()) ----
dr = next(i for i in browse if (i.get("title") or "").startswith("PSP Homebrew"))
drawer = (
    '<aside class="drawer" aria-label="Game details"><div class="drawer-inner">'
    + cover(dr, "drawer-cover")
    + f'<div class="drawer-title">{esc(dr["title"])}<span class="chip">{esc(dr.get("platform"))}</span></div>'
    + f'<div class="drawer-sub">{esc(dr["category"])} \u00b7 {fmt_size(dr["size"])}</div>'
    + '<dl class="kv"><dt>Version</dt><dd>\u2014</dd>'
      '<dt>File</dt><dd>PSP_Homebrew.PBP</dd>'
      '<dt>ID</dt><dd>Homebrew_PSP_Homebrew.PBP</dd></dl>'
    '<div class="drawer-form lic-row"><h4>Cover</h4>'
      '<div class="lic-state">Placeholder tile \u2014 no ICON0 found inside the game</div>'
      '<div class="form-row"><button class="btn btn-ghost" type="button">\u2b71 Upload art</button>'
      '<button class="btn btn-ghost" type="button">\u21bb Re-extract</button></div></div>'
    '<div class="drawer-form lic-row"><h4>License (RAP)</h4>'
      '<div class="lic-state">No license attached</div>'
      '<div class="form-row"><button class="btn btn-ghost" type="button">Attach license</button></div></div>'
    '<div class="drawer-actions">'
      '<button class="btn a-fav" type="button">\u2606 Favorite</button>'
      '<a class="btn" href="#">\u2b73 Download</a>'
      '<button class="btn btn-ghost" type="button">\u270e Rename file</button>'
      '<button class="btn btn-ghost" type="button">\u21c4 Move to folder</button>'
      '<button class="btn btn-ghost" type="button" style="grid-column:1/-1">\u270e Metadata</button>'
      '<button class="btn btn-danger" type="button" style="grid-column:1/-1">\U0001f5d1 Delete</button>'
    '</div></div></aside>')

# ---- upload rows (uploads.js rowEl) in every state ----
def up_row(name, size, pct, status, cls="", spin=False):
    return (f'<li class="up-row {cls}"><div><div class="up-name">'
            + ("<span class=\"spin\"></span>" if spin else "") + esc(name)
            + f'</div><div class="up-size">{size}</div></div>'
            + f'<div class="up-bar"><div class="up-fill" style="width:{pct}%"></div></div>'
            + f'<div class="up-status">{esc(status)}</div></li>')

add_modal = (
    '<div class="modal-scrim"><div class="modal" role="dialog" aria-modal="true" aria-label="Add games">'
    '<button class="icon-btn close-x" aria-label="Close">\u2715</button><h3>Add Games</h3>'
    '<div class="field"><label for="ref-plat">Platform</label><select id="ref-plat">'
    '<option>PSP</option><option selected>PS3</option></select></div>'
    '<div class="drop-zone" tabindex="0">Choose files or drop them here'
    '<div class="drop-hint">iso cso pbp zip 7z rar pkg rap bin cue \u2014 files land in the library '
    'root, tagged for the platform you pick, and appear in its tab at once</div>'
    '<div class="drop-hint hint-ps3">Licenses: upload a .rap together with its game, then attach it '
    'from the game\u2019s drawer. Only supply .rap files for games you own.</div></div>'
    '<ul class="up-list">'
    + up_row("Need for Speed Carbon - Own the City (Asia).iso", "1.4 GB", 0, "Waiting")
    + up_row("Midnight_Club_3_DUB_Edition_v2_USA_READNFO_PSP-Googglecus.iso", "1.1 GB", 45, "45%", "is-active", spin=False)
    + up_row("Game_Demo_1.0.pkg", "512 KB", 100, "\u2713 Added", "is-ok")
    + up_row("notes.txt", "4 KB", 0, "type not allowed", "is-err")
    + "</ul></div></div>")

# ---- Store modal (store.js openStore + renderModal) ----
store_modal = (
    '<div class="modal-scrim"><div class="modal" role="dialog" aria-modal="true" aria-label="Store">'
    '<button class="icon-btn close-x" aria-label="Close">\u2715</button><h3>Store</h3>'
    '<div class="up-row is-active q-live"><div><div class="up-name">'
    '<span class="spin"></span>Homebrew Pack</div><div class="up-size">Homebrew \u00b7 4.2 MB / 8.4 MB</div></div>'
    '<div class="up-bar"><div class="up-fill" style="width:50%"></div></div>'
    '<div class="up-status">Downloading 50%</div></div>'
    '<div class="form-row"><button class="btn btn-primary" type="button">\u2b07 Download now</button></div>'
    '<div class="q-add"><div class="field"><label>Download URL</label>'
    '<input type="url" placeholder="https://example.com/game.zip" /></div>'
    '<div class="form-row"><div class="field"><label>Title (optional)</label><input type="text" /></div>'
    '<div class="field"><label>Category</label><input type="text" value="Homebrew" /></div></div>'
    '<div class="form-row"><button class="btn btn-primary" type="button">\uff0b Add to queue</button></div></div>'
    '<h4 class="q-head">Queue</h4><ul class="up-list q-list">'
    '<li class="up-row q-row"><div><div class="up-name">Retro Ports Vol. 2</div>'
    '<div class="up-size">Homebrew \u00b7 https://example.com/retro2.zip</div></div><div></div>'
    '<div class="q-actions"><span class="up-status">Queued</span>'
    '<button class="icon-btn" aria-label="Remove from queue" type="button">\u2715</button></div></li></ul>'
    '<h4 class="q-head">Recent downloads</h4><ul class="up-list q-list">'
    '<li class="up-row is-ok"><div><div class="up-name">PSP Docviewer</div>'
    '<div class="up-size">Homebrew \u00b7 812 KB \u00b7 2 h ago</div></div><div></div>'
    '<div class="up-status">\u2713 4s</div></li>'
    '<li class="up-row is-err"><div><div class="up-name">Old Zip</div>'
    '<div class="up-size">Homebrew \u00b7 0 B \u00b7 3 d ago</div></div><div></div>'
    '<div class="up-status">HTTP 404</div></li></ul></div></div>')

# ---- transfer dock (dock.js + uploads.js + store.js renderDock) ----
dock = (
    '<div class="dock"><div class="dock-head"><span>Transfers</span>'
    '<span class="dock-caret">\u25be</span></div><div class="dock-body">'
    '<div class="dock-sect" data-sect="uploads"><div class="dock-sect-head"><span>Uploads</span>'
    '<span class="dock-sect-info">1 to go</span><span class="dock-caret">\u25be</span></div>'
    '<div class="dock-sect-body"><ul class="up-list">'
    + up_row("Midnight_Club_3_DUB_Edition_v2_USA_READNFO_PSP-Googglecus.iso", "1.1 GB", 45, "45%", "is-active")
    + up_row("notes.txt", "4 KB", 0, "type not allowed", "is-err")
    + "</ul></div></div>"
    '<div class="dock-sect" data-sect="store"><div class="dock-sect-head"><span>Store</span>'
    '<span class="dock-sect-info"></span><span class="dock-caret">\u25be</span></div>'
    '<div class="dock-sect-body"><ul class="up-list">'
    '<li class="up-row is-active"><div><div class="up-name"><span class="spin"></span>Homebrew Pack</div>'
    '<div class="up-size">Homebrew \u00b7 4.2 MB / 8.4 MB</div></div>'
    '<div class="up-bar"><div class="up-fill" style="width:50%"></div></div>'
    '<div class="up-status">Downloading 50%</div></li>'
    '<li class="up-row is-ok"><div><div class="up-name">PSP Docviewer</div>'
    '<div class="up-size">Homebrew \u00b7 812 KB \u00b7 2 h ago</div></div><div></div>'
    '<div class="up-status">\u2713 4s</div></li>'
    '</ul></div></div></div></div>')

toasts = ('<div class="toasts"><div class="toast ok">Added Midnight Club 3</div>'
          '<div class="toast err">notes.txt: type not allowed</div>'
          '<div class="toast">Catalog updated \u00b7 8 entries</div></div>')

tokens = [
    ("--bg", "app background"), ("--chrome", "sidebar / toolbar / modal"),
    ("--surface", "cards, rows, inputs"), ("--surface-2", "hover, dock head, code"),
    ("--line", "hairline borders"), ("--line-strong", "control borders"),
    ("--text", "primary text"), ("--muted", "secondary text"), ("--faint", "labels, hints"),
    ("--accent", "primary button"), ("--accent-strong", "links, active, fills"),
    ("--ok", "success \u2713"), ("--warn", ".rap / PS3 hints"), ("--danger", "delete, errors"),
    ("--fav", "favourite star"),
]
swatches = "".join(
    f'<div class="sw"><span class="sw-chip" style="background:var({v})"></span>'
    f'<code>{v}</code><span class="sw-desc">{d}</span></div>'
    for v, d in tokens)

REF = """
html,body{height:auto;overflow:visible}
body{overflow:auto;padding-bottom:80px}
.ref-head{max-width:1180px;margin:0 auto;padding:34px 20px 6px}
.ref-head h1{font-size:22px;margin:0 0 6px}
.ref-head p{color:var(--muted);max-width:80ch;margin:6px 0}
.ref-wrap{max-width:1180px;margin:0 auto;padding:0 20px}
h2.ref-sec{font-size:13px;text-transform:uppercase;letter-spacing:.8px;
  color:var(--faint);margin:38px 0 10px}
.ref-note{font-size:12.5px;color:var(--muted);max-width:90ch;margin:8px 0 14px}
.ref-note code, .sw code{background:var(--surface-2);border-radius:4px;
  padding:1px 6px;font-size:12px;color:var(--text);font-family:ui-monospace,monospace}
.screen{position:relative;border:1px solid var(--line-strong);border-radius:12px;
  overflow:hidden;box-shadow:var(--shadow);background:var(--bg);margin:6px 0}
.screen .shell{height:100%;width:100%}
.screen .modal-scrim,.screen .dock,.screen .toasts,.screen .drop-overlay{position:absolute}
.row2{display:grid;grid-template-columns:1fr 1fr;gap:16px}
.row3{display:grid;grid-template-columns:1fr 1fr 1fr;gap:16px}
@media (max-width:900px){.row2,.row3{grid-template-columns:1fr}}
.sw{display:flex;align-items:center;gap:10px;margin:5px 0;font-size:12.5px}
.sw-chip{width:26px;height:26px;border-radius:6px;border:1px solid var(--line-strong);flex:0 0 auto}
.sw-desc{color:var(--muted)}
.screen.light{--bg:#eef2f7;--chrome:#fff;--surface:#fff;--surface-2:#f2f5f9;
  --line:#dbe2ea;--line-strong:#c6d0dc;--text:#1c2733;--muted:#576879;--faint:#7b8794;
  --accent:#2f6feb;--accent-strong:#2f6feb;--accent-soft:rgba(47,111,235,.10);
  --accent-line:rgba(47,111,235,.45);--ok:#1a7f37;--warn:#9a6700;--danger:#c62f3f;
  --danger-soft:rgba(198,47,63,.08);--fav:#b58900;
  --shadow:0 10px 30px rgba(30,45,70,.16);--shadow-soft:0 2px 10px rgba(30,45,70,.10)}
/* Mock the <=860px layout at desktop page size (the media query cannot
 * trigger inside the reference page): same geometry, scoped selectors. */
.screen.narrow .sidebar{position:absolute;top:0;bottom:0;left:0;z-index:40;
  transform:translateX(-100%);transition:none;box-shadow:var(--shadow)}
.screen.narrow .sidebar.open{transform:none}
.screen.narrow .scrim{position:absolute;inset:0;z-index:35;background:rgba(0,0,0,.45)}
.screen.narrow .drawer.sheet{position:absolute;left:0;right:0;bottom:0;z-index:30;
  max-height:70%;border-left:0;border-top:1px solid var(--line);
  border-radius:14px 14px 0 0;box-shadow:var(--shadow)}
"""

html = f"""<!doctype html>
<html lang="en" data-theme="dark">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width,initial-scale=1" />
<title>PS Linker \u2014 Web GUI reference</title>
<style>
{css}
</style>
<style>
{REF}
</style>
</head>
<body>

<div class="ref-head">
  <h1>PS Linker \u2014 Web GUI reference</h1>
  <p>A static, self-contained portrait of the <b>current server UI</b>
     (<code>server/store/ui/</code>, served at <code>http://this-host:PORT/gui/</code>).
     Every screen below is rendered with the GUI's real stylesheet
     (<code>ui.css</code>, inlined) and markup copied from
     <code>ui.js / manage.js / uploads.js / store.js / dock.js</code>; the
     library data is the live <code>catalog.json</code>. Use it as the visual
     baseline for layout work \u2014 anything that drifts from these frames is a
     bug in one of the two.</p>
  <p>Tokens: dark theme <code>data-theme="dark"</code> (default; a light theme
     overrides only the token block). Radius <code>--radius:10px</code>,
     controls <code>--radius-sm:6px</code>, transitions <code>160ms</code>,
     no dependencies, no build step.</p>
</div>

<div class="ref-wrap">

<h2 class="ref-sec">1 \u00b7 Design tokens</h2>
<div class="row2">
  <div>{swatches}</div>
  <div>
    <div class="ref-note">Type: 14px system sans; card titles 12.5px/550;
      group labels 10.5px uppercase +0.8px tracking; status/counts 11\u201312px.
      Layout: 224px sidebar \u00b7 320px drawer \u00b7 fluid card grid
      (min 150px tiles, 3:4 covers) \u00b7 560px modal \u00b7 400px dock.
      Below <b>860px</b> the sidebar goes off-canvas (burger
      <code>\u2630</code>) and the drawer becomes a bottom sheet.</div>
    <div class="ref-note">The whole palette in the <b>light</b> theme, same
      components (Settings \u2192 Theme writes
      <code>pslinker.settings.v1</code> into localStorage):</div>
    {screen('<div class="shell" style="height:190px">' + toolbar() +
            '<main class="library"><div class="state-box"><div class="state-big">Light theme</div>'
            'Token override only \u2014 components are unchanged</div></main></div>', 'light')}
  </div>
</div>

<h2 class="ref-sec">2 \u00b7 Library \u2014 the app shell</h2>
<div class="ref-note">Sidebar = <code>sections()</code> (All / Favorites / Recently Viewed, then
Platforms PSP/PS3 when present, then one entry per catalog <i>Folder</i>, each with a count pill);
foot holds Store (badge = queue activity) and Settings. Toolbar = search, sort
(<code>SORTS</code>), <b>\uff0b Add Games</b>. Cards come straight from
<code>catalog.json</code>: placeholder covers are an id-hashed gradient with the title's first two
letters, <code>\u2606</code> favours, one selected card shows the accent ring.</div>
{screen('<div class="shell" style="height:640px">' + sidebar() +
        '<div class="main-col">' + toolbar() + lib_grid() + '</div></div>')}

<h2 class="ref-sec">3 \u00b7 Detail drawer (click a card)</h2>
<div class="ref-note">Cover tile, title + platform <code>chip</code>, \u201ccategory \u00b7 size\u201d,
the Version/File/ID <code>kv</code> grid, then the Cover and License (RAP) boxes
(<code>manage.js</code>) and the 2-column action grid: favourite, download
(straight <code>&lt;a download&gt;</code> against the share), rename, move, metadata, delete
(armed twice). <code>.rap</code> files get a key badge \U0001f511 on their card (red when the
license file is gone) and a dashed border.</div>
{screen('<div class="shell" style="height:640px">' +
        '<div class="main-col">' + toolbar() +
        '<main class="library" aria-label="Games">' + grid_html(browse[:5]) + '</main></div>'
        + drawer + '</div>')}

<h2 class="ref-sec">4 \u00b7 Add Games modal (uploads)</h2>
<div class="ref-note">One panel, both paths: click the dashed zone, or drop files anywhere in the
window (full-screen <b>Drop to add games</b> overlay). Files go to the library root tagged
<code>psp|ps3</code> and land in their tab immediately. Rows are one-at-a-time uploads in four
states (Waiting / % / \u2713 Added / error). When the panel closes, finished rows are discarded and
the box retires \u2014 only in-flight uploads (and unseen failures) carry on in the dock.</div>
{screen('<div class="shell" style="height:600px">' + sidebar() +
        '<div class="main-col">' + toolbar() + lib_grid() + '</div></div>' + add_modal)}

<h2 class="ref-sec">5 \u00b7 Store modal (server-side downloads)</h2>
<div class="ref-note">The server's own fetcher: a live row while it works, \u2b07 Download now to
kick it, a queue form (URL + optional title + one-folder category), and the download history.
Queue and history rows reuse the same <code>.up-row</code> grid as uploads.</div>
{screen('<div class="shell" style="height:620px">' + toolbar() +
        '<main class="library">' + grid_html(browse[:3]) + '</main></div>' + store_modal)}

<h2 class="ref-sec">6 \u00b7 Transfers dock + toasts</h2>
<div class="ref-note">The corner dock (<code>dock.js</code>) is one shell with an Uploads and a
Store section, each collapsible; it appears only while a module has activity and retires itself a
few seconds after the last transfer lands. Toasts bottom-left carry the same states (accent /
green / red left rail).</div>
{screen('<div class="shell" style="height:520px">' + sidebar() +
        '<div class="main-col">' + toolbar() + lib_grid() + '</div></div>' + dock + toasts)}

<h2 class="ref-sec">7 \u00b7 Empty, error and drop states</h2>
<div class="row3">
{screen('<div class="shell" style="height:300px">' + '<div class="main-col">' + toolbar() +
        '<main class="library"><div class="state-box"><div class="state-big">Nothing in this view</div>'
        'Use \uff0b Add Games to upload an ISO, PBP, or zip \u2014 or drop files anywhere on this page.</div></main></div></div>')}
{screen('<div class="shell" style="height:300px">' + '<div class="main-col">' + toolbar() +
        '<main class="library"><div class="state-box err">Could not load the catalog: '
        'Failed to fetch</div></main></div></div>')}
{screen('<div class="shell" style="height:300px">' + '<div class="main-col">' + toolbar() +
        '<main class="library">' + grid_html(browse[:3]) + '</main></div></div>'
        '<div class="drop-overlay" style="display:flex">Drop to add games</div>')}
</div>

<h2 class="ref-sec">8 \u00b7 Settings modal</h2>
{screen('<div class="shell" style="height:430px">' + '<div class="main-col">' + toolbar() +
        '<main class="library">' + grid_html(browse[:3]) + '</main></div></div>'
        '<div class="modal-scrim"><div class="modal" role="dialog" aria-label="Settings">'
        '<button class="icon-btn close-x" aria-label="Close">\u2715</button><h3>Settings</h3>'
        '<div class="field"><label>Theme</label><select><option>Follow system</option>'
        '<option selected>Dark</option><option>Light</option></select></div>'
        '<div class="field"><label>Default sort</label><select><option selected>Title A\u2013Z</option>'
        '<option>Title Z\u2013A</option><option>Largest first</option><option>Smallest first</option></select></div>'
        '<div class="settings-kv"><div>Catalog: <code>GET /api/catalog</code> \u2014 '
        + str(len(items)) + ' entries</div>'
        '<div>Local data: ' + str(n_fav) + ' favorites, 2 recently viewed (kept in this browser only)</div>'
        '<div><button class="btn btn-ghost" type="button">Reset local data</button></div></div>'
        '</div></div>')}

<h2 class="ref-sec">9 \u00b7 Narrow screens (&le;860px)</h2>
<div class="row2">
{screen('<div class="shell" style="height:480px">' +
        '<aside class="sidebar open"><div class="brand"><span class="brand-mark">PS Linker</span>'
        '<button class="icon-btn" aria-label="Close menu">\u2715</button></div>'
        '<nav class="nav-scroll">' + "".join(x for x in [
            '<div class="nav-group">Library</div>',
            nav_item("All Games", len(browse), True),
            nav_item("Favorites", n_fav), nav_item("Recently Viewed", 2),
            '<div class="nav-group">Platforms</div>',
            nav_item("PSP", n_psp), nav_item("PS3", n_ps3),
            '<div class="nav-group">Folders</div>',
        ] + [nav_item(c, n) for c, n in sorted(folders.items())]) + '</nav>'
        '<div class="sidebar-foot"><button class="nav-item" type="button">\u2b07 Store</button>'
        '<button class="nav-item" type="button">\u2699 Settings</button></div></aside>'
        '<div class="scrim"></div>'
        '<div class="main-col">' + toolbar().replace('hidden>\u2630', '>\u2630') +
        '<main class="library">' + grid_html(browse[:3]) + '</main></div></div>', 'narrow')}
{screen('<div class="shell" style="height:480px">' +
        '<div class="main-col">' + toolbar().replace('hidden>\u2630', '>\u2630') +
        '<main class="library">' + grid_html(browse[:2]) + '</main></div>'
        '<aside class="drawer sheet">'
        '<div class="drawer-inner">' + cover(dr, "drawer-cover") +
        f'<div class="drawer-title">{esc(dr["title"])}<span class="chip">PSP</span></div>'
        f'<div class="drawer-sub">{esc(dr["category"])} \u00b7 {fmt_size(dr["size"])}</div>'
        '<div class="drawer-actions"><button class="btn" type="button">\u2606 Favorite</button>'
        '<a class="btn" href="#">\u2b73 Download</a></div></div></aside></div>', 'narrow')}
</div>

<div class="ref-note" style="margin-top:30px">Generated from the working tree \u2014 re-run
<code>python3 scripts/gen-web-gui-reference.py</code> after UI changes. The live app is at
<code>/gui/</code> on the share port (test server: <code>http://10.0.0.63:18000/gui/</code>).</div>
</div>
</body></html>
"""

out = "docs/web-gui-reference.html"
open(out, "w").write(html)
print(f"wrote {out} ({len(html)} bytes, {len(items)} catalog entries)")
