# Lunar Lander starring Eagle the Fat Cat
# SPDX-License-Identifier: GPL-3.0-or-later

CC      ?= cc
CFLAGS  := -std=c17 -O2 -Wall -Wextra
SDLLIBS := $(shell sdl2-config --cflags --libs)

# ---------------------------------------------------------------- install
# Per-user install by default, following the XDG / FHS layout under
# ~/.local. For a system-wide install: sudo make install PREFIX=/usr/local
# Packagers: DESTDIR is honoured, and skips the per-user steps.
PREFIX      ?= $(HOME)/.local
BINDIR      ?= $(PREFIX)/bin
DATAROOTDIR ?= $(PREFIX)/share
MANDIR      ?= $(DATAROOTDIR)/man
DOCDIR      ?= $(DATAROOTDIR)/doc/lander
APPDIR      ?= $(DATAROOTDIR)/applications
ICONDIR     ?= $(DATAROOTDIR)/icons/hicolor
INSTALL     ?= install
ICON_SIZES  := 48 64 128 256
DOC_ASSETS  := assets/frame.png assets/closeup.png assets/cover_art.png
# where the game keeps scores and ghosts (it resolves this itself at
# run time; the Makefile only needs it to migrate old scores)
SAVEDIR     := $(or $(XDG_DATA_HOME),$(HOME)/.local/share)/lander

lander: lander.c
	$(CC) $(CFLAGS) $< -o $@ $(SDLLIBS) -lm

# Build and run the test harnesses. They #include lander.c directly,
# so they always test exactly the shipping code. The cksum guard
# fails the run if the player's real scores file changed under it.
test: terrain_test landing_test physics_test gameplay_test hiscore_test
	./terrain_test
	./landing_test
	./physics_test
	./gameplay_test
	@before="$$(cksum '$(SAVEDIR)/hiscores.txt' 2>/dev/null)"; \
	 ./hiscore_test || exit 1; \
	 after="$$(cksum '$(SAVEDIR)/hiscores.txt' 2>/dev/null)"; \
	 if [ "$$before" != "$$after" ]; then \
	   echo "FAIL hiscore_test modified $(SAVEDIR)/hiscores.txt"; exit 1; fi

terrain_test: terrain_test.c lander.c
	$(CC) -std=c17 -O2 -Wall $< -o $@ $(SDLLIBS) -lm

landing_test: landing_test.c lander.c
	$(CC) -std=c17 -O2 -Wall $< -o $@ $(SDLLIBS) -lm

physics_test: physics_test.c lander.c
	$(CC) -std=c17 -O2 -Wall $< -o $@ $(SDLLIBS) -lm

gameplay_test: gameplay_test.c lander.c
	$(CC) -std=c17 -O2 -Wall $< -o $@ $(SDLLIBS) -lm

hiscore_test: hiscore_test.c lander.c
	$(CC) -std=c17 -O2 -Wall $< -o $@ $(SDLLIBS) -lm

