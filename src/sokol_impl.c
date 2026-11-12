/* The one translation unit that compiles sokol's implementation.
 *
 * It was in main.c, which meant every edit to the game recompiled ~1.5s of
 * sokol headers and driver code. Here it is compiled once and cached by make;
 * only a change under libs/sokol or to the defines below rebuilds it.
 *
 * Any define that changes the sokol API surface (SOKOL_METAL, SOKOL_NO_ENTRY,
 * SOKOL_DEBUG) must be identical in src/main.c or the two TUs disagree about
 * the declarations they share.
 */
#define SOKOL_DEBUG
#define SOKOL_IMPL
/* never grab focus on launch: see the note in sokol_app.h */
#define SAPP_DONT_ACTIVATE
#define SOKOL_METAL
#define SOKOL_NO_ENTRY
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_time.h"
#include "sokol_glue.h"
