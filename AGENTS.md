# AGENTS.md - Gearsystem core for Chimera

This repository builds upstream Gearsystem (Master System and Mark III, Game
Gear, SG-1000) as a sandboxed guest for Chimera, a frontend for
tool-assisted speedruns. It produces one file, `gearsystem.chimeraCore`.
`docs/BUILDING.md` is the full build; `docs/PLAN.md` is why it is shaped
this way. This file is the short operating guide.

## Layout

- `extern/Gearsystem/` - upstream, a shallow submodule at a release tag, unchanged.
- `patches/` - every change to upstream, applied at meson configure. Today one: the controller port reads are reported (lag frames).
- `meson.build` - one build description, two builds: native (`run-native`, `run-wbx`) and guest (`core.wbx`).
- `waterbox/cinterface.cpp` - the adapter: Gearsystem behind Chimera's guest interface, compiled the same into both builds.
- `waterbox/run-native.c`, `waterbox/run-wbx.c`, `waterbox/gate-harness.h` - the two runners and the loop they share.
- `waterbox/waterbox.config`, `file_slots.json`, `default_keybinds.json`, `package-licenses.json`, `languages/` - what the package declares.
- `waterbox/run-gate.sh` - the core test script. `waterbox/tests/` - `run-frontend.sh` (Chimera's window), `run-local.sh` (your own games) and their helpers.
- `tests/make-roms.py` - writes the test cartridges (one per machine) the tests run.
- `.github/workflows/chimera.yml` - CI: core tests, frontend tests, contract tests, publish.

## Commands, in order

```
git submodule update --init
MB=<chimera>/extern/chimera-common-minibox
meson setup $MB/build/meson-cpp $MB -Dguest_cpp=true && meson compile -C $MB/build/meson-cpp
meson setup build/meson-native -Dminibox_dir=$MB && ninja -C build/meson-native
sh waterbox/setup-guest.sh -m $MB && ninja -C build/meson-guest core.wbx
CHIMERA_ROOT=<chimera> ./waterbox/run-gate.sh          # ends "<n> passed, 0 failed"
./waterbox/build-package.sh -m $MB -r <chimera>
./waterbox/tests/run-frontend.sh --chimera-root <chimera>
```

## Rules of this repository

- Never commit inside `extern/Gearsystem`. A change to upstream is a numbered
  patch in `patches/`, a hook or a build option, never a deletion.
- Build only what upstream's libretro core builds: no desktop program, no
  ImGui, no MCP server, no network code.
- Determinism is the product: no host time, no host randomness, no threads.
  A new test is broken on purpose once and seen to fail (Chimera's
  `docs/gates.md`).
- Every text a person reads is a key of `waterbox/languages/<code>.json`, in
  all 26 languages, in the same commit.
- Never commit games, boot ROMs or firmware: they go in the gitignored
  `tests/roms-local` and `tests/firmware-local`.
- Do not write `version` or `versionDate` in `waterbox.config`;
  `build-package.sh` stamps them.
- Plain language and plain ASCII in every `.md`. Commits:
  `type(scope): what changed`, a short body with what was tested and the
  numbers, staged by explicit path.
