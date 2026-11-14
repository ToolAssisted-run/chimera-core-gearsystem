#!/usr/bin/env python3
"""Writes the test cartridges the core's tests run: one for the Master
System, one for the Game Gear and one for the SG-1000.

They are small Z80 programs written here, so they are this repository's own
work under its licence and need no download (Chimera builds its test
programs from source, owner 2026-10-14). A tiny assembler below turns them
into bytes; nothing else is needed to build them.

What each one does, every frame (after the frame interrupt):
  - reads the controller ports on every second frame only, so half the
    frames are lag frames and the lag count can be checked;
  - turns what it read into movement: the background scrolls, a sprite
    moves, a colour of the palette and the sound's pitch change;
  - drives the TH lines of port 0x3F and reads the ports again (the Light
    Phaser, the Paddle and the Sports Pad answer there), and reads the
    H and V counters;
  - folds everything it read into a running checksum in RAM;
  - counts the Pause button's interrupts (NMI) in RAM.
The Master System one also plays a note on the YM2413 (FM) chip, enables
the cartridge's battery RAM (Sega mapper, 64 KiB cartridge) and writes its
frame counter there, and copies the first bytes of that RAM into the
console's RAM at power-on, so a save put back by a project shows.

Usage: make-roms.py <output folder>
  writes gstest.sms, gstest.gg and gstest.sg
"""
import os
import sys


class Asm:
    def __init__(self, size):
        self.rom = bytearray(b"\xff" * size)
        self.pc = 0
        self.labels = {}
        self.fix16 = []
        self.fix8 = []

    def org(self, a):
        self.pc = a

    def label(self, name):
        assert name not in self.labels, name
        self.labels[name] = self.pc

    def b(self, *bs):
        for x in bs:
            self.rom[self.pc] = x & 0xFF
            self.pc += 1

    def w(self, v):
        if isinstance(v, str):
            self.fix16.append((self.pc, v))
            v = 0
        self.b(v & 0xFF, v >> 8)

    def rel(self, op, target):
        self.b(op)
        self.fix8.append((self.pc, target))
        self.b(0)

    def data(self, bs):
        for x in bs:
            self.b(x)

    def link(self):
        for at, name in self.fix16:
            v = self.labels[name]
            self.rom[at] = v & 0xFF
            self.rom[at + 1] = v >> 8
        for at, name in self.fix8:
            d = self.labels[name] - (at + 1)
            assert -128 <= d <= 127, (name, d)
            self.rom[at] = d & 0xFF

    # the instructions the programs use
    def di(self): self.b(0xF3)
    def ei(self): self.b(0xFB)
    def im1(self): self.b(0xED, 0x56)
    def halt(self): self.b(0x76)
    def reti(self): self.b(0xED, 0x4D)
    def retn(self): self.b(0xED, 0x45)
    def ret(self): self.b(0xC9)
    def ld_sp(self, v): self.b(0x31); self.w(v)
    def ld_hl(self, v): self.b(0x21); self.w(v)
    def ld_de(self, v): self.b(0x11); self.w(v)
    def ld_bc(self, v): self.b(0x01); self.w(v)
    def ld_a(self, n): self.b(0x3E, n)
    def ld_b(self, n): self.b(0x06, n)
    def ld_c(self, n): self.b(0x0E, n)
    def ld_a_mem(self, a): self.b(0x3A); self.w(a)
    def ld_mem_a(self, a): self.b(0x32); self.w(a)
    def out(self, port): self.b(0xD3, port)
    def inp(self, port): self.b(0xDB, port)
    def otir(self): self.b(0xED, 0xB3)
    def outi(self): self.b(0xED, 0xA3)
    def inc_a(self): self.b(0x3C)
    def dec_a(self): self.b(0x3D)
    def inc_hl(self): self.b(0x23)
    def dec_bc(self): self.b(0x0B)
    def ld_a_hl(self): self.b(0x7E)
    def ld_hl_a(self): self.b(0x77)
    def ld_a_b(self): self.b(0x78)
    def ld_a_c(self): self.b(0x79)
    def ld_b_a(self): self.b(0x47)
    def ld_c_a(self): self.b(0x4F)
    def or_c(self): self.b(0xB1)
    def or_b(self): self.b(0xB0)
    def xor_b(self): self.b(0xA8)
    def add_a_b(self): self.b(0x80)
    def and_(self, n): self.b(0xE6, n)
    def or_(self, n): self.b(0xF6, n)
    def xor(self, n): self.b(0xEE, n)
    def add(self, n): self.b(0xC6, n)
    def cp(self, n): self.b(0xFE, n)
    def rlca(self): self.b(0x07)
    def rrca(self): self.b(0x0F)
    def bit_a(self, n): self.b(0xCB, 0x47 + 8 * n)
    def jp(self, t): self.b(0xC3); self.w(t)
    def jp_nz(self, t): self.b(0xC2); self.w(t)
    def jp_z(self, t): self.b(0xCA); self.w(t)
    def call(self, t): self.b(0xCD); self.w(t)
    def jr(self, t): self.rel(0x18, t)
    def jr_nz(self, t): self.rel(0x20, t)
    def jr_z(self, t): self.rel(0x28, t)
    def djnz(self, t): self.rel(0x10, t)
    def push_af(self): self.b(0xF5)
    def pop_af(self): self.b(0xF1)
    def push_bc(self): self.b(0xC5)
    def pop_bc(self): self.b(0xC1)
    def push_hl(self): self.b(0xE5)
    def pop_hl(self): self.b(0xE1)


