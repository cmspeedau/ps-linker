# ps-linker Roadmap

## Done

- PSP / PS3 homebrew apps (client-side)
- Store feature pulling PSP titles from Sony's CDN
- Media Go-style web server UI (sidebar, cover grid, detail pane)
- Library sections: All Games / Manage, Local PSP, PS3 Games
- Game upload via `POST /api/upload` + drag-and-drop

## Next

### Cover Flow UI (server + apps)

- Rework the library view into a **Cover Flow**: 3D-style horizontally scrolling cover carousel, keyboard/gamepad navigation (left/right to flip, enter to select)
- **Menu bar across the top** with all categories:
  - All Games | PSP Games | PS3 Games | Recently Added | Favorites | Downloads | Settings
- Selected category filters the Cover Flow; current selection title/info shown below the carousel
- Keep existing Media Go sidebar/grid view available as an alternate "Library" view mode; Cover Flow becomes the default
- Reuse the same look and feel in the PSP/PS3 client apps

### Later

- Wire "Download to PSP" button to a real transfer endpoint
- ps3netsrv rewrite
- Cover art / metadata extraction from ISO/CSO files
- Sync/Download actions for NAS shares and connected devices
