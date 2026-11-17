/* cinterface.cpp - Gearsystem behind Chimera's guest interface.
 *
 * Gearsystem emulates the Sega Master System (and the Mark III), the Game
 * Gear and the SG-1000 (and the SG-1000 II and Othello Multivision). This
 * file drives upstream's GearsystemCore the way upstream's own libretro core
 * does (platforms/libretro/libretro.cpp): the same order of setup, the same
 * calls for each control, the same reset. What is Chimera's instead:
 *
 * - the settings come from the mounted "settings" file, and the machine is
 *   the project's choice (the "system" setting), never guessed from a file;
 * - the cartridge, the boot ROMs and a save are mounted files read in Init;
 * - a frame is one RunToVBlank, its picture BGRA and its sound 44100 Hz
 *   stereo;
 * - a read of the controls is reported here (patches/0001 and 0002), for
 *   lag frames and for Lua's event.oninputpoll.
 *
 * Two of the machines are two consoles joined by a link cable, shown to
 * Chimera as one machine (the owner, 2026-11-14): two Game Gears with the
 * Gear-to-Gear cable, and two Mark IIIs with the Joy-Joy cable on their
 * SK-1100 keyboards. Both consoles live in this one sandbox and are run by
 * upstream's own scheduler for its libretro core's link (LibretroLink,
 * platforms/libretro/libretro_link.cpp): the console that is behind runs
 * the next instruction, and what one puts on the wires reaches the other at
 * the cycle it was put there. A frame is a fixed number of cycles; the two
 * pictures stand side by side and the two sounds are mixed.
 *
 * This file compiles the same for the sandbox (miniBox's emulibc) and for
 * the native reference (native-shim/emulibc.h), which is what makes the
 * comparison of the two builds mean something.
 */
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>

#include <emulibc.h>
#include <waterbox_settings.h>
#include <waterbox_slots.h>

#include "gearsystem.h"
#include "libretro_link.h"

/* this core's windows in Chimera's Emulator menu: miniBox's ready-made Export
 * Save Data and CPU Registers, built on its exports (Chimera has no tool of
 * its own for a core, Chimera issue 610) */
#define WBX_WINDOWS_IMPLEMENTATION
#include <waterbox_windows.h>

/* A text a person reads is a key of languages/en.json, each of its
 * arguments after a TEXT_ARG (Chimera docs/translations.md). */
#define TEXT_ARG "\x1f"

/* Upstream's Log() prints to stdout unless this is set; it was made for its
 * MCP server's stdio mode. The core keeps quiet: a frontend reads nothing
 * there, and a log line is a system call that changes nothing. */
bool g_mcp_stdio_mode = true;

/* ---- which machine ---- */
enum { SYS_SMS = 0, SYS_GG = 1, SYS_SG = 2, SYS_GGLINK = 3, SYS_M3LINK = 4 };
static int g_system;
static int g_count = 1; /* consoles: 2 on the link machines */

/* ---- the controls, as waterbox.config lists them for each machine ----
 * Master System: 0 Power, 1 Reset, 2 Pause, 3..8 P1 Up Down Left Right
 *                Button 1 Button 2, 9..14 P2 the same.
 *                Axes: 0 P1 Light Phaser X, 1 P1 Light Phaser Y, 2 P1 Paddle,
 *                3 P1 Sports Pad X, 4 P1 Sports Pad Y, 5 P2 Sports Pad X,
 *                6 P2 Sports Pad Y.
 * Game Gear:     0 Power, 1..6 P1 Up Down Left Right Button 1 Button 2,
 *                7 P1 Start.
 * SG-1000:       0 Power, 1 Pause, 2..7 P1 (as above), 8..13 P2.
 * Game Gear Link: 0 Power, 1..7 P1 (as a Game Gear), 8..14 P2 the same.
 * Mark III Link: 0 Power, then for each console (P1 at 1, P2 at 13): Reset,
 *                Pause, the pad's six, and the keyboard's 1, 2, Space and
 *                Return. */
#define MAX_BUTTONS 32
#define AXIS_COUNT 7
#define M3_BLOCK 12
static uint8_t g_buttons[MAX_BUTTONS];
static int32_t g_axes[AXIS_COUNT] = { 32768, 32768, 32768, 32768, 32768, 32768, 32768 };

enum { DEV_NONE = 0, DEV_PAD, DEV_PHASER, DEV_PADDLE, DEV_SPORTSPAD };
static int g_port[2];
static bool g_allowUpDown;
/* the libretro core's rule for opposite directions held together: the one
 * pressed first wins (state of the machine's controls, so in the savestate);
 * one pair for each pad: the two ports, or the two linked consoles */
static int8_t g_latchV[2], g_latchH[2];

static GearsystemCore *g_cores[2];
#define g_core (g_cores[0])
static Cartridge::ForceConfiguration g_config;       /* from the settings */
static Cartridge::ForceConfiguration g_configs[2];   /* each console's, its variant chosen */
static int g_inited;
static int g_inputRead;
static char g_loadError[1100];

/* the link machines: upstream's two instances and its scheduler */
static LibretroInstance g_inst[2];
static LibretroLink *g_link;

