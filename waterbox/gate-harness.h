/* gate-harness.h - the half run-native.c and run-wbx.c share.
 *
 * One run-and-digest loop over an abstract core, so the native reference and
 * the sandboxed core run exactly the same schedule of controls and report the
 * same digests: the picture, the sound, the lag frames and every memory
 * domain. The two programs differ only in how they reach the core.
 *
 * The controls are the wire of waterbox.config, which differs by machine:
 *   sms: 15 buttons (Power, Reset, Pause, P1 x6, P2 x6) and 7 axes
 *   gg:   8 buttons (Power, P1 x6, P1 Start)
 *   sg:  14 buttons (Power, Pause, P1 x6, P2 x6)
 * Button 0 is Power on every machine; the exercise never presses it.
 */
#ifndef GATE_HARNESS_H
#define GATE_HARNESS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GATE_MAX_BUTTONS 16
#define GATE_AXES 7

struct gate_core
{
	int (*init)(void);
	const char *(*load_error)(void);
	void (*set_button)(int32_t index, int32_t state);
	void (*set_axis)(int32_t index, int32_t value);
	void (*frame)(void);
	const uint32_t *(*video)(int *w, int *h);
	const int16_t *(*audio)(int *n);
	int (*input_was_read)(void);
	int (*domain_count)(void);
	const char *(*domain_name)(int i);
	const uint8_t *(*domain_ptr)(int i);
	int64_t (*domain_size)(int i);
	int (*vsync_numerator)(void);
	int (*vsync_denominator)(void);
	int32_t (*savedata_count)(void);
	const char *(*savedata_name)(int32_t i);
	int64_t (*savedata_size)(int32_t i);
	const uint8_t *(*savedata_buffer)(int32_t i);
	/* optional: before each frame (the rerecord test's save and load) */
	void (*pre_frame)(void);
	/* optional: the core's drawing on and off (the turbo test) */
	void (*set_rendering)(int on);
};

struct gate_opts
{
	long frames;
	const char *sys;            /* sms | gg | sg: how many buttons the wire has */
	int exercise;               /* press the controls in a fixed pseudo-random pattern */
	int wiggleAxes;             /* move every axis in a fixed pseudo-random pattern */
	const char *hold;           /* comma-separated button indices held on every frame */
	const char *inputsPath;     /* a file of lines of 0 and 1, one line a frame, one column a button */
	const char *screenshotPath; /* the last frame, as a .tga */
	const char *dumpDomain;     /* a memory domain to write... */
	const char *dumpPath;       /* ...into this file after the run */
	const char *savedataDir;    /* every save-data export written here after the run */
	int turbo;                  /* draw nothing for the first half of the run */
	long turboSettle;           /* frames to let the picture settle before hashing it */
	long powerAt;               /* hold Power on this frame (-1: never) */
};

static int gate_button_count(const char *sys)
{
	if (!strcmp(sys, "gg")) return 8;
	if (!strcmp(sys, "sg")) return 14;
	return 15;
}

static uint64_t gate_fnv(uint64_t h, const void *p, size_t n)
{
	const uint8_t *b = (const uint8_t *)p;
	if (!h) h = 1469598103934665603ULL;
	for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ULL; }
	return h;
}

static uint64_t gate_mix(uint64_t x)
{
	x = x * 6364136223846793005ULL + 1442695040888963407ULL;
	x ^= x >> 33;
	x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 29;
	return x;
}

static int gate_write_tga(const char *path, const uint32_t *bgra, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f) return 0;
	uint8_t hdr[18] = { 0 };
	hdr[2] = 2;
	hdr[12] = w & 0xff; hdr[13] = (w >> 8) & 0xff;
	hdr[14] = h & 0xff; hdr[15] = (h >> 8) & 0xff;
	hdr[16] = 32;
	hdr[17] = 0x20;
	fwrite(hdr, 1, 18, f);
	fwrite(bgra, 4, (size_t)w * h, f);
	fclose(f);
	return 1;
}

