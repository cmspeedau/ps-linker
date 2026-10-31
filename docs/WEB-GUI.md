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
parse: plain `href` anchors, with our own shares additionally carrying
`data-size`/`data-cat` after the quote for the apps' row chrome (the apps
ignore what follows the closing quote, so foreign shares parse unchanged).

## Files

Dependency-free and build-free — the Docker image copies the folder, so the
shell is ES modules and modern CSS with no bundler and no framework.

| File | Role |
|---|---|
| `index.html` | App shell: sidebar (Store and Settings in its foot), toolbar, library grid, detail drawer, transfer dock, drop overlay, modal root, toast host. A tiny inline script applies the stored theme before first paint. |
| `ui.css` | Single stylesheet. Design tokens under `@layer tokens` (`[data-theme="light"]` overrides them), then base/layout/components. |
| `ui.js` | Entry module: boot, sidebar sections with counts, toolbar (search `/` to focus, sort), card grid with roving keyboard focus (arrows navigate, Enter opens, `F` favorites), settings modal, theme handling, mobile sidebar, Escape closes the top-most overlay (Store modal, Add modal, drawer). |
| `state.js` | Shared state + localStorage (`pslinker.favorites.v1`, `pslinker.recent.v1`, `pslinker.settings.v1`), section/sort/filter logic, and the helpers (escape, size format, deterministic placeholder covers, toasts). |
| `api.js` | Everything that talks to the JSON API: catalog fetch, upload, delete/rename/meta, and the Store queue/status/run calls. |
| `dock.js` | The shared transfer dock: one corner panel, one titled collapsible section per module (Uploads, Store); the dock itself only shows while some section has activity. |
| `uploads.js` | Add Games modal, the serial upload queue (its rows render into the dock's Uploads section, so progress survives closing the modal), and the whole-window drop overlay. |
| `store.js` | Store: the sidebar entry with its pending-count badge, the queue modal (add form, remove, Download now, live current-item bar, recent-download history), and the dock's Store section. Polls `/api/store/status` every 2 s while anything is live or the modal is open, every 60 s otherwise, and refetches the catalog when a completed download has changed it so new cards appear without a reload. |
| `manage.js` | Detail drawer: metadata display, Favorite, Download, Rename, Metadata edit, delete with an explicit second-click confirm, and the License (RAP) section (attach/change/remove). |

Layout is direction "B — modern dashboard": sidebar (Library: All/Favorites/
Recently Viewed; Platforms: PSP/PS3; Folders: the catalog's own categories
with counts), toolbar (search, sort, ＋ Add Games), responsive card grid,
and a right-side detail drawer (bottom sheet on mobile). Cover Flow lives on
the PSP/PS3 apps only; the web shell replaced it with the grid.

## JSON endpoints

| Endpoint | Method | Notes |
|---|---|---|
| `/api/catalog` | GET | `{"items": [...], "categories": [...]}`. Tolerates the `items` / `entries` / `games` key spellings in the file; `items` is the key the shell reads. `categories` is passed through only when the catalogue declares one. |
| `/api/upload` | POST | `multipart/form-data` with a `platform` field (`psp`/`ps3`) and one or more `file` parts. Reads the whole body into memory, so it suits curl and small files; the shell does not use it. Writes into the library root (atomic `.part` + rename), rejects extensions outside the game set, then refreshes `catalog.json` in whatever top-level shape it already had. |
| `/api/upload/file` | POST | The shell's Add Games path: one file per request, raw request body (no multipart), `?name=<url-encoded>&platform=<psp|ps3>&dir=<optional>`. The body is streamed to `<name>.part` in 256 KiB chunks and renamed in atomically, so uploads of ISO-sized files never buffer in memory. `dir` names an existing folder inside the library (default: the root) — it is how a license `.rap` lands beside its game; an escaping or missing `dir` is refused with `400` before the body is read. Chunked bodies get `411`. |
| `/api/item?url=…` | DELETE | Removes one library file plus its `.meta.json`/`.sha256` sidecars and its catalog entry. Directories get `405`; `catalog.json`, dotfiles, in-flight `.part` files, and bare sidecars get `404`; a url resolving outside the library gets `404`. |
| `/api/item/rename` | POST | JSON `{"url":…, "name":…}`. A rename is a rename, not a move: the file stays in its folder, sidecars come with it, the catalog entry is re-keyed. Renaming a referenced `.rap` also rewrites the sidecars and catalog entries beside it that point at the old name. Same name/extension guards as uploads; an existing name in the folder is `409`. |
| `/api/item/meta` | PUT | JSON `{"url":…, "title"?, "version"?, "description"?, "platform"?, "rap"?}`; `null` or `""` clears a field, `platform` is `psp`/`ps3` only, strings cap at 500 characters. `rap` names a license file and is validated specially (see below). Merged into the game's `<name>.meta.json` (existing keys preserved; an emptied sidecar is deleted) and into the catalog entry. |
| `/api/store/queue` | GET | `{"queue": [pending items], "running": [claimed items] | null}`. The pending list is `.store-queue.json`; `running` is the fetcher's live claim (`.store-queue.json.processing`) when one exists. |
| `/api/store/queue` | POST | JSON `{"url", "title"?, "category"?}`. `url` must be http(s) (≤2000 chars); `category` is one visible folder name (no path, no leading `.`, ≤64) matching what the fetcher itself enforces; `title` defaults to the URL's filename stem. Appends under a lock; a duplicate url or one already in the live claim is `409`, a bad url/category `400`. Answers `201` with the merged queue. |
| `/api/store/queue?url=…` | DELETE | Removes the pending item with that url, answering the merged queue; `409` while the url is in the live claim, `404` when not queued. |
| `/api/store/status` | GET | `.store-status.json` plus two derived fields: `processing` (a live claim exists — the only proof a download is really running, so a stale `"downloading"` from a crashed run reads back as `idle`) and `pending` (queue length). `current` carries the in-flight item's `bytes`/`total` (0.4 s throttle); `history` is newest-first, capped at 30. |
| `/api/store/run` | POST | Spawns `store_fetch.py` detached (the GUI's *Download now*, which must not wait on `STORE_INTERVAL`). `409` while a claim lives, `400` on an empty queue, `202` once started. Two overlapping spawns are harmless — only one rename can claim the queue. |

Both `/api/upload*` and every `/api/item*` mutation answer with the full
merged `items` array — the shell adopts it instead of re-fetching, and every
mutation keeps `<name>.meta.json` as the durable record, since
`generate_catalog.py` rebuilds `catalog.json` from disk.

Search and sort are client-side over the fetched catalog. Platform sections
filter on the sidecar `platform` field only — never a title regex, the same
lesson the device module learned (`ui/INTEGRATION.md`).

## Store queue & activity

The Store is server-side: `store_fetch.py` drains a queue file of CDN urls
into the library. The web GUI manages that queue and shows the download
activity; nothing about it is a client download — the *share* fetches, and
the fresh files simply appear in the catalog like uploads.

The queue file is the contract between the two processes, so the fetcher
never trims it in place. It *claims* the queue first — an atomic rename of
`.store-queue.json` to `.store-queue.json.processing` — which means an item
the GUI appends mid-run waits for the next pass instead of being deleted
unprocessed. While the claim lives, the fetcher publishes
`.store-status.json` (phase, a 0.4 s-throttled current-item bar, a
newest-first capped history) and, once drained, regenerates `catalog.json`
before releasing the claim — so a client that reloads after *Download now*
already lists the new titles. The GUI's polls race that regeneration
harmlessly and refetch only when the catalog's ids/sizes actually change.

Three rules the shell mirrors but never trusts: a `category` is exactly one
visible path component, a `url` is http(s), and the whole queue/status pair
is dot-prefixed so the library listing and catalog skip them and no device
ever sees them. The fetcher enforces the same rules itself — the queue is a
file an operator can hand-edit, and a `../` category wrote outside the
library before that guard existed.

## Licenses (.rap)

Released PS3 content needs a `.rap` license file the user supplies for games
they own; the share only stores what they put there. The pairing lives in the
game's sidecar as `rap` (a bare file name), which `generate_catalog.py`
passes through like `title`/`platform`, so it survives regeneration.

- **Attach** — the game's drawer shows a License (RAP) section for PS3-tagged
  items. A `.rap` already sitting in the game's folder is offered with a
  **Use** button (the usual Add Games case: game and license uploaded
  together). **Attach license** opens a `.rap`-filtered picker and uploads
  via `dir=<game folder>` so the license lands beside the game, then records
  the pairing. A `.rap` whose title-ID stem doesn't appear in the game's
  file name triggers a confirm nudge, not a block.
- **Server validation** — `rap` must be a bare `.rap` name of a file that
  exists in the game's own folder (co-location is what webMAN-style
  consumers look for); anything else is `400`. `null` detaches and removes
  the key from sidecar and catalog entry alike, leaving the `.rap` file in
  the library.
- **Renaming a license** rewrites the sidecars and catalog entries in the
  same folder that reference it, so attachments never dangle. Deleting one
  stays allowed (it is the user's file) — the game then reports **license
  file missing** in red instead of pretending.
- **Grid semantics** — an attached `.rap` is an accessory, not a card: it
  disappears from the grid (the game carries a 🔑 badge, red-tinted when the
  file is gone) and is reached from the game's drawer. An unattached `.rap`
  renders as a dashed **License** card so it stays discoverable. Sidebar
  counts match the filtered grid.

## Local state

Favorites, recently-viewed (wired: opening an item records it), theme, and
default sort all live in this browser's `localStorage`; nothing about
preferences touches the server. Settings → *Reset local data* clears them.

## Checks

`server/store/check_share.sh` covers the share this GUI depends on: listings,
`Range`/206/416, the `/gui/` routes, `/api/catalog`'s key, both upload paths
(streamed bytes land identical, rejections write nothing, chunked is refused),
the management trio (merge-not-replace metadata writes, sidecar ride-alongs,
rename collisions, and refusals for directories, sidecars, `catalog.json`, and
traversal), license pairing (`dir=` placement and refusals, attach validation,
license renames repointing references, detach, regeneration passthrough), the
Store endpoints (validation codes, the hidden queue file, a claimed-url delete
refused `409`, a stale status read back as idle, and a real `POST /api/store/run`
drain byte-matched against the served file and reaching `/api/catalog`), and
path traversal in general.