/* ---- picture and sound ---- */
#define FRAME_W 512 /* two Mark III pictures side by side */
static uint8_t g_frame[FRAME_W * GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN * 4];
static int g_vwidth = GS_RESOLUTION_SMS_WIDTH, g_vheight = GS_RESOLUTION_SMS_HEIGHT;
static s16 g_audio[8192];
static int g_nsamples;
/* Turbo is the frontend's choice for the moment, not the machine's: no
 * savestate carries it. Gearsystem draws every line into its own buffer
 * either way; turned off, only the conversion into g_frame is skipped. */
ECL_INVISIBLE static int g_render = 1;

/* Chimera's function to call at each read of the controls (chimera#344). It
 * is the same callback slot in every session and no savestate carries it. */
ECL_INVISIBLE static uintptr_t (*g_inputPoll)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t);

/* patches/0001 and 0002: Input::GetPortDC, GetPortDD and GetPort00, and a
 * read of the Mark III keyboard, call this */
void chimera_input_read(void)
{
	g_inputRead = 1;
	if (g_inputPoll)
		g_inputPoll(0, 0, 0, 0, 0, 0);
}

static bool linked(void) { return g_count == 2; }

static void set_error(const char *key, const char *arg)
{
	if (arg)
		snprintf(g_loadError, sizeof g_loadError, "%s" TEXT_ARG "%s", key, arg);
	else
		snprintf(g_loadError, sizeof g_loadError, "%s", key);
}

static bool setting_is(const char *key, const char *value, const char *dflt)
{
	char buf[64];
	snprintf(buf, sizeof buf, "%s", dflt);
	wbx_setting_str(key, buf, sizeof buf);
	return strcmp(buf, value) == 0;
}

static void setting_str(const char *key, char *out, int outsz, const char *dflt)
{
	snprintf(out, outsz, "%s", dflt);
	wbx_setting_str(key, out, outsz);
}