static int gate_run(const struct gate_core *c, const struct gate_opts *o)
{
	const int nb = gate_button_count(o->sys);
	uint8_t holdMask[GATE_MAX_BUTTONS];
	memset(holdMask, 0, sizeof holdMask);
	if (o->hold && *o->hold)
	{
		const char *s = o->hold;
		while (*s)
		{
			long i = strtol(s, (char **)&s, 10);
			if (i > 0 && i < nb) holdMask[i] = 1;
			if (*s == ',') s++;
			else if (*s) { fprintf(stderr, "bad --hold list %s\n", o->hold); return 2; }
		}
	}
	FILE *inputs = NULL;
	if (o->inputsPath && !(inputs = fopen(o->inputsPath, "r")))
	{
		fprintf(stderr, "cannot read %s\n", o->inputsPath);
		return 1;
	}

	if (c->init() != 1)
	{
		fprintf(stderr, "Init failed: %s\n", c->load_error ? c->load_error() : "?");
		return 1;
	}

	uint64_t vh = 0, ah = 0, th = 0;
	const long tail = o->frames / 2;
	const long hashFrom = tail + o->turboSettle;
	long lag = 0;
	long samples = 0;
	uint8_t buttons[GATE_MAX_BUTTONS], prev[GATE_MAX_BUTTONS];
	int32_t axes[GATE_AXES], prevAxes[GATE_AXES];
	memset(prev, 0, sizeof prev);
	for (int a = 0; a < GATE_AXES; a++) prevAxes[a] = -1;

	for (long f = 0; f < o->frames; f++)
	{
		memset(buttons, 0, sizeof buttons);
		for (int a = 0; a < GATE_AXES; a++) axes[a] = 32768;
		if (o->exercise)
		{
			uint64_t x = gate_mix((uint64_t)f);
			for (int k = 1; k < nb; k++)
				buttons[k] = (x >> (k * 3)) & 1;
		}
		if (o->wiggleAxes)
		{
			uint64_t w = gate_mix((uint64_t)f + 0x9e3779b97f4a7c15ULL);
			for (int a = 0; a < GATE_AXES; a++)
				axes[a] = (int32_t)((w >> (a * 9)) & 0xffff);
		}
		for (int k = 1; k < nb; k++)
			if (holdMask[k]) buttons[k] = 1;
		if (inputs)
		{
			char line[256];
			if (fgets(line, sizeof line, inputs))
				for (int k = 0; k < nb && line[k] && line[k] != '\n'; k++)
					buttons[k] = line[k] == '1';
		}
		buttons[0] = f == o->powerAt;

		if (o->turbo && c->set_rendering)
			c->set_rendering(f >= tail);

		if (c->pre_frame)
			c->pre_frame();

		for (int i = 0; i < nb; i++)
		{
			if (buttons[i] != prev[i])
				c->set_button(i, buttons[i]);
			prev[i] = buttons[i];
		}
		for (int a = 0; a < GATE_AXES; a++)
		{
			if (axes[a] != prevAxes[a])
				c->set_axis(a, axes[a]);
			prevAxes[a] = axes[a];
		}

		c->frame();

		int w = 0, h = 0, n = 0;
		const uint32_t *video = c->video(&w, &h);
		const int16_t *audio = c->audio(&n);
		vh = gate_fnv(vh, &w, sizeof w);
		vh = gate_fnv(vh, &h, sizeof h);
		vh = gate_fnv(vh, video, (size_t)w * h * 4);
		if (f >= hashFrom)
		{
			th = gate_fnv(th, &w, sizeof w);
			th = gate_fnv(th, &h, sizeof h);
			th = gate_fnv(th, video, (size_t)w * h * 4);
		}
		ah = gate_fnv(ah, audio, (size_t)n * 2 * sizeof(int16_t));
		samples += n;
		if (!c->input_was_read())
			lag++;

		if (o->screenshotPath && f == o->frames - 1)
			gate_write_tga(o->screenshotPath, video, w, h);
	}
	if (inputs) fclose(inputs);

	int w = 0, h = 0;
	c->video(&w, &h);
	printf("frames=%ld\n", o->frames);
	printf("vsync=%d/%d\n", c->vsync_numerator(), c->vsync_denominator());
	printf("picture=%dx%d\n", w, h);
	printf("videoHash=%016llx\n", (unsigned long long)vh);
	printf("tailVideoHash=%016llx\n", (unsigned long long)th);
	printf("audioHash=%016llx\n", (unsigned long long)ah);
	printf("audioSamples=%ld\n", samples);
	printf("lagFrames=%ld\n", lag);
	int nd = c->domain_count();
	for (int i = 0; i < nd; i++)
	{
		uint64_t dh = gate_fnv(0, c->domain_ptr(i), (size_t)c->domain_size(i));
		printf("domain[%s]=%016llx\n", c->domain_name(i), (unsigned long long)dh);
	}
	if (o->savedataDir)
	{
		int32_t count = c->savedata_count();
		printf("savedataFiles=%d\n", count);
		for (int32_t i = 0; i < count; i++)
		{
			char path[4096];
			snprintf(path, sizeof path, "%s/%s", o->savedataDir, c->savedata_name(i));
			FILE *f = fopen(path, "wb");
			if (!f) { perror(path); return 1; }
			fwrite(c->savedata_buffer(i), 1, (size_t)c->savedata_size(i), f);
			fclose(f);
		}
	}
	if (o->dumpDomain && o->dumpPath)
	{
		for (int i = 0; i < nd; i++)
		{
			if (strcmp(c->domain_name(i), o->dumpDomain) != 0)
				continue;
			FILE *f = fopen(o->dumpPath, "wb");
			if (!f) { perror(o->dumpPath); return 1; }
			fwrite(c->domain_ptr(i), 1, (size_t)c->domain_size(i), f);
			fclose(f);
			return 0;
		}
		fprintf(stderr, "no such domain to dump: %s\n", o->dumpDomain);
		return 1;
	}
	return 0;
}

