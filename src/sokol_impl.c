/* The one translation unit that compiles sokol's implementation.
 *
 * It was in main.c, which meant every edit to the game recompiled ~1.5s of
 * sokol headers and driver code. Here it is compiled once and cached by make;
 * only a change under libs/sokol or to the defines below rebuilds it.
 *
 * Any define that changes the sokol API surface (the backend, SOKOL_NO_ENTRY,
 * SOKOL_DEBUG) must be identical in src/game.h -- which is why both take it
 * from src/sokol_backend.h and neither chooses it itself.
 */
#define SOKOL_IMPL
#include "sokol_backend.h"   /* backend + SOKOL_DEBUG + SAPP_DONT_ACTIVATE */
#define SOKOL_NO_ENTRY
#include "sokol_app.h"
#include "sokol_gfx.h"
#include "sokol_time.h"
#include "sokol_glue.h"
