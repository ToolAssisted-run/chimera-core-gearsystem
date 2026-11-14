# chimera-core-gearsystem: the plan and the log

Gearsystem (Ignacio Sanchez, https://github.com/drhelius/Gearsystem) as a
Chimera core: the Sega Master System and Mark III, the Game Gear, and the
SG-1000 (with the SG-1000 II and the Othello Multivision), built for the
miniBox sandbox and packaged as `gearsystem.chimeraCore`. Requested in
ToolAssisted-run/chimera#581; the owner approved building it on 2026-11-14
with every machine the emulator supports.

This file answers Chimera's `docs/new-core.md` section 1 first, then records
each decision with what was measured. Dated entries, newest last.

## Feasibility (new-core.md section 1), 2026-11-14

- **A new core?** Yes. The Gearsystem emulator is its own program; the
  machines it emulates are also in the gpgx core, but this is another
  emulator, asked for by name.
- **Licence.** The pinned release, 3.9.24 (commit 40b8eeb, 2026-10-30), is
  under the GNU GPL version 3 or later (`extern/Gearsystem/LICENSE`, and the
  same notice heads every file of `src/`). A modified binary may be passed on
  with its source, which the package names (its `licenses/` folder and
  `build.json`). Files inside with their own terms: Shay Green's sound
  buffers (LGPL-2.1 or later and GPL-2.0 or later) and miniz (MIT), both
  compatible. **One open question for the owner:** the YM2413 (FM sound)
  emulation, `src/audio/emu2413/emu2413.c`, is Jarek Burczynski's
  `ym2413.c` with Eke-Eke's changes from Genesis Plus GX. The file states no
  licence. Genesis Plus GX ships it under its own non-commercial terms;
  MAME ships the original author's code under GPL-2.0 or later. The package
  is therefore marked "commercial use prohibited" in
  `waterbox/package-licenses.json` until the owner says otherwise. It does
  not stop the non-commercial distribution Chimera does.
- **One thread, one clock?** Yes. Gearsystem is single-threaded; a frame is
  `RunToVBlank`, which runs the Z80 until the video chip reaches its vertical
  blank. Time is counted in the machine's own cycles. The only host-time
  call in the emulator is the timestamp of upstream's own save-state header
  (`GearsystemCore::SaveState`), which this core never calls (Chimera saves
  the whole sandbox). No `rand`, no `std::random_device`, no threads.
- **Picture.** The video chip's own framebuffer, converted to BGRA by
  upstream (`GS_PIXEL_BGRA8888`). No GPU, no GL.
- **Recompiler?** None; the Z80 is interpreted.
- **What the user brings.** Nothing to play a cartridge. The boot ROMs are
  optional and the user's own: `smsBios` (any Master System boot ROM) and
  `ggBios` (the Game Gear's, 1 KiB), used only when the project's "Run the
  boot ROM" setting is on (the firmware entries are `requiredWhen` that
  setting). The source has no compiled-in boot ROM or BIOS: the arrays in
  `Video.h` are palettes, those in `Processor.cpp` opcode tables, and the
  table in `emu2413.c` is the YM2413 chip's built-in instrument set, which
  every OPLL emulator carries as part of the chip.
- **What CI can run.** Test cartridges this repository writes itself:
  `tests/make-roms.py` assembles a small Z80 program for each machine
  (Master System 64 KiB with the Sega mapper and battery RAM, Game Gear
  32 KiB, SG-1000 32 KiB). They are this repository's own work, built from
  source at test time; nothing is downloaded and no binary is committed.
  The gpgx core's way (other people's homebrew whose licence was not found)
  was not followed because the owner asks for a stated licence.
- **Size.** Small: 32 MiB of sbrk heap and 32 MiB of mmap heap, of which a
  cartridge (up to 8 MiB) is the largest part.

## Sources and their roles

- **extern/Gearsystem**, a shallow submodule at tag 3.9.24 (40b8eeb). Only
  what upstream's libretro core builds (its `Makefile.common`) is compiled:
  `src/`, `src/audio/` and miniz, without `libretro.cpp`,
  `libretro_link.cpp`, the desktop program, ImGui, the debugger windows or
  the MCP server.
