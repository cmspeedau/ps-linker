/*
 * PS Linker - PS3 Store module, public entry point
 *
 * store_run() takes over the screen (pixel backend when gfx is up, the
 * ANSI Cover Flow frame with -DNASDL_TEXT_UI) and browses the catalog
 * until Circle. Input is polled through the caller's callback so the
 * pad, its edge detection and the sysutil quit flag stay in main.c.
 *
 * Returns 0 on a normal exit to the browser, <0 on catalog failure.
 */

#ifndef PS3_STORE_H
#define PS3_STORE_H

/* Same contract as main.c's get_pressed(): returns 0 and fills *pressed
   on an edge, -1 when nothing is pressed, <0 on a system quit request
   (which ends the Store view). */
typedef int (*store_input_cb)(unsigned *pressed);

int store_run(const char *nas_base, store_input_cb poll);

/* Pad bitmask bits shared by main.c and store.c (PSL1GHT's padData
   carries these as separate bitfields; both files map once). */
#define PAD_BTN_LEFT     (1u << 0)
#define PAD_BTN_DOWN     (1u << 1)
#define PAD_BTN_RIGHT    (1u << 2)
#define PAD_BTN_UP       (1u << 3)
#define PAD_BTN_START    (1u << 4)
#define PAD_BTN_SELECT   (1u << 5)
#define PAD_BTN_CROSS    (1u << 6)
#define PAD_BTN_CIRCLE   (1u << 7)
#define PAD_BTN_L1       (1u << 8)
#define PAD_BTN_R1       (1u << 9)
#define PAD_BTN_TRIANGLE (1u << 10)
#define PAD_BTN_SQUARE   (1u << 11)

#endif /* PS3_STORE_H */
