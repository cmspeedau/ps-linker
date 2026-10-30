/*
 * PS Linker - Cover Flow renderer, PSP stdout binding
 *
 * ui/coverflow.c draws its frames with printf() onto stdout — that is the
 * PS3 TTY / host-test path. On the PSP plain stdout never reaches the
 * screen, so the shared renderer is compiled here with printf bound to
 * pspDebugScreenPrintf, exactly the way main.c binds its own text.
 * The host tests keep compiling ui/coverflow.c against the real stdout.
 */

#include <pspdebug.h>
#include <stdio.h>

#define printf pspDebugScreenPrintf
#include "../ui/coverflow.c"
