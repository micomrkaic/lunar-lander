# Lunar Lander — starring Eagle the Fat Cat

A classic arcade lunar lander in a single file of C17 + SDL2, playable
natively or in the browser via WebAssembly. The pilot is Eagle: a very
round cat with excellent whiskers, blue eyes, and a rocket strapped to
his backside.

![gameplay](assets/frame.png)

Eagle himself, at 6x magnification:

![Eagle closeup](assets/closeup.png)

And as the box art insists he truly is:

![cover art](assets/cover_art.png)

*(A gorgeous box and 8-bit graphics — the best tradition of the 1980s.)*

## Play

- **LEFT / RIGHT** (or **A / D**) — rotate
- **UP** (or **W**) — thrust
- **P** — pause, **ESC** — end flight, **R** — next level / retry
- **SPACE** — start a run, **D** — start today's daily run

Land upright on a green pad: descent under 28, lateral drift under 18,
tilt under ~11 degrees. The HUD turns a reading red when it is outside
those limits, and the tilt gauge shows the legal range. Score is 50 x
the pad multiplier plus remaining fuel — narrower pads pay more.

- **Fuel is the run.** It carries from level to level. Each landing
  refuels 20 + 5 x the multiplier, so greedy pads pay twice. Eagle gets
  lighter as he burns fuel, so a near-empty tank is twitchier.
- **Nine lives, three to start.** A crash costs one and you retry the same
  moon with the fuel you started it with. A 4X-or-better landing earns
  one back.
- **Every level is harder:** rougher ground, stronger gravity, narrower
  pads. Wind from level 3 (arrow at the top of the screen), and from
  level 5 a floating 6X pad that swings side to side. To land on it you
  have to match its speed.
- **Close-up:** the camera zooms 2x when you are within 90 px of the
  surface; `ALT` shows the height of your feet above what is below you.
- **Daily moon (D):** everyone gets the same sequence of moons that day.
  Your best landing on each level is saved and replays as a ghost Eagle
  the next time you fly that level.

The top eight scores are remembered on the high-score table.

## Build

Native (needs SDL2 development headers, e.g. `apt install libsdl2-dev`):

```sh
make
./lander
```

Browser (needs the [Emscripten SDK](https://emscripten.org/docs/getting_started/downloads.html);
`source emsdk_env.sh` first):

```sh
make wasm
python3 -m http.server     # then open http://localhost:8000/index.html
```

Package for itch.io (produces `lander-itch.zip`; upload as an HTML
project and tick "This file will be played in the browser"):

```sh
make itch
```

## Tests

```sh
make test
```

Five harnesses `#include lander.c` directly, so they always exercise
the exact shipping code:

- `terrain_test` generates 16,000 terrains (2,000 seeds x 8 levels) and
  asserts every vertex is inside the window, every edge — including
  across the screen-wrap seam — respects that level's slope cap, the
  ground joins up seamlessly at the wrap, and every landing pad is flat
  under the same interpolation the collision code uses.
- `landing_test` drops Eagle gently onto every pad of those 16,000
  terrains — upright and at just under the legal tilt each way — through
  the real `update()` collision path, and requires a landing every time.
  It also checks each pad's width matches its multiplier (5X = 45 px up
  to 2X = 90 px) and leaves at least 8 px of slack beyond Eagle's
  stance at maximum tilt.
- `physics_test` checks that Eagle crashes on first hull contact when
  he comes down inverted or rolled (rather than sinking until his feet
  touch), that an upright landing still counts after any number of full
  rolls, that flying and rendering never draw from the gameplay RNG (so
  a seed fully determines the terrain), and that Eagle is drawn on both
  sides of the screen while straddling the wrap.
- `gameplay_test` covers the run mechanics. It checks the fixed
  timestep (the same flight at 30 and 144 fps), thrust rising as fuel
  burns, refuel and extra lives (capped at 9), retrying the same moon,
  daily seeds, wind bounds and smoothness, plank placement and riding,
  and the zoom.
  Its centrepiece is an autopilot that flies real levels through the
  same input path as the keyboard and must land at least 75% of moons on
  every level from 1 to 8, wind and plank included. Every landing it
  makes is saved as a daily ghost and replayed, from memory and from the
  ghost file, and the ghost must touch down at exactly the same spot.
- `hiscore_test` checks high-score ordering, the table cap, score
  qualification, the default pilot name, and a save/load round trip. It
  uses its own scratch file, and `make test` fails if your real
  `lander_hiscores.txt` changes during the run.

`make clean` removes build products only; it leaves your high scores
alone.

## Design notes

- **Physics**: fixed 120 Hz timestep, run from an accumulator, so
  behaviour doesn't depend on frame rate. Semi-implicit (symplectic)
  Euler; lunar-ish gravity, constant thrust force acting on a mass that
  falls as fuel burns, wind, horizontal screen wrap. Landing is judged
  at the feet; any other part of Eagle touching rock or the plank is a
  crash.
- **Determinism and ghosts**: one physics step is a pure function of
  (level seed, step number, input), with wind and plank motion computed
  from hashes, not drawn from an RNG. So a flight is just its input
  stream: 120 bytes a second, run-length encoded in `lander_ghost.txt`
  (IndexedDB in the browser). Ghosts are kept for the current day only.
- **Zoom**: the world is drawn into an off-screen texture, then the
  camera's window of it is scaled to the screen with nearest-neighbour
  filtering, so the close-up keeps the chunky vector look.
- **Terrain**: midpoint-displacement heightmap (2^k + 1 vertices),
  periodic so it wraps with the screen, slope-limited around the whole
  ring so cliffs stay flyable, with flat pads carved in and their edges
  blended back under the slope cap.
- **Randomness**: each level's seed is a hash of the run seed and the
  level number (the run seed is today's date for the daily moon). Two
  generators: one drives gameplay (terrain, plank, stars, spawn point)
  and nothing else; the other drives cosmetics (exhaust,
  sparks, flame flicker), so a seed reproduces the same level.
- **Sound**: synthesized live in the SDL audio callback — filtered
  noise for thrust, a decaying noise burst for the crash, a rising
  arpeggio for touchdown. No assets, no SDL_mixer.
- **Text**: a hand-rolled vector stroke font (A-Z, 0-9, a little
  punctuation), so there is no
  SDL_ttf dependency and the WASM build stays small.
- **High scores**: a plain text file natively; an IndexedDB-backed
  IDBFS mount in the browser, so scores persist across sessions
  (per browser — it is a local table, not a global leaderboard).
- **One file**: everything above is ~1,750 lines of C17 in `lander.c`.
  The only dependency is SDL2.

## Credits

- Programming and physics: the management
- Character design and art direction: the management's wife
- Starring: Eagle the Fat Cat
- In the tradition of: Atari's *Lunar Lander* (1979) and every 1980s
  game whose box art wrote checks the pixels gleefully declined to cash

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
