# PS Linker — UI Reference (Media Go derived)

This is the visual specification for PS Linker's browser interface, derived
from Sony's **Media Go** application and stripped of unrelated media-library
features. The PSP Store is documented separately; it renders the shared Cover
Flow state through its sceGu pixel renderer (its card surfaces, tab accents and
progress bar follow this document's look), with the original debug-screen
layout kept behind `-DNASDL_TEXT_UI`.

What remains is Media Go's **shell** (dark theme, sidebar, list view, status
bar, progress bar) applied to two screens: the **NAS browser** and the
**Download view**.

---

## 1. Design tokens

### Colors

| Token             | Hex       | Used for                                      |
|-------------------|-----------|-----------------------------------------------|
| `bg.base`         | `#1D1D1D` | App background (charcoal)                     |
| `bg.panel`        | `#262626` | Sidebar, list panel background                |
| `bg.header`       | `#111111` | Top title bar strip                           |
| `accent.primary`  | `#0F86E0` | Selection bar, progress fill, active tab      |
| `accent.primaryDk`| `#0B5FA0` | Pressed/active states, header underline       |
| `text.primary`    | `#FFFFFF` | Selected/highlighted text, screen title       |
| `text.secondary`  | `#9A9A9A` | Unselected rows, hints, metadata              |
| `text.disabled`   | `#5A5A5A` | Empty-state hints, unavailable items          |
| `status.ok`       | `#3FBF5A` | "Download complete"                           |
| `status.error`    | `#D9534F` | "Download failed", network errors             |

### Typography

- Face: system sans-serif (PSP debug font / small bitmap font on device).
- Screen title: bold, 1 line, `text.primary`.
- List rows: regular, `text.secondary`; selected row `text.primary`.
- Status bar / hints: small, `text.secondary`.
- Sizes are constrained to the PSP's 480×272 screen: ~30 text rows max,
  ~68 characters per line.

### Shape & spacing

- No rounded corners (the device renderers - debug screen and GU alike -
  have no compositing); "panels" are
  implied by background-color changes and 1-blank-column gutters.
- Sidebar fixed width: **14 columns** of text.
- List area: everything right of the sidebar, 1 blank column gutter.
- Header and footer bars: 1 full-width text row each, inverted colors
  (accent or dark background with light text).

---

## PSP Store category tabs

The Store's top menu is data-driven from the unique categories found in
`catalog.json`, shown in first-seen catalog order. The PSP L/R triggers move
between those categories; each tab filters by exact category name, matching
the generator's folder-based category values. The shared Cover Flow's standard
fixed menu remains available to other callers and is used as a fallback when
a caller provides no non-empty catalog category list. The PSP Store currently
caps this menu at eight categories (31 displayed bytes per label).

## 2. Layout regions

```
+--------------------------------------------------------------+
| HEADER BAR   (full width, 1 row, bg.header / accent)         |
+------------+-------------------------------------------------+
| SIDEBAR    |  LIST PANEL                                     |
| (bg.panel) |  (bg.base)                                      |
| 14 cols    |  file rows, scrolling, selection bar            |
|            |                                                 |
+------------+-------------------------------------------------+
| STATUS BAR   (full width, 1 row, accent bg, hints + counts)  |
+--------------------------------------------------------------+
```

### Header bar
- Left: app wordmark — `PS LINKER` in `text.primary`, followed by the
  current view name in `text.secondary` (e.g. `PS LINKER  ·  NAS LIBRARY`).
- Optional right side: NAS host name or connection state.

### Sidebar
Navigation entries (fixed list, not dynamic):

| Entry        | State behavior                                          |
|--------------|---------------------------------------------------------|
| `BROWSE`     | Active by default. Highlights in `accent.primary`.      |
| `DOWNLOADS`  | Shows last-completed transfer status when selected.     |
| `SETTINGS`   | Reserved: NAS URL / Wi-Fi profile editing.              |

