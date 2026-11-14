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
  compatible. The YM2413 (FM sound) emulation,
  `src/audio/emu2413/emu2413.c`, is Jarek Burczynski's `ym2413.c` with
  Eke-Eke's changes from Genesis Plus GX; the file states no licence of its
  own. The owner decided how it is declared (see the log, 2026-11-14,
  "The FM chip's terms").
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
  Its input test was seen to fail when the native reference held one of
  the two buttons the window held.
- 2026-11-14: from a fresh clone (submodule fetched at 40b8eeb), the build
  and `run-gate.sh` give 37 passed, 0 failed, 3 skipped (the engine test
  skips without CHIMERA_ROOT), and the package builds.
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

- **The boot ROMs** were not run: none is on this machine. The tests that
  run them are in `run-gate.sh` and say SKIP until `tests/firmware-local`
  holds `smsBios` or `ggBios`.
- **Windows and aarch64** were not run here; CI builds aarch64, and the
  package declares both CPUs. A movie recorded in the window and replayed in
  a new process on Windows is still to be done.
- **Registers can be read, not written**; no bus, no memory callbacks.

## 2026-11-14: The FM chip's terms (user-decided, 2026-11-14)

The owner's decision: the FM chip stays, and the package is declared
non-commercial the way the gpgx core declares Genesis Plus GX's terms.
`waterbox/package-licenses.json` now says the YM2413 emulation is under
"Genesis Plus GX licence (non-commercial), with MAME parts", and its note
says, in gpgx's words, that this code "is free for NON-COMMERCIAL use only"
and that "the package as a whole therefore may not be redistributed
commercially". `LICENSE` says the same.

## 2026-11-14: The link cables, inside the core (user-decided, 2026-11-14)

The owner's words: "No need to change this at a chimera level. Each core
should be able to do the replication at the core level and expose this as
a single machine to Chimera." The gambatte core's Game Boy Link (system GBL)
was the model for the shape.

- **Two machines more**, values of the Machine setting: `ggLink`, system
  GGL ("Game Gear Link (two)", BizHawk's id for it), two Game Gears joined
  by the Gear-to-Gear cable; and `markIIILink`, system M3L ("Mark III Link
  (two)"), two Mark IIIs joined by the Joy-Joy cable, which plugs into the
  SK-1100 keyboard (only F-16 Fighting Falcon, Japan, used it). Gearsystem
  emulates both cables.
- **How the two run**: upstream's own scheduler for its libretro core's
  link, `LibretroLink` in `platforms/libretro/libretro_link.cpp`, compiled
  unchanged but for one patch (0003) that lets it plug in the Mark III cable
  as well as the Gear-to-Gear one. It runs the console that is behind for
  one instruction at a time; what one console puts on the wires reaches the
  other at the cycle it was put there. A frame is a fixed number of cycles
  (262 or 313 lines), so a link machine's frames do not end at a picture.
  Both consoles live in the one sandbox, so a savestate holds both and the
  cable between them; upstream's own link savestate code is not used.
- **What Chimera sees**: one machine. The picture is P1's beside P2's
  (320x144, or 512x192 for two Mark IIIs; the border setting does not apply);
  the sound is the two mixed, as upstream mixes them. The controls: one
  Power (both consoles restart and the cable is plugged in again, upstream's
  reset), then each console's: a Game Gear's pad and Start; a Mark III's
  Reset, Pause, pad and the four keyboard keys that game reads (1, 2, Space,
  Return). Memory domains are each console's, P2's named "P2 Main RAM" and
  so on. Save data: battery.sav and battery2.sav, and the slots `savedata`
  and `savedata2` to start from them.
- **Files**: a second cartridge slot, `cart_gg2` or `cart_sms2`, which may
  be left empty (see the entry below).
- **Settings**: shared by both consoles. A Game Gear Link runs its
  consoles natively (the cable is the native mode's) and refuses a
  cartridge that is not a Game Gear one; a Mark III Link is a Japanese
  console. A cartridge the cable does not work with is refused with
  `error.link_refused`.
- **The keyboard's reads are control reads** (patch 0002), so a frame in
  which a Mark III read only its keyboard is not a lag frame. A link
  machine's frame is a lag frame when neither console read its controls.

The test cartridges grew a link mode (`tests/make-roms.py` writes
`gslink.gg` and `gslink.sms`): the Game Gear one sends its checksum over the
serial port whenever the line is free and counts the bytes it receives; the
Mark III one puts two bits of its checksum on the cable's two lines through
the keyboard's PPI and counts the reads that saw the other console pull one
low. Each folds what it received into its checksum, so what one console's
pad does reaches the other's memory only through the cable.

Measured (jaffanator01): `run-gate.sh` 53 passed, 0 failed, 2 skipped (the
boot ROMs). For each link machine, 600 frames with every control pressed in
a fixed pattern: native == sandbox, the same with a save and a load before
every frame, turbo, Power; in 300 frames P1 and P2 each received over the
cable (Game Gear: 30 bytes each; Mark III: 143 reads each saw a line low)
while the same cartridge on one console alone received nothing; P2's button
held changes P1's memory; lag frames between 1 and 300 of 600 (14 on the
Mark III Link, whose consoles read on alternate frames of their own, 307 on
the Game Gear Link). A Master System cartridge in a Game Gear Link is
refused. Seen to fail: the cable left unplugged (the exchange and the
crossing tests of both machines, 4 failures), P2's controls taken from P1's
place on the wire (the crossing test of both, 2 failures), and the refusal
before it was written (it failed first, which is how it was found).
Chimera's contract tests: 25 passed. `run-frontend.sh`: 14 passed; in the
window each link machine's P1 RAM is the native reference's after 300
frames and shows bytes received over the cable, and both link controllers'
key bindings are the package's.

Games: no Gear-to-Gear game is in the owner's collection on jaffanator01 yet
(only Frogger, a prototype, and Nazo Puyo: Aruru no Ru, neither known to use
the cable), and no Mark III game at all. Each of the two was run on the Game
Gear Link with itself, 3600 frames with the controls pressed in a fixed
pattern (`run-local.sh --link`): native == sandbox == a save and load every
frame. Columns, Sonic 2's two-player mode and F-16 Fighting Falcon were not
tried: they are not on the machine. So no game has been played over the
cable here; only the test cartridges prove the cable.

## 2026-11-14: P2's cartridge may be left empty (user-decided, 2026-11-14)

So that the Gear cores behave alike (the gearboy core's Link machine does
this), a link project with no cartridge for P2 runs P1's on both consoles:
P2's console reads its own copy of the file, and keeps its own saved data
(`savedata2`). A project lists a file once, so before this the same game on
both consoles took a renamed copy; now it takes nothing. A P2 cartridge, when
given, is used as before. The slots `cart_gg2` and `cart_sms2` take 0 or 1
file, and their help says so in all 26 languages.

Measured: `run-gate.sh` 55 passed, 0 failed, 2 skipped. New test
`<machine>:p2-empty` on both link machines: with P2's slot empty, native ==
sandbox == a save and load before every frame, the machine is the one the
same file in both slots makes, and P2 still receives over the cable (Game
Gear 30 bytes, Mark III 143 reads, in 300 frames). Seen to fail with the
fallback taken out (both machines: `error.no_file`). The window test now
makes its link projects with P2's slot empty: 14 passed. Contract tests: 25
passed.
