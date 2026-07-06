# Lunar Lander starring Eagle the Fat Cat
# SPDX-License-Identifier: GPL-3.0-or-later

CC      ?= cc
CFLAGS  := -std=c17 -O2 -Wall -Wextra
SDLLIBS := $(shell sdl2-config --cflags --libs)

lander: lander.c
	$(CC) $(CFLAGS) $< -o $@ $(SDLLIBS) -lm

# Build and run both test harnesses. They #include lander.c directly,
# so they always test exactly the shipping code.
test: terrain_test hiscore_test
	./terrain_test
	./hiscore_test

terrain_test: terrain_test.c lander.c
	$(CC) -std=c17 -O2 -Wall $< -o $@ $(SDLLIBS) -lm

hiscore_test: hiscore_test.c lander.c
	$(CC) -std=c17 -O2 -Wall $< -o $@ $(SDLLIBS) -lm

# Requires the Emscripten SDK (source emsdk_env.sh first).
# Fixed 64 MB heap instead of ALLOW_MEMORY_GROWTH: growable memory is a
# resizable ArrayBuffer, which TextDecoder in current browsers rejects,
# crashing the JS glue at startup. This game's memory needs are tiny and
# static, so a fixed heap is the correct build anyway.
# -lidbfs.js: IndexedDB-backed FS so browser high scores persist.
# -std=gnu17 (not c17): EM_ASM is built on GNU statement expressions
# and refuses to compile in strict -std=c* modes. The source itself
# remains strict C17; the native target above still enforces it.
# Output is index.html (itch.io requires that name); test locally with
#   python3 -m http.server   and open http://localhost:8000/index.html
wasm: lander.c shell.html
	emcc -std=gnu17 -O2 lander.c -o index.html \
	     --shell-file shell.html \
	     -s USE_SDL=2 -s INITIAL_MEMORY=67108864 \
	     -lidbfs.js

# Zip ready to upload to itch.io (kind of project: HTML)
itch: wasm
	zip -j lander-itch.zip index.html index.js index.wasm

clean:
	rm -f lander terrain_test hiscore_test \
	      index.html index.js index.wasm lander-itch.zip \
	      lander_hiscores.txt

.PHONY: test wasm itch clean