static int gate_parse_opts(int argc, char **argv, int first, struct gate_opts *o)
{
	memset(o, 0, sizeof *o);
	o->frames = 600;
	o->sys = "sms";
	o->powerAt = -1;
	for (int i = first; i < argc; i++)
	{
		if (!strcmp(argv[i], "--frames") && i + 1 < argc) o->frames = strtol(argv[++i], 0, 0);
		else if (!strcmp(argv[i], "--sys") && i + 1 < argc) o->sys = argv[++i];
		else if (!strcmp(argv[i], "--exercise")) o->exercise = 1;
		else if (!strcmp(argv[i], "--wiggle-axes")) o->wiggleAxes = 1;
		else if (!strcmp(argv[i], "--hold") && i + 1 < argc) o->hold = argv[++i];
		else if (!strcmp(argv[i], "--inputs") && i + 1 < argc) o->inputsPath = argv[++i];
		else if (!strcmp(argv[i], "--power-at") && i + 1 < argc) o->powerAt = strtol(argv[++i], 0, 0);
		else if (!strcmp(argv[i], "--screenshot") && i + 1 < argc) o->screenshotPath = argv[++i];
		else if (!strcmp(argv[i], "--dump-domain") && i + 2 < argc) { o->dumpDomain = argv[++i]; o->dumpPath = argv[++i]; }
		else if (!strcmp(argv[i], "--savedata-out") && i + 1 < argc) o->savedataDir = argv[++i];
		else if (!strcmp(argv[i], "--turbo")) o->turbo = 1;
		else if (!strcmp(argv[i], "--turbo-settle") && i + 1 < argc) o->turboSettle = strtol(argv[++i], 0, 0);
		else if (!strcmp(argv[i], "--rerecord")) ; /* run-wbx's own */
		else { fprintf(stderr, "unknown argument %s\n", argv[i]); return 0; }
	}
	if (strcmp(o->sys, "sms") && strcmp(o->sys, "gg") && strcmp(o->sys, "sg"))
	{
		fprintf(stderr, "--sys is sms, gg or sg\n");
		return 0;
	}
	return 1;
}

#endif /* GATE_HARNESS_H */