- **platforms/libretro/libretro.cpp** is the model for the adapter: the order
  of setup (settings, boot ROMs, then the cartridge), the calls for each
  controller, the reset that keeps the battery RAM, and the rule for
  opposite directions held together.
- **patches/0001-chimera-report-controller-port-reads.patch**: the one change
  to upstream. `Input::GetPortDC`, `GetPortDD` and `GetPort00` (the reads of
  the controller ports) call `chimera_input_read()` when the build defines
  `CHIMERA_INPUT_HOOK`. That is the lag-frame signal and Lua's
  `event.oninputpoll`.

## Decisions

- **One package, three machines** (gpgx's shape): the `system` setting
  (`sms`, `gg`, `sg1000`) picks the machine; each machine declares its own
  system id (SMS, GG, SG), controller, picture size and cartridge slot. The
  machine comes from the project, never from a file name.
- **Variants of a machine are settings.** `ggModel`: automatic, early
  (two chips) or late (one chip), each natively or in the Master System
  mode. `sgModel`: automatic, SG-1000 or SG-1000 II (other colours). On
  "automatic" upstream's own detection (the file name, the header and its
  game list) picks the variant, but never another machine: on a Game Gear,
  a cartridge upstream does not take for a Game Gear one runs in the Master
  System mode, as with the Master Gear converter.
- **Every libretro option that shapes the machine is a setting**: region,
  refresh rate, mapper, boot ROM, the two controller ports, the FM chip,
  the 3D glasses' eye, the border (overscan), hiding the left column, no
  sprite limit, opposite directions, and the PSG and FM volumes. Left out:
  the aspect ratio (Chimera's), the light gun's crosshair (it is drawn into
  the picture the gun reads, so it would be part of the machine; off), the
  libretro touchscreen and sensitivity options (the axes below replace them),
  and the link cable (see "Not done").
- **The controls.** Master System: Power, Reset, Pause and two pads of six
  buttons; seven axes for the other controllers of port 1 (Light Phaser X
  and Y, Paddle, Sports Pad X and Y) and port 2's Sports Pad. A gun's
  trigger and the other devices' buttons are the pad buttons of that port,
  as on the hardware. Game Gear: Power, one pad and Start. SG-1000: Power,
  Pause and two pads. Axes are 0 to 65535 with 32768 the middle: the gun's
  place across the live picture, the paddle's knob position (0 to 255
  inside), and a Sports Pad's speed (32768 still, either end 8 counts a
  frame). TAStudio shows the buttons; the axes are in its Columns menu.
- **Power** held on a frame is upstream's `ResetROMPreservingRAM`, the
  libretro core's reset: the console switched off and on, battery RAM kept.
  **Reset** on a Master System is the console's reset button, a bit the game
  reads (upstream's `SetReset`), not a restart.
- **Upstream's debugger is not built** (`GS_DISABLE_DISASSEMBLER`). With it,
  `Memory::Init` alone asks for 64 MiB of pointers (one for each byte a
  cartridge may have), which ran the sandbox out of memory; it is the
  desktop program's tooling and the emulation does not read it.
- **Upstream's log** goes nowhere (`g_mcp_stdio_mode` set): it printed to
  stdout, which nothing reads.
- **Memory**: domains Main RAM (8 KiB, 1 KiB on an SG-1000), VRAM, CRAM (not
  on an SG-1000), Cartridge RAM (when the cartridge has it) and ROM; the
  Z80's registers; save data `battery.sav` (the battery RAM once the game
  has switched it on) and a `savedata` slot to start from one, refused when
  the mapper does not take it.
- **No bus export, no memory callbacks yet**: a read of the Z80's address
  space through upstream's mappers can have side effects on some mappers,
  and the debugger hooks that callbacks would use are not built. Left for
  later, as Chimera's tools work on the domains.

## The tests and what proves them

