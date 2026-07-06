# Lunar Lander — native and WASM builds

CC      ?= cc
CFLAGS  := -std=c17 -O2 -Wall -Wextra
SDLLIBS := $(shell sdl2-config --cflags --libs)

lander: lander.c
	$(CC) $(CFLAGS) $< -o $@ $(SDLLIBS) -lm

# Requires the Emscripten SDK (source emsdk_env.sh first).
# Fixed 64 MB heap instead of ALLOW_MEMORY_GROWTH: growable memory is a
# resizable ArrayBuffer, which TextDecoder in current browsers rejects,
# crashing the JS glue at startup. This game's memory needs are tiny and
# static, so a fixed heap is the correct build anyway.
# -lidbfs.js: IndexedDB-backed FS so browser high scores persist.
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
	rm -f lander index.html index.js index.wasm lander-itch.zip

.PHONY: wasm itch clean
