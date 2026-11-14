# Building the Gearsystem core

This repository builds Gearsystem as a sandboxed guest (`core.wbx`) and
packages it as one file, `gearsystem.chimeraCore`, which Chimera loads as a
Master System, a Game Gear or an SG-1000. `.github/workflows/chimera.yml`
runs the same steps from a fresh clone on a public runner.

Two placeholders are used throughout:

- `<chimera>` is the absolute path of a Chimera checkout
  (https://github.com/ToolAssisted-run/chimera).
- `<minibox>` is `<chimera>/extern/chimera-common-minibox`, the miniBox
  submodule of that checkout: the sandbox host and the guest toolchain.

## Requirements

- Linux on x86_64 or aarch64 (CI: `ubuntu-24.04` and `ubuntu-24.04-arm`).
  A package is built on the CPU it is for, never cross-built.
- git, and:

  ```
  sudo apt-get install -y --no-install-recommends meson ninja-build build-essential python3
  ```

- For the frontend tests only: Chimera built with its window
  (`<chimera>/tools/ci-chimera.sh` installs Qt 6.12 and builds it).
- miniBox's C++ guest toolchain downloads the GCC source matching the
  installed gcc once, to build libstdc++ for the sandbox.

## Get the sources

```
git clone https://github.com/ToolAssisted-run/chimera-core-gearsystem.git
cd chimera-core-gearsystem
git submodule update --init
git -C <chimera> submodule update --init extern/chimera-common-minibox
```

`extern/Gearsystem` is upstream at a release tag, unchanged. `patches/` is
applied to its working tree at every meson configure
(`waterbox/apply-patches.sh`); `git status` then shows the submodule as
modified, which is expected.

## Build

```
# miniBox with its C++ guest toolchain
meson setup <minibox>/build/meson-cpp <minibox> -Dguest_cpp=true
meson compile -C <minibox>/build/meson-cpp

# the native reference (run-native) and the sandbox runner (run-wbx)
meson setup build/meson-native -Dminibox_dir=<minibox>
ninja -C build/meson-native

# the sandboxed core
sh waterbox/setup-guest.sh -m <minibox>
ninja -C build/meson-guest core.wbx

# the package, written to <chimera>/build/Cores/gearsystem.chimeraCore
./waterbox/build-package.sh -m <minibox> -r <chimera>
```

`NINJA_JOBS=<n>` before `build-package.sh` limits the build's jobs. A package
built by hand is stamped `<commit>+local` (`-dirty` when the tree has
changes; the applied patch is one). CI sets `CORE_VERSION` to the commit.

## Test

Rebuild both builds first: the test scripts build nothing.

```
ninja -C build/meson-native && ninja -C build/meson-guest core.wbx

# native == sandbox == a save and a load every frame, on all three machines
CHIMERA_ROOT=<chimera> ./waterbox/run-gate.sh

# the package in Chimera's window, run headless
./waterbox/tests/run-frontend.sh --chimera-root <chimera>

# Chimera's contract tests, on a folder holding only this package
mkdir -p /tmp/gs-pkg && cp <chimera>/build/Cores/gearsystem.chimeraCore /tmp/gs-pkg/
<chimera>/build/meson-linux/source/ui/test-packages /tmp/gs-pkg
```

- `run-gate.sh` writes its test cartridges with `tests/make-roms.py`. Without
  `CHIMERA_ROOT` (a Chimera build with this package installed) the
  `engine:input-polls` test says SKIP.
- The boot ROM tests say SKIP unless `tests/firmware-local/smsBios` or
  `tests/firmware-local/ggBios` is your console's boot ROM.
- `waterbox/tests/run-local.sh` runs every game in `tests/roms-local`
  (gitignored; copy your own cartridges there, never commit them) through
  the same comparison.

## Troubleshooting

- `arena exhausted` from miniBox: the memory layout in `waterbox.config`
  (`memoryLayoutMiB`) and `waterbox/run-wbx.c` must stay the same, and big
  allocations come from the last (mmap) number.
- A test that fails after a source change with the old numbers: one of the
  two builds is stale. Run both ninja commands.
- `extern/Gearsystem is empty`: `git submodule update --init`.
