#include "../store_logic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    char text[16], path[256], other[256];
    char categories[STORE_CATEGORY_LIMIT][STORE_CATEGORY_LEN] = {{0}};
    int category_count = 0;
    assert(store_category_add(categories, &category_count, "Games") == 1);
    assert(store_category_add(categories, &category_count, "Homebrew") == 1);
    assert(store_category_add(categories, &category_count, "Games") == 0);
    assert(store_category_add(categories, &category_count, "") == 0);
    assert(category_count == 2 && strcmp(categories[0], "Games") == 0 &&
           strcmp(categories[1], "Homebrew") == 0);
    for (int i = category_count; i < STORE_CATEGORY_LIMIT; i++) {
        char name[STORE_CATEGORY_LEN];
        snprintf(name, sizeof name, "Category%d", i);
        assert(store_category_add(categories, &category_count, name) == 1);
    }
    assert(store_category_add(categories, &category_count, "Overflow") == 0);
    assert(category_count == STORE_CATEGORY_LIMIT);

    /* --- size formatting (generated catalogs carry an integer `size`) ----- */
    store_format_size(0, text, sizeof text); assert(strcmp(text, "0 KB") == 0);
    store_format_size(1536, text, sizeof text); assert(strcmp(text, "2 KB") == 0);
    store_format_size(2U * 1024U * 1024U, text, sizeof text); assert(strcmp(text, "2 MB") == 0);
    store_format_size(5ULL * 1024ULL * 1024ULL * 1024ULL, text, sizeof text); assert(strcmp(text, "5 GB") == 0);

    /* --- EBOOT.PBP takes its folder from the URL, not the title ---------- *
     * generate_catalog.py titles every EBOOT.PBP "EBOOT" (the file stem), so
     * a title-derived folder put all homebrew in ms0:/PSP/GAME/EBOOT/ and the
     * second install resumed against the first one's bytes and died on 416. */
    assert(store_derive_path("/Games/Doom/EBOOT.PBP", "EBOOT", path, sizeof path) == 0);
    assert(strcmp(path, "ms0:/PSP/GAME/Doom/EBOOT.PBP") == 0);
    assert(store_derive_path("/Games/CaveStory/EBOOT.PBP", "EBOOT", other, sizeof other) == 0);
    assert(strcmp(other, "ms0:/PSP/GAME/CaveStory/EBOOT.PBP") == 0);
    assert(strcmp(path, other) != 0);
    /* absolute catalog URLs work too, and "//host" is not a folder name */
    assert(store_derive_path("http://192.168.1.50:8000/store/doom-psp/EBOOT.PBP",
                             "Doom PSP (Homebrew Port)", path, sizeof path) == 0);
    assert(strcmp(path, "ms0:/PSP/GAME/doom-psp/EBOOT.PBP") == 0);
    assert(store_derive_path("http://192.168.1.50:8000/EBOOT.PBP", "My Game",
                             path, sizeof path) == 0);
    assert(strcmp(path, "ms0:/PSP/GAME/My_Game/EBOOT.PBP") == 0);
    /* query string must not defeat the extension test or the folder split */
    assert(store_derive_path("/x/EBOOT.PBP?token=1", "ignored", path, sizeof path) == 0);
    assert(strcmp(path, "ms0:/PSP/GAME/x/EBOOT.PBP") == 0);
    /* with nothing above the file the title is used, and sanitised */
    assert(store_derive_path("/EBOOT.PBP", "../bad", path, sizeof path) == 0);
    assert(strcmp(path, "ms0:/PSP/GAME/bad/EBOOT.PBP") == 0);
    assert(store_derive_path("http://h/EBOOT.PBP", "!!!", path, sizeof path) == 0);
    assert(strcmp(path, "ms0:/PSP/GAME/Homebrew/EBOOT.PBP") == 0);

    /* --- images go to ms0:/ISO, whatever the title says ------------------ */
    assert(store_derive_path("/PSP/game.iso", "Ignored", path, sizeof path) == 0);
    assert(strcmp(path, "ms0:/ISO/game.iso") == 0);
    assert(store_derive_path("/g/GAME.ISO?t=1", "T", path, sizeof path) == 0);
    assert(strcmp(path, "ms0:/ISO/GAME.ISO") == 0);

    /* --- a PBP that is not EBOOT keeps the title folder ------------------ */
    assert(store_derive_path("/Utilities/clock_tool.PBP", "Clock Tool",
                             path, sizeof path) == 0);
    assert(strcmp(path, "ms0:/PSP/GAME/Clock_Tool/clock_tool.PBP") == 0);
    assert(store_derive_path("/x/a b.PBP", "T", path, sizeof path) == 0);
    assert(strcmp(path, "ms0:/PSP/GAME/T/a_b.PBP") == 0);

    /* --- anything else is not installable ------------------------------- */
    assert(store_derive_path("/x/game.elf", "Unsupported", path, sizeof path) == -1);
    assert(store_derive_path(NULL, "x", path, sizeof path) == -1);
    /* too long is a different failure from "not installable" */
    assert(store_derive_path("/x/EBOOT.PBP", "x", path, 4) == -2);

    puts("store logic: all assertions passed");
    return 0;
}
