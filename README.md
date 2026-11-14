# chimera-core-gearsystem

[Gearsystem](https://github.com/drhelius/Gearsystem), Ignacio Sanchez's
emulator of the Sega Master System (and Mark III), the Game Gear and the
SG-1000 (and SG-1000 II and Othello Multivision), as a core for
[Chimera](https://github.com/ToolAssisted-run/chimera). It runs in Chimera's
miniBox sandbox, so a movie plays the same every time.

One package holds the three machines; a project picks one with its Machine
setting. The Master System also takes the Light Phaser, the Paddle Control
and the Sports Pad, and the 3D glasses' pictures; the FM sound chip is
there.

## Using it

Put `gearsystem.chimeraCore` in Chimera's cores folder (Tools > Core
Manager... shows it). Games are not included: you bring the cartridges.
Nothing else is needed. The consoles' boot ROMs are optional; to run one,
turn on "Run the boot ROM" in the project and give Chimera your dump of it.

## Building

`docs/BUILDING.md`. The tests run on test cartridges the repository writes
itself, so they need no file from anywhere else.

## Licence

The built core is passed on under the GPL-3.0-or-later, Gearsystem's
licence; this repository's own code is MIT. See `LICENSE` and
`waterbox/package-licenses.json`.