/* reads a whole mounted file; NULL when it is not there */
static uint8_t *read_file(const char *name, long *size)
{
	FILE *f = fopen(name, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = new uint8_t[n > 0 ? (size_t)n : 1];
	if (n < 0 || fread(buf, 1, (size_t)n, f) != (size_t)n)
	{
		fclose(f);
		delete[] buf;
		*size = -1;
		return NULL;
	}
	fclose(f);
	*size = n;
	return buf;
}

static int port_device(const char *key, bool allowGuns)
{
	char buf[32];
	setting_str(key, buf, sizeof buf, "pad");
	if (!strcmp(buf, "none")) return DEV_NONE;
	if (!strcmp(buf, "pad")) return DEV_PAD;
	if (!strcmp(buf, "sportsPad")) return DEV_SPORTSPAD;
	if (allowGuns && !strcmp(buf, "lightPhaser")) return DEV_PHASER;
	if (allowGuns && !strcmp(buf, "paddle")) return DEV_PADDLE;
	fprintf(stderr, "chimera-gearsystem: unknown %s '%s', using a pad\n", key, buf);
	return DEV_PAD;
}

static bool is_gg(void) { return g_system == SYS_GG || g_system == SYS_GGLINK; }

static void read_force_config(void)
{
	char buf[48];

	g_config.system = Cartridge::CartridgeUnknownSystem;

	setting_str("region", buf, sizeof buf, "auto");
	if (g_system == SYS_M3LINK)
		g_config.zone = Cartridge::CartridgeJapanSMS; /* the Mark III is Japan's */
	else if (!strcmp(buf, "japan"))
		g_config.zone = is_gg() ? Cartridge::CartridgeJapanGG : Cartridge::CartridgeJapanSMS;
	else if (!strcmp(buf, "export"))
		g_config.zone = is_gg() ? Cartridge::CartridgeExportGG : Cartridge::CartridgeExportSMS;
	else if (!strcmp(buf, "international") && is_gg())
		g_config.zone = Cartridge::CartridgeInternationalGG;
	else
		g_config.zone = Cartridge::CartridgeUnknownZone;

	setting_str("timing", buf, sizeof buf, "auto");
	if (!strcmp(buf, "ntsc"))
		g_config.region = Cartridge::CartridgeNTSC;
	else if (!strcmp(buf, "pal"))
		g_config.region = Cartridge::CartridgePAL;
	else
		g_config.region = Cartridge::CartridgeUnknownRegion;

	static const struct { const char *name; Cartridge::CartridgeTypes type; } mappers[] = {
		{ "rom", Cartridge::CartridgeRomOnlyMapper },
		{ "sega", Cartridge::CartridgeSegaMapper },
		{ "codemasters", Cartridge::CartridgeCodemastersMapper },
		{ "korean", Cartridge::CartridgeKoreanMapper },
		{ "sg1000", Cartridge::CartridgeSG1000Mapper },
		{ "msx", Cartridge::CartridgeMSXMapper },
		{ "janggun", Cartridge::CartridgeJanggunMapper },
		{ "korean2000xor1f", Cartridge::CartridgeKorean2000XOR1FMapper },
		{ "koreanMsx32kb2000", Cartridge::CartridgeKoreanMSX32KB2000Mapper },
		{ "koreanMsxSms8000", Cartridge::CartridgeKoreanMSXSMS8000Mapper },
		{ "koreanSms32kb2000", Cartridge::CartridgeKoreanSMS32KB2000Mapper },
		{ "koreanMsx8kb0300", Cartridge::CartridgeKoreanMSX8KB0300Mapper },
		{ "korean0000xorff", Cartridge::CartridgeKorean0000XORFFMapper },
		{ "koreanFfffHiCom", Cartridge::CartridgeKoreanFFFFHiComMapper },
		{ "koreanFffe", Cartridge::CartridgeKoreanFFFEMapper },
		{ "koreanBffc", Cartridge::CartridgeKoreanBFFCMapper },
		{ "koreanFff3Fffc", Cartridge::CartridgeKoreanFFF3FFFCMapper },
		{ "koreanMdFff5", Cartridge::CartridgeKoreanMDFFF5Mapper },
		{ "koreanMdFff0", Cartridge::CartridgeKoreanMDFFF0Mapper },
		{ "jumboDahjee", Cartridge::CartridgeJumboDahjeeMapper },
		{ "eeprom93c46", Cartridge::CartridgeEeprom93C46Mapper },
		{ "multi4PakAllAction", Cartridge::CartridgeMulti4PAKAllActionMapper },
		{ "iratahack", Cartridge::CartridgeIratahackMapper },
	};
	setting_str("mapper", buf, sizeof buf, "auto");
	g_config.type = Cartridge::CartridgeNotSupported; /* upstream's "auto" */
	for (size_t i = 0; i < sizeof mappers / sizeof mappers[0]; i++)
		if (!strcmp(buf, mappers[i].name))
			g_config.type = mappers[i].type;
}

/* The machine is the project's. What upstream would detect from the file
 * (its name and its header) only picks among the variants of that machine:
 * a Game Gear cartridge on the Game Gear runs as upstream finds it, and any
 * other cartridge on a Game Gear runs in its Master System mode, as with
 * the Master Gear converter. A Game Gear Link runs its consoles natively,
 * as the cable is the native mode's. */
static bool g_notForLink; /* set by choose_system: not a Game Gear cartridge */
static Cartridge::CartridgeSystem choose_system(const uint8_t *rom, long size, const char *name)
{
	char model[32];
	if (g_system == SYS_SMS || g_system == SYS_M3LINK)
		return Cartridge::CartridgeSMS;

	Cartridge probe;
	probe.Init();
	/* the flags are set from the name and the header even when upstream
	 * finds the header wanting */
	probe.LoadFromBuffer(rom, (int)size, name);
	bool gg = probe.IsGameGear();
	bool sg = probe.IsSG1000();

	if (is_gg())
	{
		g_notForLink = g_system == SYS_GGLINK && !gg;
		setting_str("ggModel", model, sizeof model, "auto");
		if (!strcmp(model, "2asic")) return Cartridge::CartridgeGG2ASIC;
		if (!strcmp(model, "1asic")) return Cartridge::CartridgeGG1ASIC;
		if (g_system == SYS_GGLINK)
			return probe.GetGameGearASIC() == 1 ? Cartridge::CartridgeGG1ASIC : Cartridge::CartridgeGG2ASIC;
		if (!strcmp(model, "2asicSmsMode")) return Cartridge::CartridgeGG2ASICSMSMode;
		if (!strcmp(model, "1asicSmsMode")) return Cartridge::CartridgeGG1ASICSMSMode;
		if (gg)
		{
			bool smsMode = probe.IsGameGearInSMSMode();
			if (probe.GetGameGearASIC() == 1)
				return smsMode ? Cartridge::CartridgeGG1ASICSMSMode : Cartridge::CartridgeGG1ASIC;
			return smsMode ? Cartridge::CartridgeGG2ASICSMSMode : Cartridge::CartridgeGG2ASIC;
		}
		return Cartridge::CartridgeGG2ASICSMSMode;
	}

	setting_str("sgModel", model, sizeof model, "auto");
	if (!strcmp(model, "sg1000")) return Cartridge::CartridgeSG1000;
	if (!strcmp(model, "sg1000ii")) return Cartridge::CartridgeSG1000II;
	if (sg && probe.IsSG1000II())
		return Cartridge::CartridgeSG1000II;
	return Cartridge::CartridgeSG1000;
}

static void apply_devices(void)
{
	bool sms = g_system == SYS_SMS;
	for (int i = 0; i < g_count; i++)
	{
		GearsystemCore *c = g_cores[i];
		c->EnablePhaser(sms && g_port[0] == DEV_PHASER);
		c->EnablePaddle(sms && g_port[0] == DEV_PADDLE);
		c->EnableSportsPad(Joypad_1, sms && g_port[0] == DEV_SPORTSPAD);
		c->EnableSportsPad(Joypad_2, sms && g_port[1] == DEV_SPORTSPAD);
		/* the crosshair would be drawn into the picture the phaser reads */
		c->EnablePhaserCrosshair(false, Video::LightPhaserCrosshairCross, Video::LightPhaserCrosshairWhite);
	}
}

/* The picture settings, the sound chip's switch and the volumes: set before
 * the cartridge loads, as the libretro core does. A linked console has no
 * border: the two pictures stand side by side. */
static void apply_machine_settings(GearsystemCore *core)
{
	char buf[32];
	Video *video = core->GetVideo();

	setting_str("overscan", buf, sizeof buf, "off");
	if (linked()) video->SetOverscan(Video::OverscanDisabled);
	else if (!strcmp(buf, "topBottom")) video->SetOverscan(Video::OverscanTopBottom);
	else if (!strcmp(buf, "full284")) video->SetOverscan(Video::OverscanFull284);
	else if (!strcmp(buf, "full320")) video->SetOverscan(Video::OverscanFull320);
	else video->SetOverscan(Video::OverscanDisabled);

	setting_str("hideLeftBar", buf, sizeof buf, "no");
	if (!strcmp(buf, "auto")) video->SetHideLeftBar(Video::HideLeftBarAuto);
	else if (!strcmp(buf, "always")) video->SetHideLeftBar(Video::HideLeftBarAlways);
	else video->SetHideLeftBar(Video::HideLeftBarNo);

	video->SetNoSpriteLimit(wbx_setting_bool("noSpriteLimit", 0) != 0);

	core->GetAudio()->DisableYM2413(setting_is("ym2413", "off", "auto"));
	long psg = wbx_setting_long("psgVolume", 100);
	long fm = wbx_setting_long("fmVolume", 100);
	if (psg < 0 || psg > 200) psg = 100;
	if (fm < 0 || fm > 200) fm = 100;
	core->GetAudio()->SetPSGVolume((float)psg / 100.0f);
	core->GetAudio()->SetFMVolume((float)fm / 100.0f);

	setting_str("glasses", buf, sizeof buf, "both");
	if (!strcmp(buf, "left")) core->SetGlassesConfig(GearsystemCore::GlassesLeftEye);
	else if (!strcmp(buf, "right")) core->SetGlassesConfig(GearsystemCore::GlassesRightEye);
	else core->SetGlassesConfig(GearsystemCore::GlassesBothEyes);

	g_allowUpDown = wbx_setting_bool("allowUpDown", 0) != 0;
}

/* a boot ROM the project chose to run; returns 0 and sets the error when
 * it is asked for and cannot be had */
static int load_bios(GearsystemCore *core)
{
	bool gg = is_gg();
	if (g_system == SYS_SG || !wbx_setting_bool("bios", 0))
		return 1;
	const char *id = gg ? "ggBios" : "smsBios";
	long size = 0;
	uint8_t *buf = read_file(id, &size);
	if (!buf || size <= 0)
	{
		delete[] buf;
		set_error("error.bios_not_read", id);
		return 0;
	}
	bool ok = core->GetMemory()->LoadBootromFromBuffer(buf, (int)size, gg);
	delete[] buf;
	if (!ok)
	{
		set_error("error.bios_not_read", id);
		return 0;
	}
	if (gg)
		core->GetMemory()->EnableBootromGG(true);
	else
		core->GetMemory()->EnableBootromSMS(true);
	return 1;
}

static const char *cart_slot(int console)
{
	switch (g_system)
	{
	case SYS_GG: return "cart_gg";
	case SYS_SG: return "cart_sg";
	case SYS_GGLINK: return console ? "cart_gg2" : "cart_gg";
	case SYS_M3LINK: return console ? "cart_sms2" : "cart_sms";
	default: return "cart_sms";
	}
}

/* the battery RAM a save file of the project puts back, before the machine
 * runs (docs/save-data.md in Chimera: names are identity) */
static int load_savedata(GearsystemCore *core, const char *slot)
{
	char name[512];
	if (wbx_slot_count(slot) <= 0 || wbx_slot_name(slot, 0, name, sizeof name) == NULL)
		return 1;
	long size = 0;
	uint8_t *buf = read_file(name, &size);
	if (!buf)
	{
		set_error("error.not_read", name);
		return 0;
	}
	MemoryRule *rule = core->GetMemory()->GetCurrentRule();
	std::string bytes((const char *)buf, (size_t)size);
	delete[] buf;
	std::istringstream stream(bytes);
	if (size <= 0 || !rule->LoadRam(stream, (s32)size))
	{
		set_error("error.save_refused", name);
		return 0;
	}
	return 1;
}

/* one console: its settings, boot ROM and cartridge */
static int load_console(int i)
{
	GearsystemCore *core = new GearsystemCore();
	g_cores[i] = core;
	/* a linked console draws into upstream's 16-bit instance buffer */
	core->Init(linked() ? GS_PIXEL_RGB565 : GS_PIXEL_BGRA8888);
	/* The game is a cartridge, in the cartridge slot: what a boot ROM finds
	 * when it looks. Upstream's desktop program says so (its Media Slot
	 * setting, Cartridge by default); left alone, Memory keeps it in the
	 * expansion slot, which a Game Gear does not have, and the Game Gear's
	 * boot ROM then hands over to nothing. */
	core->GetMemory()->SetMediaSlot(Memory::CartridgeSlot);
	apply_machine_settings(core);
	if (!load_bios(core))
		return 0;

	/* the cartridge: the project's slot for this console, else (one
	 * console only) the plain rom mount */
	char name[512];
	const char *slot = cart_slot(i);
	const char *file = i == 0 ? "rom" : NULL;
	if (wbx_slot_count(slot) > 0 && wbx_slot_name(slot, 0, name, sizeof name) != NULL)
		file = name;
	/* a link machine with no cartridge for P2: P2's console runs its own
	 * copy of P1's (as the Gearboy core's link does) */
	else if (i == 1 && wbx_slot_count(cart_slot(0)) > 0 && wbx_slot_name(cart_slot(0), 0, name, sizeof name) != NULL)
		file = name;
	if (!file)
	{
		set_error("error.no_file", NULL);
		return 0;
	}
	long size = 0;
	uint8_t *rom = read_file(file, &size);
	if (!rom)
	{
		set_error(size < 0 ? "error.not_read" : "error.no_file", size < 0 ? file : NULL);
		return 0;
	}
	Cartridge::ForceConfiguration config = g_config;
	config.system = choose_system(rom, size, file);
	g_configs[i] = config;
	bool loaded = size > 0 && size <= MAX_ROM_SIZE
		&& core->LoadROMFromBuffer(rom, (int)size, &config, file);
	delete[] rom;
	if (!loaded)
	{
		set_error("error.rejected", file);
		return 0;
	}
	/* Video::Reset keeps what was set; set it again all the same, so a
	 * later upstream that resets it on load cannot drop a setting */
	apply_machine_settings(core);

	if (linked())
	{
		GS_LinkCable_Protocol want = g_system == SYS_GGLINK ? LinkCableProtocolGearToGear : LinkCableProtocolMarkIII;
		bool palMatch = i == 0 || core->GetCartridge()->IsPAL() == g_cores[0]->GetCartridge()->IsPAL();
		if (g_notForLink || core->GetSupportedLinkCableProtocol() != want || !palMatch)
		{
			set_error("error.link_refused", file);
			return 0;
		}
	}
	return 1;
}

extern "C" {

ECL_EXPORT const char *GetLoadError(void)
{
	return g_loadError;
}

ECL_EXPORT int Init(void)
{
	g_loadError[0] = 0;

	char sys[32];
	setting_str("system", sys, sizeof sys, "sms");
	if (!strcmp(sys, "gg")) g_system = SYS_GG;
	else if (!strcmp(sys, "sg1000")) g_system = SYS_SG;
	else if (!strcmp(sys, "ggLink")) g_system = SYS_GGLINK;
	else if (!strcmp(sys, "markIIILink")) g_system = SYS_M3LINK;
	else g_system = SYS_SMS;
	g_count = (g_system == SYS_GGLINK || g_system == SYS_M3LINK) ? 2 : 1;

	read_force_config();
	for (int i = 0; i < g_count; i++)
		if (!load_console(i))
			return 0;

	if (g_system == SYS_SMS || g_system == SYS_SG)
	{
		g_port[0] = port_device("port1", g_system == SYS_SMS);
		g_port[1] = port_device("port2", false);
	}
	else
	{
		/* a Game Gear's own pad; a linked Mark III's first port */
		g_port[0] = DEV_PAD;
		g_port[1] = DEV_NONE;
	}
	apply_devices();

	if (!load_savedata(g_cores[0], "savedata"))
		return 0;
	if (linked() && !load_savedata(g_cores[1], "savedata2"))
		return 0;

	if (linked())
	{
		/* upstream's scheduler for two consoles, plugged in before the
		 * first instruction runs (the Mark III's keyboard and cable must be
		 * there at power-on) */
		for (int i = 0; i < 2; i++)
			g_inst[i].core = g_cores[i];
		g_link = new LibretroLink(g_inst, g_latchV, g_latchH);
		g_link->SetProtocol(g_system == SYS_GGLINK ? LinkCableProtocolGearToGear : LinkCableProtocolMarkIII);
		g_link->Reset();
		GS_RuntimeInfo a, b;
		g_cores[0]->GetRuntimeInfo(a);
		g_cores[1]->GetRuntimeInfo(b);
		g_vwidth = a.screen_width + b.screen_width;
		g_vheight = a.screen_height > b.screen_height ? a.screen_height : b.screen_height;
	}
	else
	{
		GS_RuntimeInfo info;
		g_core->GetRuntimeInfo(info);
		g_vwidth = info.screen_width;
		g_vheight = info.screen_height;
	}
	memset(g_frame, 0, sizeof g_frame);
	for (size_t i = 3; i < sizeof g_frame; i += 4)
		g_frame[i] = 0xFF;

	g_inited = 1;
	return 1;
}

ECL_EXPORT void SetButton(int32_t index, int32_t state)
{
	if (index >= 0 && index < MAX_BUTTONS)
		g_buttons[index] = state != 0;
}

ECL_EXPORT void SetAxis(int32_t index, int32_t value)
{
	if (index >= 0 && index < AXIS_COUNT)
		g_axes[index] = value;
}

} /* extern "C" */

static void key(GearsystemCore *core, GS_Joypads pad, GS_Keys k, bool down)
{
	if (down)
		core->KeyPressed(pad, k);
	else
		core->KeyReleased(pad, k);
}

/* one pad's six controls from the wire, opposite directions settled the
 * libretro core's way unless the project allows both; latch is the pad's
 * own pair of the latches */
static void pad(GearsystemCore *core, GS_Joypads p, int latch, int base)
{
	bool up = g_buttons[base + 0], down = g_buttons[base + 1];
	bool left = g_buttons[base + 2], right = g_buttons[base + 3];
	if (!g_allowUpDown)
	{
		if (up && down)
		{
			if (g_latchV[latch] == 0) g_latchV[latch] = 1;
			up = g_latchV[latch] > 0;
			down = !up;
		}
		else
			g_latchV[latch] = up ? 1 : down ? -1 : 0;
		if (left && right)
		{
			if (g_latchH[latch] == 0) g_latchH[latch] = 1;
			left = g_latchH[latch] > 0;
			right = !left;
		}
		else
			g_latchH[latch] = left ? 1 : right ? -1 : 0;
	}
	key(core, p, Key_Up, up);
	key(core, p, Key_Down, down);
	key(core, p, Key_Left, left);
	key(core, p, Key_Right, right);
	key(core, p, Key_1, g_buttons[base + 4]);
	key(core, p, Key_2, g_buttons[base + 5]);
}

/* an absolute axis, 0..65535 across the picture, to a place on it */
static int scale(int32_t v, int range)
{
	if (v < 0) v = 0;
	if (v > 65535) v = 65535;
	return (int)(((int64_t)v * range) >> 16);
}

/* a Sports Pad axis is a speed: 32768 still, either end 8 counts a frame */
static float speed(int32_t v)
{
	if (v < 0) v = 0;
	if (v > 65535) v = 65535;
	return (float)(v - 32768) / 4096.0f;
}

static void m3key(GearsystemCore *core, GS_MarkIII_Key k, bool down)
{
	if (down)
		core->MarkIIIKeyPressed(k);
	else
		core->MarkIIIKeyReleased(k);
}

static void apply_inputs(void)
{
	switch (g_system)
	{
	case SYS_SMS:
		g_core->SetReset(g_buttons[1]);
		key(g_core, Joypad_1, Key_Start, g_buttons[2]); /* the Pause button: an NMI as it goes down */
		if (g_port[0] != DEV_NONE) pad(g_core, Joypad_1, 0, 3);
		if (g_port[1] != DEV_NONE) pad(g_core, Joypad_2, 1, 9);
		if (g_port[0] == DEV_PHASER)
		{
			GS_RuntimeInfo info;
			g_core->GetRuntimeInfo(info);
			g_core->SetPhaser(scale(g_axes[0], info.screen_width), scale(g_axes[1], info.screen_height));
		}
		if (g_port[0] == DEV_PADDLE)
		{
			/* the knob's place: upstream only moves it by an amount, and
			 * stops it at either end, so take it to one end first */
			g_core->SetPaddle(-1000.0f);
			g_core->SetPaddle((float)scale(g_axes[2], 256));
		}
		if (g_port[0] == DEV_SPORTSPAD)
			g_core->MoveSportsPad(Joypad_1, speed(g_axes[3]), speed(g_axes[4]));
		if (g_port[1] == DEV_SPORTSPAD)
			g_core->MoveSportsPad(Joypad_2, speed(g_axes[5]), speed(g_axes[6]));
		break;
	case SYS_GG:
		pad(g_core, Joypad_1, 0, 1);
		key(g_core, Joypad_1, Key_Start, g_buttons[7]);
		break;
	case SYS_GGLINK:
		for (int i = 0; i < 2; i++)
		{
			pad(g_cores[i], Joypad_1, i, 1 + 7 * i);
			key(g_cores[i], Joypad_1, Key_Start, g_buttons[7 + 7 * i]);
		}
		break;
	case SYS_M3LINK:
		for (int i = 0; i < 2; i++)
		{
			const int b = 1 + M3_BLOCK * i;
			g_cores[i]->SetReset(g_buttons[b]);
			key(g_cores[i], Joypad_1, Key_Start, g_buttons[b + 1]);
			pad(g_cores[i], Joypad_1, i, b + 2);
			m3key(g_cores[i], MarkIIIKey1, g_buttons[b + 8]);
			m3key(g_cores[i], MarkIIIKey2, g_buttons[b + 9]);
			m3key(g_cores[i], MarkIIIKeySpace, g_buttons[b + 10]);
			m3key(g_cores[i], MarkIIIKeyReturn, g_buttons[b + 11]);
		}
		break;
	default:
		key(g_core, Joypad_1, Key_Start, g_buttons[1]); /* the Pause button */
		if (g_port[0] != DEV_NONE) pad(g_core, Joypad_1, 0, 2);
		if (g_port[1] != DEV_NONE) pad(g_core, Joypad_2, 1, 8);
		break;
	}
}

/* RGB565 (upstream's link instances) to opaque BGRA, each channel widened
 * by repeating its top bits */
static inline uint32_t bgra565(uint16_t c)
{
	uint32_t r = (c >> 11) & 0x1F, g = (c >> 5) & 0x3F, b = c & 0x1F;
	r = (r << 3) | (r >> 2);
	g = (g << 2) | (g >> 4);
	b = (b << 3) | (b >> 2);
	return b | (g << 8) | (r << 16) | 0xFF000000u;
}

/* the two linked consoles' pictures side by side, P1 on the left */
static void compose_link_picture(void)
{
	GS_RuntimeInfo info[2];
	int x = 0, h = 0;
	for (int i = 0; i < 2; i++)
	{
		g_cores[i]->GetRuntimeInfo(info[i]);
		if (info[i].screen_height > h) h = info[i].screen_height;
	}
	uint32_t *dst = (uint32_t *)g_frame;
	const int w = info[0].screen_width + info[1].screen_width;
	for (int i = 0; i < 2; i++)
	{
		const int cw = info[i].screen_width, ch = info[i].screen_height;
		const uint16_t *src = g_inst[i].frame_buffer;
		for (int y = 0; y < h; y++)
			for (int c = 0; c < cw; c++)
				dst[y * w + x + c] = y < ch ? bgra565(src[y * cw + c]) : 0xFF000000u;
		x += cw;
	}
	g_vwidth = w;
	g_vheight = h;
}

extern "C" {

ECL_EXPORT void FrameAdvance(uint64_t packed)
{
	(void)packed; /* the controls arrive through SetButton and SetAxis */
	if (!g_inited)
		return;

	/* Power, held, is the console switched off and on: the libretro core's
	 * reset, which keeps the battery RAM. The link machines restart both
	 * consoles and plug the cable in again, as that core does. */
	if (g_buttons[0])
	{
		for (int i = 0; i < g_count; i++)
			g_cores[i]->ResetROMPreservingRAM(&g_configs[i]);
		if (g_link)
			g_link->Reset();
		apply_devices();
	}

	apply_inputs();

	g_inputRead = 0;
	int count = 0;
	if (g_link)
	{
		g_link->RunFrame();
		const s16 *mix = g_link->Audio(2, &count);
		if (count > (int)(sizeof g_audio / sizeof g_audio[0])) count = (int)(sizeof g_audio / sizeof g_audio[0]);
		if (count > 0)
			memcpy(g_audio, mix, (size_t)count * sizeof(s16));
		if (g_render)
			compose_link_picture();
	}
	else
	{
		g_core->RunToVBlank(g_frame, g_audio, &count, NULL, g_render != 0);
		if (g_render)
		{
			GS_RuntimeInfo info;
			g_core->GetRuntimeInfo(info);
			g_vwidth = info.screen_width;
			g_vheight = info.screen_height;
		}
	}
	if (count < 0) count = 0;
	if (count > (int)(sizeof g_audio / sizeof g_audio[0])) count = (int)(sizeof g_audio / sizeof g_audio[0]);
	g_nsamples = count / 2;
}

/* Turbo: while off the core draws no picture and is otherwise the machine
 * it would have been (run-gate.sh's turbo test is the proof). */
ECL_EXPORT void SetRenderingEnabled(int on) { g_render = on != 0; }

ECL_EXPORT uint32_t *GetVideoBgra(void) { return (uint32_t *)g_frame; }
ECL_EXPORT int GetVideoWidth(void) { return g_vwidth; }
ECL_EXPORT int GetVideoHeight(void) { return g_vheight; }

ECL_EXPORT int16_t *GetAudio(void) { return g_audio; }
ECL_EXPORT int GetAudioSampleCount(void) { return g_nsamples; }

/* the frame rate: the master clock over 228 cycles a line, 262 or 313
 * lines a frame (upstream's GetRuntimeInfo, as a fraction) */
ECL_EXPORT int GetVsyncNumerator(void)
{
	Cartridge *c = g_core ? g_core->GetCartridge() : NULL;
	if (c && c->IsPAL())
		return c->IsSG1000() ? GS_MASTER_CLOCK_PAL_SG1000 : GS_MASTER_CLOCK_PAL;
	return GS_MASTER_CLOCK_NTSC;
}
ECL_EXPORT int GetVsyncDenominator(void)
{
	Cartridge *c = g_core ? g_core->GetCartridge() : NULL;
	return GS_CYCLES_PER_LINE * ((c && c->IsPAL()) ? GS_LINES_PER_FRAME_PAL : GS_LINES_PER_FRAME_NTSC);
}

ECL_EXPORT int InputWasRead(void) { return g_inputRead; }

ECL_EXPORT void SetInputPollCallback(uint64_t fn)
{
	g_inputPoll = (uintptr_t (*)(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t))(uintptr_t)fn;
}

/* ---- memory domains: each console's, the second's named "P2 ..." ---- */
static const char *const kDomainNames[2][5] = {
	{ "Main RAM", "VRAM", "CRAM", "Cartridge RAM", "ROM" },
	{ "P2 Main RAM", "P2 VRAM", "P2 CRAM", "P2 Cartridge RAM", "P2 ROM" },
};
static const char *memdom(int which, uint8_t **area, int64_t *size, int *writable)
{
	const int console = which / 5;
	if (console >= g_count)
		return NULL;
	GearsystemCore *core = g_cores[console];
	const char *name = kDomainNames[console][which % 5];
	*writable = 1;
	switch (which % 5)
	{
	case 0:
		/* the console's RAM: 8 KiB at 0xC000, 1 KiB on an SG-1000 */
		*area = core->GetMemory()->GetMemoryMap() + 0xC000;
		*size = core->GetCartridge()->IsSG1000() ? 0x400 : 0x2000;
		return name;
	case 1:
		*area = core->GetVideo()->GetVRAM();
		*size = 0x4000;
		return name;
	case 2:
	{
		Cartridge *c = core->GetCartridge();
		if (c->IsSG1000())
			return NULL; /* the TMS9918's colours are fixed */
		*area = core->GetVideo()->GetCRAM();
		*size = (c->IsGameGear() && !c->IsGameGearInSMSMode()) ? 0x40 : 0x20;
		return name;
	}
	case 3:
	{
		MemoryRule *rule = core->GetMemory()->GetCurrentRule();
		if (!rule || rule->GetRamBanks() == NULL || rule->GetRamSize() == 0)
			return NULL;
		*area = rule->GetRamBanks();
		*size = (int64_t)rule->GetRamSize();
		return name;
	}
	default:
		*area = core->GetCartridge()->GetROM();
		*size = core->GetCartridge()->GetROMSize();
		*writable = 0;
		return name;
	}
}

#define DOMAIN_KINDS 10
static int domain_slot(int i)
{
	int found = 0;
	for (int which = 0; which < DOMAIN_KINDS; which++)
	{
		uint8_t *area = NULL;
		int64_t size = 0;
		int w = 0;
		if (memdom(which, &area, &size, &w) != NULL)
		{
			if (found == i) return which;
			found++;
		}
	}
	return -1;
}

ECL_EXPORT int GetMemoryDomainCount(void)
{
	if (!g_inited) return 0;
	int count = 0;
	while (domain_slot(count) >= 0) count++;
	return count;
}
ECL_EXPORT const char *GetMemoryDomainName(int i)
{
	uint8_t *area; int64_t size; int w;
	int which = domain_slot(i);
	return which >= 0 ? memdom(which, &area, &size, &w) : NULL;
}
ECL_EXPORT uint8_t *GetMemoryDomainPtr(int i)
{
	uint8_t *area = NULL; int64_t size; int w;
	int which = domain_slot(i);
	return which >= 0 && memdom(which, &area, &size, &w) ? area : NULL;
}
ECL_EXPORT int64_t GetMemoryDomainSize(int i)
{
	uint8_t *area; int64_t size = 0; int w;
	int which = domain_slot(i);
	return which >= 0 && memdom(which, &area, &size, &w) ? size : 0;
}
ECL_EXPORT int GetMemoryDomainWritable(int i)
{
	uint8_t *area; int64_t size; int w = 0;
	int which = domain_slot(i);
	return which >= 0 && memdom(which, &area, &size, &w) ? w : 0;
}

/* ---- save data: each cartridge's battery RAM, once its game has used it:
 * battery.sav (P1's), battery2.sav (P2's on the link machines) ---- */
static MemoryRule *battery(int console)
{
	if (!g_inited || console >= g_count) return NULL;
	MemoryRule *rule = g_cores[console]->GetMemory()->GetCurrentRule();
	if (!rule || !rule->PersistedRAM() || rule->GetRamBanks() == NULL || rule->GetRamSize() == 0)
		return NULL;
	return rule;
}
static int savedata_console(int32_t i)
{
	int found = 0;
	for (int c = 0; c < 2; c++)
		if (battery(c) && found++ == i)
			return c;
	return -1;
}
ECL_EXPORT int32_t GetSaveDataFileCount(void)
{
	return (battery(0) ? 1 : 0) + (battery(1) ? 1 : 0);
}
ECL_EXPORT const char *GetSaveDataFileName(int32_t i)
{
	int c = savedata_console(i);
	return c < 0 ? NULL : c == 0 ? "battery.sav" : "battery2.sav";
}
ECL_EXPORT int64_t GetSaveDataFileSize(int32_t i)
{
	int c = savedata_console(i);
	return c < 0 ? 0 : (int64_t)battery(c)->GetRamSize();
}
ECL_EXPORT const uint8_t *GetSaveDataFileBuffer(int32_t i)
{
	int c = savedata_console(i);
	return c < 0 ? NULL : battery(c)->GetRamBanks();
}

/* ---- the Z80's registers (P1's) ---- */
static const char *const kRegNames[] = {
	"AF", "BC", "DE", "HL", "AF'", "BC'", "DE'", "HL'", "IX", "IY", "SP", "PC", "I", "R", "IM", "IFF1", "IFF2",
};
#define REG_COUNT ((int32_t)(sizeof kRegNames / sizeof kRegNames[0]))
ECL_EXPORT int32_t GetRegisterCount(void) { return g_inited ? REG_COUNT : 0; }
ECL_EXPORT const char *GetRegisterName(int32_t i) { return i >= 0 && i < REG_COUNT ? kRegNames[i] : NULL; }
ECL_EXPORT int32_t GetRegisterBits(int32_t i) { return i >= 0 && i < 12 ? 16 : (i < 14 ? 8 : (i == 14 ? 8 : 1)); }
ECL_EXPORT int64_t GetRegisterValue(int32_t i)
{
	if (!g_inited || i < 0 || i >= REG_COUNT) return 0;
	Processor::ProcessorState *s = g_core->GetProcessor()->GetState();
	SixteenBitRegister *pairs[] = { s->AF, s->BC, s->DE, s->HL, s->AF2, s->BC2, s->DE2, s->HL2, s->IX, s->IY, s->SP, s->PC };
	if (i < 12) return pairs[i]->GetValue();
	switch (i)
	{
	case 12: return *s->I;
	case 13: return *s->R;
	case 14: return *s->InterruptMode;
	case 15: return *s->IFF1 ? 1 : 0;
	default: return *s->IFF2 ? 1 : 0;
	}
}
ECL_EXPORT const char *GetRegisterRoles(void) { return "pc PC\nsp SP\n"; }

} /* extern "C" */