- Active entry: full-width accent bar, `text.primary`.
- Inactive entries: `text.secondary` on `bg.panel`.
- The file-browser sidebar remains display-only; the Store is opened from the
  browser with Triangle.

### List panel (file browser)
- One row per file on the NAS share: filename only, no icons, no columns.
- **Selection**: full-width `accent.primary` bar, `text.primary` text.
- Unselected rows: `bg.base`, `text.secondary`.
- Scroll indicator: when the list overflows, the top/bottom-most visible
  row of the overflow edge is truncated with `...`.
- Empty state: centered `text.disabled` message
  ("No files found on NAS") with a hint row beneath ("Check server URL").

### Status bar
- Left: context-sensitive button hints —
  `X  Download    O  Back    ↑↓  Move` in `text.primary` on `accent.primary`.
- Right: live counters — `128 files` / `42%  3,104 KB` during transfer.

---

## 3. Screens

### 3.1 Splash / connect

- `bg.base` full screen.
- Wordmark `PS LINKER` centered, `text.primary`, plus subtitle
  `NAS DOWNLOADER` in `text.secondary`.
- Below: one status line that updates in place —
  `Connecting to Wi-Fi (profile 1)...` → `Connected: <SSID> (<IP>)`
  in `text.secondary`; on failure, message in `status.error`.
- No interaction; auto-advances to the browser on success.

### 3.2 Browse (main screen)

- Header: `PS LINKER · NAS LIBRARY`.
- Sidebar: BROWSE active.
- List panel: fetched NAS directory listing (see `parse_listing` in
  `psp/main.c` — flat list, subdirectories filtered out).
- Status bar: hints left, file count right.
- Interactions: ↑/↓ move selection (wraps), X = download, O = quit.

### 3.3 Download overlay

Replaces the list panel in place (header/sidebar/status stay):

```
Downloading
  EBOOT.PBP
  → ms0:/PSP/GAME/NASDL/EBOOT.PBP

  [██████████░░░░░░░░░░░░░░░░░░░░]  42%  (3,104 KB)
```

- Track: 30 characters wide; filled portion `accent.primary`,
  remainder `bg.panel`.
- Percentage + KB right of the track, `text.secondary`.
- On completion the status line becomes `Done!` in `status.ok`;
  on failure `Failed (error N).` in `status.error`.
- Any button returns to Browse.

---

## 4. Behavior rules

1. Never redraw the whole screen mid-download; only the progress row
   updates (uses `\r` carriage returns on the debug screen; the GU build
   instead repaints its own double-buffered install screen, so nothing
   flickers there).
2. Selection must always be visible: scrolling windows the list so the
   accent bar never leaves the panel.
3. All state messages (connecting, fetching, error) print in place —
   no modal dialogs.
4. Errors are always red text + a hint; the app returns to Browse,
   never exits unexpectedly.
5. Button hints in the status bar always match the current screen.

---

## 5. Intentionally excluded (from Media Go)

Music/video/photo/podcast libraries, SensMe, playlists, Device/Store
views, cover-art grids, column customization, drag-and-drop, and the
"Marketing" tray. If a future feature (e.g. PSP game backups preview)
is added, it should reuse the list-panel + status-bar pattern rather
than reintroducing those views.

---

## 6. Implementation mapping

| Reference element | Code location (`psp/main.c`)          |
|-------------------|----------------------------------------|
| Colors/tokens     | `#define` color block near top         |
| Header/footer bars| `draw_header()` / `draw_statusbar()`   |
| Sidebar           | `draw_sidebar()` (display-only, v1.2)  |
| Selection bar     | `draw_list()` row loop                 |
| Progress bar      | `download_file()` inline progress      |
| Splash            | `net_init()` + splash draw calls       |

The browser-side counterpart (the `/gui/` web shell) is documented in
[WEB-GUI.md](WEB-GUI.md).

