# PS Linker — web GUI reference

The browser shell served by the HTTP share (`server/store/ui/`). This is the
web side; the on-device look is specified in [UI-REFERENCE.md](UI-REFERENCE.md)
and the shared device-side Cover Flow module in
[`ui/INTEGRATION.md`](../ui/INTEGRATION.md).

## Where it is served

`http://<host>:<port>/gui/`, assets at `/gui/<asset>` (`/ui/*` is an alias).

The GUI sits under a prefix on purpose: `/` must stay the library's directory
listing, because that is the first request the PSP and PS3 apps make and what
the README points people at for downloads.

Routing in `server/store/http_server.py` is therefore: `/api/*`, then `/gui/`
(and `/ui/*`), then everything else resolved **inside the library directory** —
files with `Range`/`206` support, and listings in the format the device apps
parse.

## Files

- `server/store/ui/index.html` — application structure: menu/toolbar, Cover Flow
  carousel, grid toggle, selected-item detail pane, **Add Games** panel with
  its per-file upload list. Asset links are relative, so they resolve under
  whatever prefix serves the shell.
- `server/store/ui/ui.css` — the dark Media Go-inspired palette, blue selection
  states, dense utility chrome, responsive layout, cover-tile treatments.
- `server/store/ui/ui.js` — fetches `GET /api/catalog` and renders the
  catalogue. The URL is **absolute**; a relative one resolves against `/gui/`
  and 404s, which is how the shell came to display "Failed to load catalog"
  for every catalog. Placeholder artwork is never presented as live data; an
  empty or unavailable catalogue is shown as empty.

## JSON endpoints

| Endpoint | Method | Notes |
|---|---|---|
| `/api/catalog` | GET | `{"items": [...], "categories": [...]}`. Tolerates the `items` / `entries` / `games` key spellings in the file; `items` is the key `ui.js` reads. `categories` is passed through only when the catalogue declares one. |
| `/api/upload` | POST | `multipart/form-data` with a `platform` field (`psp`/`ps3`) and one or more `file` parts. Reads the whole body into memory, so it suits curl and small files; the shell does not use it. Writes into the library root (atomic `.part` + rename), rejects extensions outside the game set, then refreshes `catalog.json` in whatever top-level shape it already had. The Store regeneration loop rewrites that file from disk on its own timer, so this is a between-cycles convenience. |
| `/api/upload/file` | POST | The shell's Add Games path: one file per request, raw request body (no multipart), `?name=<url-encoded>&platform=<psp|ps3>`. The body is streamed to `<name>.part` in 256 KiB chunks and renamed in atomically, so uploads of ISO-sized files never buffer in memory. Same name/extension guards as `/api/upload`; chunked bodies get `411` (there is no `Content-Length` to trust). |
| `/api/upload/*` responses | | Both endpoints record the upload with one code path: the entry is merged into the catalog by `url` (so it joins rather than duplicates `generate_catalog.py`'s entry for the same file), and `platform`/`title` are written into the game's `<name>.meta.json` sidecar, which `generate_catalog.py` passes through — so uploads keep their prettified title and PSP/PS3 tab across regenerations. |

### The Add Games panel

The topbar **＋ Add Games** button opens a panel in place of the browse view
(Esc or *Back to library* returns). A PSP/PS3 select tags the uploads, a click
or drop feeds files into a queue that uploads one at a time via
`XMLHttpRequest` — the only way to get real upload-progress events without a
`fetch` streaming polyfill. Rows show progress, then per-file success or the
server's error; state is kept outside the DOM so reopening the panel
mid-queue re-renders live rows. Files land in the library root under the
`Uncategorized` category and the selected platform, which the PSP/PS3 menu
tabs filter on. The client-side extension check mirrors `ALLOWED_EXT` in
`server/store/http_server.py` and only pre-rejects hopeless rows; the server
remains the authority.

Download/sync controls in the shell are visual only until a PSP transfer API
exists.

## Menu tabs

`ui.js` filters on `platform` **or** a `/PSP/i` / `/PS3/i` regex over the title,
and derives extra tabs (`Emulators`, `Demos`, `Utilities`) from
`item.category`. `Downloads` and `Settings` are placeholders that match
nothing. The device module deliberately filters on `category` only — a title
regex misfiles things like "PSP Emulator Frontend" — so the two implementations
are not identical on purpose; see `ui/INTEGRATION.md`.

## Checks

`server/store/check_share.sh` covers the share this GUI depends on: listings,
`Range`/206/416, the `/gui/` routes, `/api/catalog`'s key, both upload paths
(streamed bytes land identical, rejections write nothing, chunked is refused),
that an upload never collapses the entries `generate_catalog.py` wrote, and
path traversal.
