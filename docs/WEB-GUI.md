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
  carousel, grid toggle, selected-item detail pane, transfer status bar. Asset
  links are relative, so they resolve under whatever prefix serves the shell.
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
| `/api/upload` | POST | `multipart/form-data` with a `platform` field (`psp`/`ps3`) and one or more `file` parts. Writes into the library root (atomic `.part` + rename), rejects extensions outside the game set, then refreshes `catalog.json` in whatever top-level shape it already had. The Store regeneration loop rewrites that file from disk on its own timer, so this is a between-cycles convenience. |

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
`Range`/206/416, the `/gui/` routes, `/api/catalog`'s key, uploads, and path
traversal.