`waterbox/run-gate.sh` (core), `waterbox/tests/run-frontend.sh` (Chimera's
window), Chimera's contract tests, and `waterbox/tests/run-local.sh` (games
kept out of git).

What the synthetic cartridges do not stand in for (docs/gates.md, E): they
use one mapper each (Sega, ROM only, SG-1000), no Korean or Codemasters
mapper, no EEPROM, no line interrupts, no 224-line mode and no
sprite-collision or overflow timing. Real games are the test for those.

## Log

- 2026-11-14: repository made, upstream pinned at 3.9.24 (40b8eeb).
  Feasibility as above.
- 2026-11-14: the core builds natively and in the sandbox (check-wbx clean).
  `run-gate.sh`: 38 passed, 0 failed, 2 skipped (the two boot ROM tests:
  no boot ROM is on this machine). On each of the three machines, 600
  frames: native == sandbox on the picture, the sound, the lag count and
  every memory domain; the controls shape the machine; about half the
  frames are lag frames (307 of 600, the cartridge reads every second
  frame); turbo leaves the machine as it was; a save and a load before
  every frame change nothing; Power at frame 300 restarts. Nine settings
  each build the machine they name and not the default's; the Light
  Phaser, the Paddle, a Sports Pad on either port and empty ports each
  answer the cartridge's reads, survive a save and a load every frame and
  are not a plain pad's machine; save data goes out (battery.sav, 32768
  bytes) and back in, and a save of the wrong size is refused; a boot ROM
  asked for and missing stops the load. Through Chimera's engine
  (chimera-run): 858 reads of the controls in 300 frames reach Chimera, 157
  lag frames, RAM the same as without counting.
  Negative controls, each seen to fail and then put back: the controller
  read not reported (lag 600 of 600 on all three machines), turbo a no-op
  (the turbo run drew every frame), the timing setting ignored (PAL vsync
  unchanged), the paddle's name unknown (the plain pad's machine, the knob
  changes nothing), the save not loaded (the cartridge does not find it):
  10 failures. The savestate test was not seen to fail: nothing in the
  adapter could be broken to make a save and load lose state that would not
  also break the other tests first.
- 2026-11-14: the package. Chimera's contract tests: 25 passed. A project
  of each machine's test cartridge made with `chimera-run --new-project`
  runs 300 frames with `--project`. `run-frontend.sh` in the window
  (headless, Qt's offscreen platform, so software drawing): 8 passed. Main
  RAM after 300 frames is the native reference's on all three machines,
  buttons held in the window reach the machine, `port1=none` reaches it, and
  the package's key bindings are the window's for all three controllers.
- 2026-11-14: games from the owner's collection, copied into
  tests/roms-local on jaffanator01 (`run-local.sh`, 3600 frames each with
  the controls pressed in a fixed pseudo-random pattern): Prince of Persia
  (Master System, sha1 bf0cdb664129b5548083dc0dde5449ab5f1bcc3a), Frogger
  prototype (Game Gear, b917ac6fbd3e5d18c07246f1309d81943ee1dd47) and Nazo
  Puyo: Aruru no Ru (Game Gear, 20308b7389a9e860697a66510d4925d1af3b769a):
  native == sandbox == a save and a load every frame. Frogger and Nazo Puyo
  were seen in play on the last frame; Prince of Persia reaches its title
  and answers Button 1. No SG-1000 game was on the machine yet, so no real
  SG-1000 game was tried.

## Not done

- **The link cables** (Gear-to-Gear, Mark III Joy-Joy): they join two
  consoles. Upstream's libretro core runs two emulators in one process for
  it; a core could do the same, but it is a second machine in one project
  and was left out of this first version.
- **The boot ROMs** were not run: none is on this machine. The tests that
  run them are in `run-gate.sh` and say SKIP until `tests/firmware-local`
  holds `smsBios` or `ggBios`.
- **Windows and aarch64** were not run here; CI builds aarch64, and the
  package declares both CPUs. A movie recorded in the window and replayed in
  a new process on Windows is still to be done.
- **Registers can be read, not written**; no bus, no memory callbacks.
