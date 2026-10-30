# Cover Flow module — integration notes

`ui/coverflow.c` + `ui/coverflow.h` is the on-device counterpart of the web
GUI's Cover Flow (`server/store/ui/ui.js`), so the two share the same category
menu labels, the flow ⇄ grid toggle and the favourite semantics. It has no
device headers in it on purpose, so the same translation unit is compiled by
the host test and by both device toolchains.

Two things to know when porting further, both read off `ui.js`:

- The web GUI filters `PSP Games` / `PS3 Games` on `platform` **or** a
  `/PSP/i` regex over the title, and its `Downloads` / `Settings` entries are
  placeholders that match nothing. This module filters on the `category` field
  only (prefix match) — deliberately stricter, since a title regex misfiles
  things like "PSP Emulator Frontend".
- The web GUI's extra tabs (`Emulators`, `Demos`, `Utilities`) come from
  `item.category` in `catalog.json`. If the device view needs them, the
  category list has to become data-driven; `COVERFLOW_CAT_*` is a fixed enum
  today.

## What is verified (2026-10-23)

| Check | Command | Result |
|---|---|---|
| Host build, warnings as errors | `make -C ui test` (`-std=c11 -Wall -Wextra -Wpedantic -Werror`) | clean |
| Behaviour | same target runs the test binary | 197 assertions pass |
| Memory safety | `make -C ui test-asan` (ASan + UBSan, includes a 1×1…400×40 render sweep) | clean |
| PSP toolchain | `psp-gcc -c -O2 -G0 -Wall -Wextra` in `pspdev/pspdev` | compiles, no warnings |
| PS3 toolchain | `ppu-gcc -c -O2 -Wall -Wextra -std=gnu99 -mcpu=cell -mminimal-toc` in `ps3dev/ps3dev:submodules` | compiles, no warnings |

The PSP integration is in progress: `psp/store.c` feeds catalog items to the
renderer and `Triangle` opens it from the browser. The PSP build now includes
the shared object. The PS3 app remains unintegrated.

## API in one paragraph

`coverflow_init(state, items, n, screen_w, screen_h, has_colors)` takes
**writable** `items` (the favourite toggle updates `is_favorite` in place — a
`static const` table lives in `.rodata` and faults), allocates one
filtered-index buffer, and filters by category. Feed pad input with
`coverflow_handle_key(state, key, &action)`; it returns 1 only when something
actually moved (so an edge press does not repaint the screen) and reports
`SELECT` / `BACK` / `SETTINGS` through `action`. Draw with
`coverflow_render_default(state)`, or build your own frame and reuse
`coverflow_menu_row()`. `coverflow_current()` is the selected item;
`coverflow_shutdown()` releases the buffer.

## Fixing the draft before it could run

The first drop of this module did not work. Each item below was reproduced,
then fixed and covered by an assertion:

1. **Favourite toggle segfaulted.** `coverflow_state_t.items` was
   `const coverflow_item_t *` while `COVERFLOW_KEY_FAVORITE` wrote through a
   cast away of that const. The delivered smoke test pressed FAVORITE on a
   `static const` table and died with SIGSEGV in `coverflow_handle_key`
   (`nm` showed `r kItems`, i.e. `.rodata`). The field and `coverflow_init()`
   now take writable items, and the test's table is writable.
2. **Re-filtering kept the wrong cover.** `coverflow_recompute_filter()`
   compared the old *slot number* against new *item ids*. In `All Games`
   (identity filter) it looked right; select slot 2 of the 5-item `PS3` list
   ("Uncharted 1", item 9) and switch to `All Games` and it landed on item 2.
   The selected item id is now captured before the list is rebuilt.
3. **The centre cover was drawn chopped.** `render_flow()` looped over the side
   tile height (7) while the centre tile is 11 rows tall, so its lower half —
   bottom border included — was never printed. The loop now walks the centre
   height and blank-pads the shorter side tiles.
4. **The selected category could be invisible.** The menu row was clipped to
   `screen_w` bytes after writing, but ANSI escapes counted toward that width
   and the seven labels total 91 columns, so at 80 columns `Settings` was cut
   off entirely — reachable with the shoulder buttons, never shown. The row is
   now built by `coverflow_menu_row()`, which windows around the current
   category and marks the hidden ones with ASCII `<` / `>`.
5. **Grid view ignored `screen_h`.** `render_grid()` printed every item
   (`row` was set but unused — a `-Wall` warning), so the footer and the
   selection scrolled off screen on any library longer than a page. It now
   draws one screenful around the selection with a `[ page x / y ]` strip.
6. **Keys at the edge reported a change.** `LEFT` on the first cover returned
   1 ("state changed, redraw"), forcing a full repaint per stray press.
   Movement keys now return 0 when the selection did not move.
7. **Category matching read fixed offsets** (`category[0..2]`) instead of
   `strncmp`, and an out-of-range `state->category` indexed the label table
   out of bounds. Both guarded; the label table is only reachable through
   `coverflow_category_label()`.
8. **`-Wformat-truncation` on psp-gcc.** The `%*s%s%*s` centring in
   `cover_title()` cannot be bounded by that compiler. It builds the padded
   text by hand now; both device toolchains compile the file warning-free.

## Wiring it into an app

