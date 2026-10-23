
## Implemented Media Go-style web GUI

The server now exposes a small, dependency-free browser shell from the same HTTP process. Run the existing store server command, then open `http://<host>:<port>/`.

- `server/store/ui/index.html` provides the application structure: menu/toolbar, library navigation, NAS Shares and PSP Device sections, artwork grid, selected-item detail pane, and transfer status bar.
- `server/store/ui/ui.css` implements the dark Sony Media Go-inspired palette, blue selection states, dense utility chrome, responsive layout, and cover-tile treatments.
- `server/store/ui/ui.js` loads `GET /api/catalog` and renders the server catalogue. The UI never treats placeholder artwork as live catalogue data; an empty or unavailable catalogue is explicitly shown as empty.
- `GET /api/catalog` reads the existing `catalog.json` from the served library directory and returns it as JSON. Download/sync controls are currently visual shell controls until a PSP transfer API is available.

This is intentionally stdlib-only and preserves the existing Range-enabled file-serving behavior.