VDP_DATA, VDP_CTRL = 0xBE, 0xBF


def vdp_reg(a, reg, value):
    a.ld_a(value); a.out(VDP_CTRL)
    a.ld_a(0x80 | reg); a.out(VDP_CTRL)


def vdp_addr(a, addr, code):
    """code 1 = write VRAM, 3 = write CRAM"""
    a.ld_a(addr & 0xFF); a.out(VDP_CTRL)
    a.ld_a(((addr >> 8) & 0x3F) | (code << 6)); a.out(VDP_CTRL)


def out_block(a, label, length):
    """out length bytes from label to the VDP data port (length <= 65535)"""
    a.ld_hl(label)
    a.ld_bc(length)
    loop = "blk%d" % a.pc
    a.label(loop)
    a.ld_a_hl(); a.out(VDP_DATA); a.inc_hl(); a.dec_bc()
    a.ld_a_b(); a.or_c(); a.jr_nz(loop)


def fold(a, ram_hash):
    """hash = rotl(hash) ^ a, keeping a"""
    a.ld_b_a()
    a.ld_a_mem(ram_hash); a.rlca(); a.xor_b(); a.ld_mem_a(ram_hash)
    a.ld_a_b()


def build(kind):
    sms, gg, sg = kind == "sms", kind == "gg", kind == "sg"
    size = 0x10000 if sms else 0x8000
    a = Asm(size)
    ram = 0xC000
    R = {name: ram + 0x100 + i for i, name in enumerate(
        ["frame", "hash", "p1", "p2", "start", "sx", "sy", "spx", "spy", "nmi",
         "hc", "vc", "th1", "th2", "th3", "th4", "fm", "save0", "save1"])}
    stack = 0xC3F0 if sg else 0xDFF0

    # reset
    a.org(0)
    a.di(); a.im1(); a.ld_sp(stack); a.jp("main")
    # the frame interrupt: acknowledge it
    a.org(0x38)
    a.push_af(); a.inp(VDP_CTRL); a.pop_af(); a.ei(); a.reti()
    # Pause (NMI): count it
    a.org(0x66)
    a.push_af(); a.ld_a_mem(R["nmi"]); a.inc_a(); a.ld_mem_a(R["nmi"]); a.pop_af(); a.retn()

    a.org(0x100)
    a.label("main")
    # clear the work RAM the program uses
    a.ld_hl(ram + 0x100); a.ld_b(0x40)
    a.label("clr"); a.ld_a(0); a.ld_hl_a(); a.inc_hl(); a.djnz("clr")
    if sms:
        # the cartridge's battery RAM (Sega mapper): bring back what a save
        # held, then keep it switched in at 0x8000
        a.ld_a(0x08); a.ld_mem_a(0xFFFC)
        a.ld_a_mem(0x8000); a.ld_mem_a(R["save0"])
        a.ld_a_mem(0x8001); a.ld_mem_a(R["save1"])

    # the VDP's registers
    if sg:
        regs = [(0, 0x00), (1, 0x80), (2, 0x0E), (3, 0x80), (4, 0x00), (5, 0x7E), (6, 0x00), (7, 0x14)]
    else:
        regs = [(0, 0x06), (1, 0x80), (2, 0xFF), (3, 0xFF), (4, 0xFF), (5, 0xFF), (6, 0xFB),
                (7, 0x00), (8, 0x00), (9, 0x00), (10, 0xFF)]
    for reg, v in regs:
        vdp_reg(a, reg, v)

    # clear all of VRAM
    vdp_addr(a, 0x0000, 1)
    a.ld_bc(0x4000)
    a.label("vclr"); a.ld_a(0); a.out(VDP_DATA); a.dec_bc(); a.ld_a_b(); a.or_c(); a.jr_nz("vclr")

    if sg:
        # patterns 0..3 at 0x0000, colours at 0x2000, names at 0x3800
        vdp_addr(a, 0x0000, 1); out_block(a, "tiles", 32)
        vdp_addr(a, 0x2000, 1); out_block(a, "colours", 32)
        vdp_addr(a, 0x3800, 1); out_block(a, "names", 768)
        # one sprite: pattern 1, colour 15, then the end marker
        vdp_addr(a, 0x3F00, 1); out_block(a, "sprites", 5)
        vdp_reg(a, 1, 0xE0)
    else:
        vdp_addr(a, 0x0000, 1); out_block(a, "tiles", 128)
        vdp_addr(a, 0x3800, 1); out_block(a, "names", 32 * 28 * 2)
        vdp_addr(a, 0x0000, 3); out_block(a, "palette", 64 if gg else 32)
        vdp_addr(a, 0x3F00, 1); out_block(a, "sat_y", 2)
        vdp_addr(a, 0x3F80, 1); out_block(a, "sat_xt", 2)
        vdp_reg(a, 1, 0xE0)
    if sms:
        # the FM chip: switch it on and set an instrument
        a.ld_a(0x01); a.out(0xF2)
        for reg, v in [(0x30, 0x30), (0x10, 0x80), (0x20, 0x16)]:
            a.ld_a(reg); a.out(0xF0); a.ld_a(v); a.out(0xF1)
    a.ei()

    a.label("loop")
    a.halt()
    # the frame counter
    a.ld_a_mem(R["frame"]); a.inc_a(); a.ld_mem_a(R["frame"])
    # the controls, every second frame
    a.ld_a_mem(R["frame"]); a.and_(1); a.jp_nz("noinput")
    a.inp(0xDC); a.ld_mem_a(R["p1"]); fold(a, R["hash"])
    a.inp(0xDD); a.ld_mem_a(R["p2"]); fold(a, R["hash"])
    if gg:
        a.inp(0x00); a.ld_mem_a(R["start"]); fold(a, R["hash"])
    if not gg and not sg:
        # TH of both ports as outputs, high then low, and read each time
        # (Paddle, Sports Pad and Light Phaser answer here), then back to
        # inputs
        a.ld_a(0xF5); a.out(0x3F)
        a.inp(0xDC); a.ld_mem_a(R["th1"]); fold(a, R["hash"])
        a.ld_a(0x55); a.out(0x3F)
        a.inp(0xDC); a.ld_mem_a(R["th2"]); fold(a, R["hash"])
        a.inp(0xDD); a.ld_mem_a(R["th3"]); fold(a, R["hash"])
        a.ld_a(0xFF); a.out(0x3F)
        a.inp(0xDC); a.ld_mem_a(R["th4"]); fold(a, R["hash"])
    a.label("noinput")
    # the counters
    a.inp(0x7E); a.ld_mem_a(R["vc"]); fold(a, R["hash"])
    a.inp(0x7F); a.ld_mem_a(R["hc"]); fold(a, R["hash"])

    # movement from player 1's pad (a bit is 0 while its button is down)
    a.ld_a_mem(R["p1"]); a.bit_a(2); a.jr_nz("nl")
    a.ld_a_mem(R["sx"]); a.dec_a(); a.ld_mem_a(R["sx"])
    a.label("nl")
    a.ld_a_mem(R["p1"]); a.bit_a(3); a.jr_nz("nr")
    a.ld_a_mem(R["sx"]); a.inc_a(); a.ld_mem_a(R["sx"])
    a.label("nr")
    a.ld_a_mem(R["p1"]); a.bit_a(0); a.jr_nz("nu")
    a.ld_a_mem(R["spy"]); a.dec_a(); a.ld_mem_a(R["spy"])
    a.label("nu")
    a.ld_a_mem(R["p1"]); a.bit_a(1); a.jr_nz("nd")
    a.ld_a_mem(R["spy"]); a.inc_a(); a.ld_mem_a(R["spy"])
    a.label("nd")
    a.ld_a_mem(R["p1"]); a.bit_a(4); a.jr_nz("n1")
    a.ld_a_mem(R["spx"]); a.add(3); a.ld_mem_a(R["spx"])
    a.label("n1")
    a.ld_a_mem(R["p2"] if not gg else R["p1"]); a.bit_a(0 if not gg else 5); a.jr_nz("n2")
    a.ld_a_mem(R["sy"]); a.inc_a(); a.ld_mem_a(R["sy"])
    a.label("n2")

    if sg:
        # the backdrop colour follows the frame counter and the pad
        a.ld_a_mem(R["frame"]); a.rrca(); a.rrca(); a.rrca(); a.and_(0x0F)
        a.ld_b_a(); a.ld_a_mem(R["p1"]); a.xor(0xFF); a.add_a_b(); a.and_(0x0F); a.or_(0x10)
        a.out(VDP_CTRL); a.ld_a(0x87); a.out(VDP_CTRL)
        # the sprite
        vdp_addr(a, 0x3F00, 1)
        a.ld_a_mem(R["spy"]); a.out(VDP_DATA)
        a.ld_a_mem(R["spx"]); a.out(VDP_DATA)
    else:
        # scroll
        a.ld_a_mem(R["sx"]); a.out(VDP_CTRL); a.ld_a(0x88); a.out(VDP_CTRL)
        a.ld_a_mem(R["sy"]); a.and_(0xDF); a.out(VDP_CTRL); a.ld_a(0x89); a.out(VDP_CTRL)
        # colour 0 follows the frame counter and the pad
        vdp_addr(a, 0x0000, 3)
        a.ld_a_mem(R["frame"]); a.rrca(); a.rrca(); a.ld_b_a()
        a.ld_a_mem(R["p1"]); a.xor(0xFF); a.add_a_b()
        a.out(VDP_DATA)
        if gg:
            a.ld_a_mem(R["frame"]); a.and_(0x0F); a.out(VDP_DATA)
        # the sprite
        vdp_addr(a, 0x3F00, 1); a.ld_a_mem(R["spy"]); a.out(VDP_DATA)
        vdp_addr(a, 0x3F80, 1); a.ld_a_mem(R["spx"]); a.out(VDP_DATA)

    # sound: tone 0's pitch from the counter and the pad, louder while
    # button 2 is up; the noise channel steps through its modes
    a.ld_a_mem(R["frame"]); a.and_(0x0F); a.or_(0x80); a.out(0x7F)
    a.ld_a_mem(R["p1"]); a.and_(0x3F); a.out(0x7F)
    a.ld_a_mem(R["p1"]); a.bit_a(5); a.ld_a(0x92); a.jr_nz("loud"); a.ld_a(0x9C)
    a.label("loud"); a.out(0x7F)
    a.ld_a_mem(R["frame"]); a.rrca(); a.rrca(); a.rrca(); a.and_(0x07); a.or_(0xE0); a.out(0x7F)
    a.ld_a(0xF6); a.out(0x7F)
    if sms:
        # FM: the note's pitch follows the frame counter; key it on and off
        a.ld_a(0x10); a.out(0xF0); a.ld_a_mem(R["frame"]); a.out(0xF1)
        a.ld_a(0x20); a.out(0xF0); a.ld_a_mem(R["frame"]); a.and_(0x10); a.or_(0x06); a.out(0xF1)
        a.inp(0xF2); a.ld_mem_a(R["fm"])
        # the battery RAM keeps the frame counter and the checksum
        a.ld_a_mem(R["frame"]); a.ld_mem_a(0x8000)
        a.ld_a_mem(R["hash"]); a.ld_mem_a(0x8001)
    a.jp("loop")

    # data
    if sg:
        a.label("tiles")
        a.data([0] * 8 + [0xAA, 0x55] * 4 + [0xFF] * 8 + [0xF0] * 8)
        a.label("colours")
        a.data([0x4B, 0x4E, 0x6A, 0xC3] * 8)
        a.label("names")
        a.data([((c ^ r) & 3) for r in range(24) for c in range(32)])
        a.label("sprites")
        a.data([80, 120, 1, 0x0F, 0xD0])
    else:
        a.label("tiles")
        t = []
        t += [0] * 32                                  # tile 0: colour 0
        for row in range(8):                           # tile 1: a checker of colour 1
            p = 0xAA if row & 1 else 0x55
            t += [p, 0, 0, 0]
        t += [0, 0xFF, 0, 0] * 8                       # tile 2: colour 2
        t += [0xF0, 0xF0, 0x0F, 0] * 8                 # tile 3: colours 3 and 4
        a.data(t)
        a.label("names")
        n = []
        for r in range(28):
            for c in range(32):
                n += [(c ^ r) & 3, 0x08 if (c + r) % 7 == 0 else 0x00]
        a.data(n)
        a.label("palette")
        if gg:
            cols = [0x000, 0xF00, 0x0F0, 0x00F, 0xFF0, 0x0FF, 0xF0F, 0xFFF] * 4
            pal = []
            for c in cols:
                pal += [c & 0xFF, c >> 8]
            a.data(pal)
        else:
            a.data([0x00, 0x03, 0x0C, 0x30, 0x0F, 0x3C, 0x33, 0x3F] * 4)
        a.label("sat_y")
        a.data([90, 0xD0])
        a.label("sat_xt")
        a.data([100, 1])

    a.link()
    rom = a.rom
    if not sg:
        # the "TMR SEGA" header at 0x7FF0, its checksum over 0x0000-0x7FEF
        hdr = 0x7FF0
        rom[hdr:hdr + 8] = b"TMR SEGA"
        rom[hdr + 8:hdr + 10] = b"\x00\x00"
        csum = sum(rom[0:0x7FF0]) & 0xFFFF
        rom[hdr + 10] = csum & 0xFF
        rom[hdr + 11] = csum >> 8
        rom[hdr + 12:hdr + 15] = b"\x00\x00\x00"
        region = 0x6 if gg else 0x4                    # Game Gear export, Master System export
        rom[hdr + 15] = (region << 4) | 0x0C           # 32 KiB checked
    return bytes(rom)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for kind, name in (("sms", "gstest.sms"), ("gg", "gstest.gg"), ("sg", "gstest.sg")):
        with open(os.path.join(out, name), "wb") as f:
            f.write(build(kind))


if __name__ == "__main__":
    main()