# Installs: binary, man page (section 6, games), desktop entry, icons
# (hicolor theme), README/LICENSE/screenshots as docs. Uses only
# mkdir -p and install -m, so it works with BSD/macOS install too.
install: lander
	$(INSTALL) -d '$(DESTDIR)$(BINDIR)' '$(DESTDIR)$(MANDIR)/man6' \
	              '$(DESTDIR)$(APPDIR)' '$(DESTDIR)$(DOCDIR)/assets'
	$(INSTALL) -m 755 lander   '$(DESTDIR)$(BINDIR)/lander'
	$(INSTALL) -m 644 lander.6 '$(DESTDIR)$(MANDIR)/man6/lander.6'
	sed 's|@BINDIR@|$(BINDIR)|' lander.desktop.in > lander.desktop
	$(INSTALL) -m 644 lander.desktop '$(DESTDIR)$(APPDIR)/lander.desktop'
	@for n in $(ICON_SIZES); do \
	   $(INSTALL) -d "$(DESTDIR)$(ICONDIR)/$${n}x$${n}/apps" && \
	   $(INSTALL) -m 644 "assets/lander-$$n.png" \
	              "$(DESTDIR)$(ICONDIR)/$${n}x$${n}/apps/lander.png" || exit 1; \
	 done
	$(INSTALL) -m 644 README.md LICENSE '$(DESTDIR)$(DOCDIR)'
	$(INSTALL) -m 644 $(DOC_ASSETS) '$(DESTDIR)$(DOCDIR)/assets'
	@if [ -z '$(DESTDIR)' ]; then \
	   command -v update-desktop-database >/dev/null 2>&1 && \
	     update-desktop-database -q '$(APPDIR)' 2>/dev/null; \
	   command -v gtk-update-icon-cache >/dev/null 2>&1 && \
	     gtk-update-icon-cache -q -t '$(ICONDIR)' 2>/dev/null; \
	   if [ -f lander_hiscores.txt ] && [ ! -f '$(SAVEDIR)/hiscores.txt' ]; then \
	     mkdir -p '$(SAVEDIR)' && \
	     cp lander_hiscores.txt '$(SAVEDIR)/hiscores.txt' && \
	     echo "migrated ./lander_hiscores.txt -> $(SAVEDIR)/hiscores.txt"; \
	   fi; \
	   case ":$$PATH:" in *":$(BINDIR):"*) ;; \
	     *) echo "note: $(BINDIR) is not on your PATH";; esac; \
	 fi; true
	@echo "installed: $(BINDIR)/lander  (man 6 lander)"

# Removes what install put in place. Leaves your scores and ghosts in
# $(SAVEDIR); delete that directory yourself if you want them gone.
uninstall:
	rm -f '$(DESTDIR)$(BINDIR)/lander' \
	      '$(DESTDIR)$(MANDIR)/man6/lander.6' \
	      '$(DESTDIR)$(APPDIR)/lander.desktop'
	@for n in $(ICON_SIZES); do \
	   rm -f "$(DESTDIR)$(ICONDIR)/$${n}x$${n}/apps/lander.png"; done
	rm -rf '$(DESTDIR)$(DOCDIR)'
	@echo "scores and ghosts kept in $(SAVEDIR)"

# Requires the Emscripten SDK (source emsdk_env.sh first).
# Fixed 64 MB heap instead of ALLOW_MEMORY_GROWTH: growable memory is a
# resizable ArrayBuffer, which TextDecoder in current browsers rejects,
# crashing the JS glue at startup. This game's memory needs are tiny and
# static, so a fixed heap is the correct build anyway.
# -lidbfs.js: IndexedDB-backed FS so browser high scores persist.
# -std=gnu17 (not c17): EM_ASM is built on GNU statement expressions
# and refuses to compile in strict -std=c* modes. The source itself
# remains strict C17; the native target above still enforces it.
# MINIFY_HTML=0: Debian's packaged emscripten lacks html-minifier-terser,
# and minifying a 2 KB shell page saves nothing worth having.
# Output is index.html (itch.io requires that name); test locally with
#   python3 -m http.server   and open http://localhost:8000/index.html
wasm: lander.c shell.html
	emcc -std=gnu17 -O2 lander.c -o index.html \
	     --shell-file shell.html \
	     -s USE_SDL=2 -s INITIAL_MEMORY=67108864 -s MINIFY_HTML=0 \
	     -lidbfs.js

# Zip ready to upload to itch.io (kind of project: HTML)
itch: wasm
	zip -j lander-itch.zip index.html index.js index.wasm

# Build products only. Player data lives in $(SAVEDIR), never here.
clean:
	rm -f lander terrain_test landing_test physics_test gameplay_test \
	      hiscore_test hiscore_test_scores.txt \
	      gameplay_test_scores.txt gameplay_test_ghost.txt \
	      lander.desktop \
	      index.html index.js index.wasm lander-itch.zip

.PHONY: test install uninstall wasm itch clean
