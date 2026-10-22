/*
 * PS Linker - Store module (PSP)
 *
 * Fetches <NAS_BASE>/catalog.json from the PS Linker server and presents
 * a Media Go-style store browser: category tabs, a paginated list, and
 * one-press download-and-install to the install_path declared per item.
 *
 * LEGAL: the catalog is expected to reference only legally distributable
 * content (homebrew, freeware, demos, or the user's own purchases).
 *
 * Integration (psp/main.c):
 *   #include "store.h"
 *   ...
 *   // In the browser's button handling, add:
 *   } else if (pressed & PSP_CTRL_TRIANGLE) {
 *       store_run(g_nas_base);   // returns when the user exits the store
 *   }
 *   // Triangle opens the Store from the file browser; Select still opens
 *   // the Settings view.
 *
 * Build: add store.c to PSP OBJS in the Makefile.
 * The catalog URL is derived from the runtime NAS base URL, so a URL
 * changed in Settings is honored here too.
 */

#ifndef STORE_H
#define STORE_H

/**
 * Run the Store view.
 *
 * nas_base: current NAS base URL, e.g. "http://192.168.1.50:8000"
 *           (no trailing slash). The catalog is fetched from
 *           "<nas_base>/catalog.json".
 *
 * Returns 0 when the user exits the store normally, <0 on a
 * catalog-fetch failure (an error screen is shown either way).
 */
int store_run(const char *nas_base);

#endif /* STORE_H */
