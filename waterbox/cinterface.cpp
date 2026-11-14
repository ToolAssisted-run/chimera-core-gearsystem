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
 * - a read of the controller ports is reported here (patches/0001), for lag
 *   frames and for Lua's event.oninputpoll.
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

/* A text a person reads is a key of languages/en.json, each of its
 * arguments after a TEXT_ARG (Chimera docs/translations.md). */
#define TEXT_ARG "\x1f"

/* Upstream's Log() prints to stdout unless this is set; it was made for its
 * MCP server's stdio mode. The core keeps quiet: a frontend reads nothing
 * there, and a log line is a system call that changes nothing. */
bool g_mcp_stdio_mode = true;

/* ---- which machine ---- */
enum { SYS_SMS = 0, SYS_GG = 1, SYS_SG = 2 };
static int g_system;

/* ---- the controls, as waterbox.config lists them for each machine ----
 * Master System: 0 Power, 1 Reset, 2 Pause, 3..8 P1 Up Down Left Right
 *                Button 1 Button 2, 9..14 P2 the same.
 *                Axes: 0 P1 Light Phaser X, 1 P1 Light Phaser Y, 2 P1 Paddle,
 *                3 P1 Sports Pad X, 4 P1 Sports Pad Y, 5 P2 Sports Pad X,
 *                6 P2 Sports Pad Y.
 * Game Gear:     0 Power, 1..6 P1 Up Down Left Right Button 1 Button 2,
 *                7 P1 Start.
 * SG-1000:       0 Power, 1 Pause, 2..7 P1 (as above), 8..13 P2. */
#define MAX_BUTTONS 16
#define AXIS_COUNT 7
static uint8_t g_buttons[MAX_BUTTONS];
static int32_t g_axes[AXIS_COUNT] = { 32768, 32768, 32768, 32768, 32768, 32768, 32768 };

enum { DEV_NONE = 0, DEV_PAD, DEV_PHASER, DEV_PADDLE, DEV_SPORTSPAD };
static int g_port[2];
static bool g_allowUpDown;
/* the libretro core's rule for opposite directions held together: the one
 * pressed first wins (state of the machine's controls, so in the savestate) */
static int8_t g_latchV[2], g_latchH[2];

static GearsystemCore *g_core;
static Cartridge::ForceConfiguration g_config;
static int g_inited;
static int g_inputRead;
static char g_loadError[1100];

/* ---- picture and sound ---- */
static uint8_t g_frame[GS_RESOLUTION_MAX_WIDTH_WITH_OVERSCAN * GS_RESOLUTION_MAX_HEIGHT_WITH_OVERSCAN * 4];
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

/* patches/0001: Input::GetPortDC, GetPortDD and GetPort00 call this */
void chimera_input_read(void)
{
	g_inputRead = 1;
	if (g_inputPoll)
		g_inputPoll(0, 0, 0, 0, 0, 0);
}

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

