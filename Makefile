# Build for the terrain viewer (macOS / Metal only).
#
#   make            the game            -> build/game
#   make -j         same, objects in parallel
#   make test       unit tests, ASan+UBSan, and runs them
#   make debug      game with ASan+UBSan -> build/debug/game
#   make tsan       game with ThreadSanitizer -> build/tsan/game
#   make clean
#
# `./build.sh [game|test|debug]` is a thin wrapper over this, because that is
# what every note and agent remembers.
#
# src/sokol_impl.c is deliberately its own TU: it costs ~1.5s and only changes
# when libs/sokol does, so game edits rebuild in well under a second.

CC        = clang
# ccache (if present) makes unchanged TUs free -- which is most of them when
# switching branches or between per-agent worktrees with identical content.
ifneq ($(shell command -v ccache 2>/dev/null),)
CC        := ccache clang
endif
MODE     ?= game

ifeq ($(MODE),tsan)
# ThreadSanitizer over the streamer: the only cross-thread surface in the game
# is the chunk queue, and it is the thing most likely to break silently.
OPT       := -O1 -g -fsanitize=thread
LDEXTRA   := -fsanitize=thread
OBJDIR    := build/tsan
else ifeq ($(MODE),debug)
# -O1, not -O0: the horizon bake is O(n^2) and at -O0 the world takes minutes to
# stream in, which makes the sanitizer build useless for rendering.
OPT       := -O1 -fsanitize=address,undefined
LDEXTRA   := -fsanitize=address,undefined
OBJDIR    := build/debug
else
OPT       := -O2
LDEXTRA   :=
OBJDIR    := build
endif

CFLAGS    := -std=c99 -g -Wall -Wextra -Wshadow -Werror -Wno-deprecated-declarations \
             $(OPT) -I libs -I libs/sokol -I libs/stb -I src
OBJCFLAGS := -fno-objc-arc
FRAMEWORKS := -framework Cocoa -framework QuartzCore -framework Metal -framework AudioToolbox
LDLIBS    := -lm -lpthread

HEADERS   := $(wildcard libs/sokol/*.h src/*.h)

.PHONY: all game test debug clean
all: game

game: $(OBJDIR)/game

OBJS := $(OBJDIR)/main.o $(OBJDIR)/world.o $(OBJDIR)/render.o $(OBJDIR)/replay.o \
        $(OBJDIR)/state.o $(OBJDIR)/map.o $(OBJDIR)/sim.o $(OBJDIR)/sokol_impl.o

$(OBJDIR)/game: $(OBJS)
	@mkdir -p $(OBJDIR)
	$(CC) $^ -o $@ $(FRAMEWORKS) $(LDLIBS) $(LDEXTRA)

$(OBJDIR)/%.o: src/%.c $(HEADERS) Makefile
	@mkdir -p $(OBJDIR)
	$(CC) -x objective-c $(CFLAGS) $(OBJCFLAGS) -MMD -MP -c $< -o $@

-include $(OBJDIR)/*.d

# --- unit tests -------------------------------------------------------------
# -O0 + sanitizers on purpose: these are cheap and they are where UB shows up.
TESTCFLAGS := -std=c99 -g -O1 -Wall -Wextra -Werror -Wno-deprecated-declarations \
              -fsanitize=address,undefined -I libs -I src

build/math_test: tests/math_test.c $(HEADERS)
	@mkdir -p build
	$(CC) $(TESTCFLAGS) $< -o $@ -lm

build/terrain_test: tests/terrain_test.c $(HEADERS)
	@mkdir -p build
	$(CC) $(TESTCFLAGS) $< -o $@ -lm

test: build/math_test build/terrain_test
	./build/math_test
	./build/terrain_test

debug:
	@$(MAKE) --no-print-directory MODE=debug game

clean:
	rm -rf build