The renderer wants title / category / size / version / favourite per item.
The PSP Store now supplies these fields; the PS3 directory browser still lacks
catalog metadata:

- `psp/main.c` and `ps3/main.c` browse a directory listing into
  `FileEntry { name, url, is_dir }` — no size, no category, no favourites.
  Cover Flow over that data is a list of one-word titles; the store-style
  chrome would be decoration.
- `psp/store.c` supplies the Store catalog to Cover Flow; `docs/STORE.md`
  remains the legacy catalog-format overview.

**Decided: option A — the Store view.** Build `store.c` with Cover Flow as its
renderer, add `Triangle -> store_run(g_nas_base)` as `psp/store.h` already
documents, take categories/sizes from the catalog the server already generates,
and keep favourites/recent in a small file next to `settings.txt`. Option B
(Cover Flow as the browser) stays open as roadmap "Later": it needs
`server/store/http_server.py` to report size and category per entry, which
touches the verified download path.

Two things block A, both read off the code rather than assumed:

1. `store.c` parses `size_hint` and `install_path`; `generate_catalog.py` emits
   `size` (an integer) and **no** `install_path`, and `store_download(url,
   install_path)` needs a destination. Either format `size` on the device and
   derive the install path from platform/category, or have the generator emit
   both. Deriving on-device keeps the server thin, which is this project's rule.
2. `store.c` prints through `pspDebugScreenPrintf` (`#define printf` at the top
   of the file), so pass `has_colors = 0` — the debug screen is a text console
   and would show escape bytes. At 40 columns the flow tiles are 8+2+14+2+8,
   which fits (40 is in the render sweep the host test runs).

Either way the wiring is the same shape (PS3 names shown; on PSP the
equivalents are `PSP_CTRL_LEFT` … and `PSP_CTRL_LTRIGGER` / `PSP_CTRL_RTRIGGER`
for the shoulders):

```c
#include "../ui/coverflow.h"

/* 1. Fill a writable item array from whatever the view lists. */
static coverflow_item_t g_covers[MAX_FILES];
/* title / category / size_human / version / is_favorite / is_recent */

coverflow_state_t cf;
coverflow_init(&cf, g_covers, n, SCREEN_COLS, SCREEN_ROWS,
               1 /* PS3 TTY does ANSI; 0 on the PSP debug screen */);

/* 2. Map pad bits to abstract keys, then feed them.
   ps3/main.c's pad_buttons() currently reports only LEFT/DOWN/RIGHT/UP/START/
   SELECT/CROSS/CIRCLE, so L1/R1/TRIANGLE have to be added there before the
   category and favourite keys can be reached. */
coverflow_action_t action;
coverflow_key_t key = COVERFLOW_KEY_NONE;
unsigned b = pad_buttons(&pd);
if (b & PAD_BTN_LEFT)         key = COVERFLOW_KEY_LEFT;
else if (b & PAD_BTN_RIGHT)   key = COVERFLOW_KEY_RIGHT;
else if (b & PAD_BTN_UP)      key = COVERFLOW_KEY_UP;
else if (b & PAD_BTN_DOWN)    key = COVERFLOW_KEY_DOWN;
else if (b & PAD_BTN_L1)      key = COVERFLOW_KEY_SHOULDER_L;
else if (b & PAD_BTN_R1)      key = COVERFLOW_KEY_SHOULDER_R;
else if (b & PAD_BTN_SELECT)  key = COVERFLOW_KEY_TAB;
else if (b & PAD_BTN_TRIANGLE) key = COVERFLOW_KEY_FAVORITE;
else if (b & PAD_BTN_CROSS)   key = COVERFLOW_KEY_ENTER;
else if (b & PAD_BTN_CIRCLE)  key = COVERFLOW_KEY_BACK;

if (coverflow_handle_key(&cf, key, &action))
    coverflow_render_default(&cf);       /* only when something changed */

if (action == COVERFLOW_ACTION_SELECT) {
    const coverflow_item_t *pick = coverflow_current(&cf);
    /* start the download for pick */
} else if (action == COVERFLOW_ACTION_BACK) {
    coverflow_shutdown(&cf);             /* leave the view */
}
if (cf.favorite_toggle_pending) {
    /* persist favourites, then clear the flag */
    cf.favorite_toggle_pending = 0;
}
```

and add the object to the build:

- `psp/Makefile`: `OBJS = main.o resume.o ../ui/coverflow.o`
- `ps3/Makefile`: `SRC := main.c resume.c ../ui/coverflow.c`

`psp/main.c` has a `Style`/blit screen layer rather than an ANSI console, so on
PSP either render through it (use `has_colors = 0` plus the existing row
helpers) or keep the ANSI path on the debug screen. `ps3/main.c` prints to the
TTY, where the ANSI frames work directly.

The PSP Store integration passes `has_colors = 0` because its output goes to
the PSP debug screen, and `SCREEN_COLS = 60` because that is the 480-pixel
debug screen on an 8-pixel cell; the renderer grows its tiles to suit
(14 + 2 + 20 + 2 + 14). Run `make -C ui test` to check the shared module. The
PSP app has since been built with `psp-gcc` and driven in PPSSPPSDL
(`log.md`, 2026-10-30); real-hardware testing is still open.
