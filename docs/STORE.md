# PS Linker Store

A homebrew store browser built into the PSP app (PS3 port planned). It fetches a
`catalog.json` from the PS Linker server and lets you install items with one press.

**Legal:** the catalog must only reference legally distributable content —
homebrew, freeware, demos, or your own legally-obtained files. It does not and
will not pull commercial titles from Sony's PSN CDN.

## How it works

1. The app fetches `<NAS URL>/catalog.json` (the same HTTP share as the file browser).
2. The Store view shows category tabs and a paginated item list in the Media Go style.
3. Pressing **X** downloads the item to its `install_path` (e.g. `ms0:/PSP/GAME/DOOM/`),
   creating the directory if needed, with the standard progress bar.
4. Installed items appear in the XMB like any other homebrew.

## Controls

| Button | Action |
|---|---|
| Up / Down | Move selection |
| Left / Right | Switch category tab |
| Cross | Install selected item |
| Circle | Exit Store (back to file browser) |
| Triangle (in file browser) | Open the Store |

## Catalog format (`server/catalog.json`)

```json
{
  "schema": 1,
  "categories": ["Games", "Emulators", "Demos", "Utilities"],
  "items": [
    {
      "id": "example-doom-port",
      "title": "Doom PSP (Homebrew Port)",
      "category": "Games",
      "description": "Open-source Doom port. Requires a WAD you legally own.",
      "version": "1.0",
      "size_hint": "2 MB",
      "icon": "",
      "url": "http://192.168.1.50:8000/store/doom-psp/EBOOT.PBP",
      "install_path": "ms0:/PSP/GAME/DOOM/"
    }
  ]
}
```

- `url` and `install_path` are required; an item missing either is skipped.
- `icon` is reserved for future cover-art support (currently unused).
- The Store URL follows the NAS URL set in the app's Settings view.

## Adding it to your PSP build

In `psp/main.c`:

```c
#include "store.h"

/* in the browser's button handling, alongside the other branches: */
} else if (pressed & PSP_CTRL_TRIANGLE) {
    store_run(g_nas_base);   /* returns when the user exits the store */
}
```

And add `store.o` to the PSP objects in `psp/Makefile`.

## Seeding your catalog

The repo ships `server/catalog.json` with example entries pointing at
placeholder URLs on your own share. Replace them with real homebrew
downloads (official project pages are the best source) and drop the files
under your HTTP share, e.g. `/games/store/<name>/EBOOT.PBP`.