static void read_force_config(void)
{
	char buf[48];

	g_config.system = Cartridge::CartridgeUnknownSystem;

	setting_str("region", buf, sizeof buf, "auto");
	if (!strcmp(buf, "japan"))
		g_config.zone = g_system == SYS_GG ? Cartridge::CartridgeJapanGG : Cartridge::CartridgeJapanSMS;
	else if (!strcmp(buf, "export"))
		g_config.zone = g_system == SYS_GG ? Cartridge::CartridgeExportGG : Cartridge::CartridgeExportSMS;
	else if (!strcmp(buf, "international") && g_system == SYS_GG)
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
 * the Master Gear converter. */
static Cartridge::CartridgeSystem choose_system(const uint8_t *rom, long size, const char *name)
{
	char model[32];
	if (g_system == SYS_SMS)
		return Cartridge::CartridgeSMS;

	Cartridge probe;
	probe.Init();
	/* the flags are set from the name and the header even when upstream
	 * finds the header wanting */
	probe.LoadFromBuffer(rom, (int)size, name);
	bool gg = probe.IsGameGear();
	bool sg = probe.IsSG1000();

	if (g_system == SYS_GG)
	{
		setting_str("ggModel", model, sizeof model, "auto");
		if (!strcmp(model, "2asic")) return Cartridge::CartridgeGG2ASIC;
		if (!strcmp(model, "1asic")) return Cartridge::CartridgeGG1ASIC;
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
	bool phaser = g_system == SYS_SMS && g_port[0] == DEV_PHASER;
	bool paddle = g_system == SYS_SMS && g_port[0] == DEV_PADDLE;
	g_core->EnablePhaser(phaser);
	g_core->EnablePaddle(paddle);
	g_core->EnableSportsPad(Joypad_1, g_system == SYS_SMS && g_port[0] == DEV_SPORTSPAD);
	g_core->EnableSportsPad(Joypad_2, g_system == SYS_SMS && g_port[1] == DEV_SPORTSPAD);
	/* the crosshair would be drawn into the picture the phaser reads */
	g_core->EnablePhaserCrosshair(false, Video::LightPhaserCrosshairCross, Video::LightPhaserCrosshairWhite);
}

/* The picture settings, the sound chip's switch and the volumes: set before
 * the cartridge loads, as the libretro core does. */
static void apply_machine_settings(void)
{
	char buf[32];
	Video *video = g_core->GetVideo();

	setting_str("overscan", buf, sizeof buf, "off");
	if (!strcmp(buf, "topBottom")) video->SetOverscan(Video::OverscanTopBottom);
	else if (!strcmp(buf, "full284")) video->SetOverscan(Video::OverscanFull284);
	else if (!strcmp(buf, "full320")) video->SetOverscan(Video::OverscanFull320);
	else video->SetOverscan(Video::OverscanDisabled);

	setting_str("hideLeftBar", buf, sizeof buf, "no");
	if (!strcmp(buf, "auto")) video->SetHideLeftBar(Video::HideLeftBarAuto);
	else if (!strcmp(buf, "always")) video->SetHideLeftBar(Video::HideLeftBarAlways);
	else video->SetHideLeftBar(Video::HideLeftBarNo);

	video->SetNoSpriteLimit(wbx_setting_bool("noSpriteLimit", 0) != 0);

	g_core->GetAudio()->DisableYM2413(setting_is("ym2413", "off", "auto"));
	long psg = wbx_setting_long("psgVolume", 100);
	long fm = wbx_setting_long("fmVolume", 100);
	if (psg < 0 || psg > 200) psg = 100;
	if (fm < 0 || fm > 200) fm = 100;
	g_core->GetAudio()->SetPSGVolume((float)psg / 100.0f);
	g_core->GetAudio()->SetFMVolume((float)fm / 100.0f);

	setting_str("glasses", buf, sizeof buf, "both");
	if (!strcmp(buf, "left")) g_core->SetGlassesConfig(GearsystemCore::GlassesLeftEye);
	else if (!strcmp(buf, "right")) g_core->SetGlassesConfig(GearsystemCore::GlassesRightEye);
	else g_core->SetGlassesConfig(GearsystemCore::GlassesBothEyes);

	g_allowUpDown = wbx_setting_bool("allowUpDown", 0) != 0;
}

/* a boot ROM the project chose to run; returns 0 and sets the error when
 * it is asked for and cannot be had */
static int load_bios(void)
{
	bool gg = g_system == SYS_GG;
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
	bool ok = g_core->GetMemory()->LoadBootromFromBuffer(buf, (int)size, gg);
	delete[] buf;
	if (!ok)
	{
		set_error("error.bios_not_read", id);
		return 0;
	}
	if (gg)
		g_core->GetMemory()->EnableBootromGG(true);
	else
		g_core->GetMemory()->EnableBootromSMS(true);
	return 1;
}

static const char *cart_slot(void)
{
	return g_system == SYS_GG ? "cart_gg" : g_system == SYS_SG ? "cart_sg" : "cart_sms";
}

/* the battery RAM a save file of the project puts back, before the machine
 * runs (docs/save-data.md in Chimera: names are identity) */
static int load_savedata(void)
{
	char name[512];
	if (wbx_slot_count("savedata") <= 0 || wbx_slot_name("savedata", 0, name, sizeof name) == NULL)
		return 1;
	long size = 0;
	uint8_t *buf = read_file(name, &size);
	if (!buf)
	{
		set_error("error.not_read", name);
		return 0;
	}
	MemoryRule *rule = g_core->GetMemory()->GetCurrentRule();
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

extern "C" {

ECL_EXPORT const char *GetLoadError(void)
{
	return g_loadError;
}

ECL_EXPORT int Init(void)
{
	g_loadError[0] = 0;

	if (setting_is("system", "gg", "sms")) g_system = SYS_GG;
	else if (setting_is("system", "sg1000", "sms")) g_system = SYS_SG;
	else g_system = SYS_SMS;

	g_core = new GearsystemCore();
	g_core->Init(GS_PIXEL_BGRA8888);

	read_force_config();
	apply_machine_settings();
	if (!load_bios())
		return 0;

	/* the cartridge: the project's slot for this machine, else the plain
	 * rom mount */
	char name[512];
	const char *file = "rom";
	if (wbx_slot_count(cart_slot()) > 0 && wbx_slot_name(cart_slot(), 0, name, sizeof name) != NULL)
		file = name;
	long size = 0;
	uint8_t *rom = read_file(file, &size);
	if (!rom)
	{
		set_error(size < 0 ? "error.not_read" : "error.no_file", size < 0 ? file : NULL);
		return 0;
	}
	g_config.system = choose_system(rom, size, file);
	bool loaded = size > 0 && size <= MAX_ROM_SIZE
		&& g_core->LoadROMFromBuffer(rom, (int)size, &g_config, file);
	delete[] rom;
	if (!loaded)
	{
		set_error("error.rejected", file);
		return 0;
	}
	/* Video::Reset keeps what was set; set it again all the same, so a
	 * later upstream that resets it on load cannot drop a setting */
	apply_machine_settings();

	g_port[0] = port_device("port1", true);
	g_port[1] = port_device("port2", false);
	if (g_system == SYS_GG)
	{
		g_port[0] = DEV_PAD;
		g_port[1] = DEV_NONE;
	}
	apply_devices();

	if (!load_savedata())
		return 0;

	GS_RuntimeInfo info;
	g_core->GetRuntimeInfo(info);
	g_vwidth = info.screen_width;
	g_vheight = info.screen_height;
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

static void key(GS_Joypads pad, GS_Keys k, bool down)
{
	if (down)
		g_core->KeyPressed(pad, k);
	else
		g_core->KeyReleased(pad, k);
}

/* one pad's six controls from the wire, opposite directions settled the
 * libretro core's way unless the project allows both */
static void pad(int player, int base)
{
	GS_Joypads p = (GS_Joypads)player;
	bool up = g_buttons[base + 0], down = g_buttons[base + 1];
	bool left = g_buttons[base + 2], right = g_buttons[base + 3];
	if (!g_allowUpDown)
	{
		if (up && down)
		{
			if (g_latchV[player] == 0) g_latchV[player] = 1;
			up = g_latchV[player] > 0;
			down = !up;
		}
		else
			g_latchV[player] = up ? 1 : down ? -1 : 0;
		if (left && right)
		{
			if (g_latchH[player] == 0) g_latchH[player] = 1;
			left = g_latchH[player] > 0;
			right = !left;
		}
		else
			g_latchH[player] = left ? 1 : right ? -1 : 0;
	}
	key(p, Key_Up, up);
	key(p, Key_Down, down);
	key(p, Key_Left, left);
	key(p, Key_Right, right);
	key(p, Key_1, g_buttons[base + 4]);
	key(p, Key_2, g_buttons[base + 5]);
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

static void apply_inputs(void)
{
	switch (g_system)
	{
	case SYS_SMS:
		g_core->SetReset(g_buttons[1]);
		key(Joypad_1, Key_Start, g_buttons[2]); /* the Pause button: an NMI as it goes down */
		if (g_port[0] != DEV_NONE) pad(0, 3);
		if (g_port[1] != DEV_NONE) pad(1, 9);
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
		pad(0, 1);
		key(Joypad_1, Key_Start, g_buttons[7]);
		break;
	default:
		key(Joypad_1, Key_Start, g_buttons[1]); /* the Pause button */
		if (g_port[0] != DEV_NONE) pad(0, 2);
		if (g_port[1] != DEV_NONE) pad(1, 8);
		break;
	}
}

extern "C" {

ECL_EXPORT void FrameAdvance(uint64_t packed)
{
	(void)packed; /* the controls arrive through SetButton and SetAxis */
	if (!g_inited)
		return;

	/* Power, held, is the console switched off and on: the libretro core's
	 * reset, which keeps the battery RAM */
	if (g_buttons[0])
	{
		g_core->ResetROMPreservingRAM(&g_config);
		apply_devices();
	}

	apply_inputs();

	g_inputRead = 0;
	int count = 0;
	g_core->RunToVBlank(g_frame, g_audio, &count, NULL, g_render != 0);
	if (count < 0) count = 0;
	if (count > (int)(sizeof g_audio / sizeof g_audio[0])) count = (int)(sizeof g_audio / sizeof g_audio[0]);
	g_nsamples = count / 2;

	if (g_render)
	{
		GS_RuntimeInfo info;
		g_core->GetRuntimeInfo(info);
		g_vwidth = info.screen_width;
		g_vheight = info.screen_height;
	}
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

/* ---- memory domains ---- */
static const char *memdom(int which, uint8_t **area, int64_t *size, int *writable)
{
	*writable = 1;
	switch (which)
	{
	case 0:
		/* the console's RAM: 8 KiB at 0xC000, 1 KiB on an SG-1000 */
		*area = g_core->GetMemory()->GetMemoryMap() + 0xC000;
		*size = g_core->GetCartridge()->IsSG1000() ? 0x400 : 0x2000;
		return "Main RAM";
	case 1:
		*area = g_core->GetVideo()->GetVRAM();
		*size = 0x4000;
		return "VRAM";
	case 2:
	{
		Cartridge *c = g_core->GetCartridge();
		if (c->IsSG1000())
			return NULL; /* the TMS9918's colours are fixed */
		*area = g_core->GetVideo()->GetCRAM();
		*size = (c->IsGameGear() && !c->IsGameGearInSMSMode()) ? 0x40 : 0x20;
		return "CRAM";
	}
	case 3:
	{
		MemoryRule *rule = g_core->GetMemory()->GetCurrentRule();
		if (!rule || rule->GetRamBanks() == NULL || rule->GetRamSize() == 0)
			return NULL;
		*area = rule->GetRamBanks();
		*size = (int64_t)rule->GetRamSize();
		return "Cartridge RAM";
	}
	case 4:
		*area = g_core->GetCartridge()->GetROM();
		*size = g_core->GetCartridge()->GetROMSize();
		*writable = 0;
		return "ROM";
	default:
		return NULL;
	}
}

#define DOMAIN_KINDS 5
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

/* ---- save data: the cartridge's battery RAM, once the game has used it ---- */
static MemoryRule *battery(void)
{
	if (!g_inited) return NULL;
	MemoryRule *rule = g_core->GetMemory()->GetCurrentRule();
	if (!rule || !rule->PersistedRAM() || rule->GetRamBanks() == NULL || rule->GetRamSize() == 0)
		return NULL;
	return rule;
}
ECL_EXPORT int32_t GetSaveDataFileCount(void) { return battery() ? 1 : 0; }
ECL_EXPORT const char *GetSaveDataFileName(int32_t i) { return i == 0 && battery() ? "battery.sav" : NULL; }
ECL_EXPORT int64_t GetSaveDataFileSize(int32_t i)
{
	MemoryRule *r = i == 0 ? battery() : NULL;
	return r ? (int64_t)r->GetRamSize() : 0;
}
ECL_EXPORT const uint8_t *GetSaveDataFileBuffer(int32_t i)
{
	MemoryRule *r = i == 0 ? battery() : NULL;
	return r ? r->GetRamBanks() : NULL;
}

/* ---- the Z80's registers ---- */
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
