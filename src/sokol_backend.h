/* Which sokol backend this translation unit targets.
 *
 * Included by BOTH src/game.h (declarations) and src/sokol_impl.c (the
 * implementation) before any sokol header, because the two must agree about the
 * API surface or they disagree about the declarations they share.
 *
 * Default: Metal on macOS, GL core everywhere else. `make BACKEND=gl` forces GL
 * on any platform -- that is how the GL path is verified on a Mac without a
 * Linux box, and it is the reason the GL code is testable at all.
 */
#ifndef SOKOL_BACKEND_H
#define SOKOL_BACKEND_H

#if defined(SOKOL_BACKEND_GL)
#define SOKOL_GLCORE
#define GAME_BACKEND_GL 1
#elif defined(__APPLE__)
#define SOKOL_METAL
#define GAME_BACKEND_METAL 1
#else
#define SOKOL_GLCORE
#define GAME_BACKEND_GL 1
#endif

/* Never touch the app's startup state (see the note in sokol_app.h). */
#define SAPP_DONT_ACTIVATE
#define SOKOL_DEBUG

#endif
