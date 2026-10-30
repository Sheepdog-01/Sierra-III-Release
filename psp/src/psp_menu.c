/* psp_menu.c - the launch menu (build 66)

Shown before the game starts, on the PSP's debug screen (libpspdebug: a
plain text console on the frame buffer; the GE isn't up yet). Two pages:

- maps: every .map in ms0:/halo/maps (ui.map aside), with whether its
  texture pack (.ptex) is beside it. The chosen map goes into init.txt as
  the game's map_name command (a multiplayer map's scenario is
  levels\test\<name>\<name>, a campaign map's levels\<name>\<name>: the map
  header's type says which).
- options: the keep words that matter day to day, as toggles, written to
  keep.txt. Words the menu doesn't list (anything typed by hand) are kept.

- presets: named sets of keep words (ms0:/halo/presets.txt).
- nub: the nub measured for ms0:/halo/nub.txt (nub_page, below).

Up/down move, left/right or cross toggle, L/R or triangle switch pages,
start saves both files and starts the game.

The front door (docs/plan_front_end.md): which screen the game starts on.
- ms0:/halo/input.txt (a scripted run: the rigs, the emulator) or
  ms0:/halo/nomenu.txt: no launch menu; init.txt and keep.txt are read as
  they are.
- ms0:/halo/menu.txt, or L held as the game starts: this launch menu.
- otherwise: the game's own main menu, as on the Xbox. init.txt still runs,
  without its map_name lines (psp_front_door_main_menu, read by the engine's
  console_startup: psp_fixups.py FIXES_FRONT), so the launch menu keeps the
  last map it chose.

The multiplayer build keeps its files in ms0:/halomp and its maps in
ms0:/halo/maps (psp_paths.h); its front door is the same. */
#include <pspkernel.h>
#include <pspctrl.h>
#include <pspdebug.h>
#include <pspdisplay.h>
#include <pspiofilemgr.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "psp_paths.h"
#define DATA_ROOT PSP_DATA_ROOT
#define MAXIMUM_MAPS 32
#define NAME_LENGTH 40

struct map_entry
{
	char name[NAME_LENGTH];
	int type; /* 0 campaign, 1 multiplayer, 2 ui, -1 unknown */
	int has_pack;
};

struct option
{
	const char *word;
	const char *label;
	/* a group: one of the group on at a time (0: on its own) */
	int group;
	int on;
};

/* build 93: trimmed: near2, near4, overlayslope1, overlayslope4,
decalall, flatshade, tdump, visdump, noboxtest, detailallfar, levelcompact and
interpolation left the menu. They still work typed into keep.txt, and the
menu keeps them there as "other words" */
static struct option options[] = {
	{ "faceaim", "controls: nub moves, face buttons aim (PSP shooters)", 0, 0 },
	{ "nearauto", "near plane: by what is near the eye", 1, 0 },
	{ "nooverlay", "performance overlay OFF at start", 0, 0 }, { "overlay", "performance overlay ON at start (a release starts without it)", 0, 0 },
	{ "nomeaudio", "sound mixed on the main CPU, not the ME", 0, 0 },
	/* build 114's switches (docs/build_b114.md): b113's defaults back */
	{ "list512", "display list 512 KB (b113's; 768 now)", 0, 0 },
	{ "sound1536", "sound cache 1536 KB (b113's; 1792 now)", 0, 0 },
	{ "nosoundplace", "sound cache: big sounds not kept at the top", 0, 0 },
	{ "seethrough160", "see-through draws: 160 a frame (b113's; 320 now)", 0, 0 },
	{ "listcachedall", "GE command words written through the cache", 0, 0 },
	{ "lean", "lean play: timers and overlay off (notimers nooverlay)", 0, 0 },
	{ "statememo", "GE state the GE already has left out (binds, lighting)", 0, 0 },
	/* build 113's switches (docs/build_b113.md) */
	{ "nocontrails", "no tracers (contrails) behind shots", 0, 0 },
	{ "zoommask", "zoom: the mask as a soft edge (blur stand-in)", 6, 0 },
	{ "zoommaskfull", "zoom: the mask at full strength", 6, 0 },
	{ "gebusy", "GE busy time in the log", 0, 0 },
#ifdef PSP_MULTIPLAYER
	{ "gedefer", "GE calls written down, carried out at frame end", 0, 0 },
	{ "gemeon", "GE calls carried out on the ME (with gedefer)", 0, 0 },
#else
	/* the campaign has gedefer, gemeon and the rest of M6 #2 on unless
	told otherwise (psp_platform.c, keep_defaults) */
	{ "m6off", "frames in flight on the ME (M6 #2) OFF", 0, 0 },
#endif
	{ "weaksum", "b112's sleep checksum (the flip bug)", 0, 0 },
	{ "renderstates256", "render-state cache 256 (b112's)", 0, 0 },
	{ "benchrun", "benchmark: 4 views x 9 switches, ~6 min", 0, 0 },
	{ "assettrack", "asset track: the maps' _track packs", 5, 0 },
	{ "assetdetail", "asset track: the maps' _detail packs", 5, 0 },
#if PSP_DEBUG_TOOLS
	{ "meclassify", "ME speed test: 2 render jobs, measure only", 0, 0 },
#endif
	{ "hwprofile", "CPU counters in the log (benchrun: always)", 0, 0 },
	{ "handletest", "file handles: how many are free 15 s in", 0, 0 },
	{ "blackbox", "crash hunt: I/O traced in play, log saved each frame", 0, 0 },
	{ "crashplay", "crash test: a deliberate crash 20 s into play", 0, 0 },
	{ "lightwhite", "objects lit as before b102 (full light)", 0, 0 },
	{ "crumbs", "crash hunt: last steps drawn on the screen (slow)", 0, 0 },
	{ "detailauto", "detail maps: the GE's own level choice", 0, 0 },
	{ "noangle", "no slant buckets for texture levels", 0, 0 },
	{ "mipauto", "all textures: the GE's own level choice", 0, 0 },
	{ "nodetail", "no detail maps", 0, 0 },
	{ "noshadows", "no shadows", 0, 0 },
	{ "nodecals", "no decals", 0, 0 },
	{ "nohud", "no HUD", 0, 0 },
	{ "novblankthrottle", "Halo's frame pacing off", 0, 0 },
	{ "novfpu", "no VFPU", 0, 0 },
	{ "texsqueeze", "texture RAM cache capped at 1 MB: stream", 3, 0 },
	{ "texvramonly", "no texture RAM cache: VRAM only", 3, 0 },
	{ "geoasync", "geometry reads asynchronous", 0, 0 },
	{ "geo15", "campaign geometry cache 1.5 MB (less for textures)", 4, 0 },
	{ "geo2", "campaign geometry cache 2 MB (less for textures)", 4, 0 },
};
#define OPTION_COUNT (int)(sizeof(options) / sizeof(options[0]))
/* rows for options: 34 on the screen, less the 3-line header and 3 lines
around the list */
#define OPTION_ROWS 26
/* the debug screen is 68 columns: a line that fills all 68 wraps by itself
and its newline then adds a blank row (b91: every option took two rows, so
the list ran past the bottom and wrapped over the top). Lines stay at 67 */
/* build 92: the menu drawn into two buffers in turn (VRAM, uncached view) */
#define MENU_VRAM 0x44000000u
#define MENU_BUFFER_BYTES (512u * 272u * 4u)

static struct map_entry maps[MAXIMUM_MAPS];
static int map_count = 0;
/* words in keep.txt the menu doesn't know: kept as typed */
static char extra_words[384] = ""; /* build 116: 384 (256), for the longer presets */

/* build 114: presets, ms0:/halo/presets.txt: one a line, "name: keep words"
(# starts a comment). A build's zip can carry the night's tests as presets,
so a test is one press instead of a dozen toggles. Applying one sets exactly
its words: the options it names on, every other option off, its other words
kept as "other words" */
/* one row each and the cursor's words under the list: 3 header rows, 22
names, a blank, 5 rows of words and the applied line take 32 of the screen's
34 rows (a line past the last row wraps to the top) */
#define MAXIMUM_PRESETS 22
#define PRESET_WORD_ROWS 5
#define PRESET_NAME 48
/* build 116: 320 (200 before): b116's "every restoration off" preset is 199 */
#define PRESET_WORDS 320
/* taken from the heap for the menu's life only (the program sits a few KB
under one of the heap's 64 KB steps) */
static struct preset { char name[PRESET_NAME]; char words[PRESET_WORDS]; } *presets = NULL;
static int preset_count = 0, preset_applied = -1;
/* lines past MAXIMUM_PRESETS: said on the page, so a test's preset can't go
missing without a word */
static int presets_left_out = 0;

static void load_presets(void)
{
	SceUID file = sceIoOpen(DATA_ROOT "/presets.txt", PSP_O_RDONLY, 0);
	enum { TEXT_BYTES = 4096 };
	char *text, *line, *save = NULL;
	int length;

	preset_count = 0;
	presets_left_out = 0;
	if (file < 0)
		return;
	text = malloc(TEXT_BYTES);
	presets = malloc(MAXIMUM_PRESETS * sizeof(*presets));
	if (!text || !presets)
	{
		free(text);
		free(presets);
		presets = NULL;
		sceIoClose(file);
		return;
	}
	length = sceIoRead(file, text, TEXT_BYTES - 1);
	sceIoClose(file);
	if (length <= 0)
	{
		free(text);
		return;
	}
	text[length] = '\0';
	for (line = strtok_r(text, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save))
	{
		char *colon = strchr(line, ':');

		if (line[0] == '#' || !colon)
			continue;
		if (preset_count == MAXIMUM_PRESETS)
		{
			presets_left_out++;
			continue;
		}
		*colon = '\0';
		snprintf(presets[preset_count].name, PRESET_NAME, "%s", line);
		for (colon++; *colon == ' '; colon++)
			;
		snprintf(presets[preset_count].words, PRESET_WORDS, "%s", colon);
		preset_count++;
	}
	free(text);
}

/* the preset the game was started with, for the launch menu's header and
the screenshots' stamp line: its number (the digits its name starts with, or
its place in the list), "+" once an option is changed after it; empty when
none was applied (a test run's keep.txt, or words set by hand) */
static char preset_label[8] = "";

const char *psp_preset_label(void)
{
	return preset_label;
}

static void apply_preset(int p)
{
	char words[PRESET_WORDS], *word, *save = NULL;
	int i;

	for (i = 0; i < OPTION_COUNT; i++)
		options[i].on = 0;
	extra_words[0] = '\0';
	snprintf(words, sizeof(words), "%s", presets[p].words);
	for (word = strtok_r(words, " \t,", &save); word; word = strtok_r(NULL, " \t,", &save))
	{
		int known = 0;

		for (i = 0; i < OPTION_COUNT; i++)
			if (strcmp(options[i].word, word) == 0)
				options[i].on = known = 1;
		if (!known && strlen(extra_words) + strlen(word) + 2 < sizeof(extra_words))
		{
			strcat(extra_words, word);
			strcat(extra_words, " ");
		}
	}
	preset_applied = p;
	{
		int number = atoi(presets[p].name);

		snprintf(preset_label, sizeof(preset_label), "%d", number > 0 ? number : p + 1);
	}
}

static int file_exists(const char *path)
{
	SceIoStat status;

	return sceIoGetstat(path, &status) >= 0;
}

void platform_log(const char *format, ...);

/* set when the game starts on its main menu, so init.txt's map_name lines
(the launch menu's last choice) don't take it straight into a level */
static int front_door_main_menu = 0;

int psp_front_door_main_menu(void)
{
	return front_door_main_menu;
}

/* set when this launch went through the launch menu: the boot screen's hint
on how to open it is left out then (psp_loading.c) */
static int front_door_launch_menu = 0;

int psp_front_door_launch_menu(void)
{
	return front_door_launch_menu;
}

/* the buttons held as the game starts; reading waits for the next sample,
so it sees the pad as it is now */
static unsigned int buttons_held(void)
{
	SceCtrlData pad;

	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
	sceCtrlReadBufferPositive(&pad, 1);
	return pad.Buttons;
}

static int map_type(const char *path)
{
	SceUID file = sceIoOpen(path, PSP_O_RDONLY, 0);
	unsigned char header[0x64];
	int type = -1;

	if (file < 0)
		return -1;
	if (sceIoRead(file, header, sizeof(header)) == (int)sizeof(header) && header[0] == 'd' && header[1] == 'a' &&
		header[2] == 'e' && header[3] == 'h')
		type = header[0x60] | (header[0x61] << 8);
	sceIoClose(file);
	return type;
}

static void scan_maps(void)
{
	SceUID directory = sceIoDopen(PSP_MAPS_ROOT);
	SceIoDirent entry;

	map_count = 0;
	if (directory < 0)
		return;
	memset(&entry, 0, sizeof(entry));
	while (map_count < MAXIMUM_MAPS && sceIoDread(directory, &entry) > 0)
	{
		int length = strlen(entry.d_name);
		char path[128];

		if (length > 4 && length < NAME_LENGTH + 4 && strcmp(entry.d_name + length - 4, ".map") == 0 &&
			strcmp(entry.d_name, "ui.map") != 0)
		{
			struct map_entry *map = &maps[map_count];

			memcpy(map->name, entry.d_name, length - 4);
			map->name[length - 4] = '\0';
			snprintf(path, sizeof(path), PSP_MAPS_ROOT "/%s", entry.d_name);
			map->type = map_type(path);
			snprintf(path, sizeof(path), PSP_MAPS_ROOT "/%s.ptex", map->name);
			map->has_pack = file_exists(path);
			map_count++;
		}
		memset(&entry, 0, sizeof(entry));
	}
	sceIoDclose(directory);
	/* alphabetical */
	{
		int i, j;

		for (i = 1; i < map_count; i++)
			for (j = i; j > 0 && strcmp(maps[j - 1].name, maps[j].name) > 0; j--)
			{
				struct map_entry swap = maps[j - 1];

				maps[j - 1] = maps[j];
				maps[j] = swap;
			}
	}
}

/* the current map from init.txt's map_name line: the last path element */
static int current_map(void)
{
	SceUID file = sceIoOpen(DATA_ROOT "/init.txt", PSP_O_RDONLY, 0);
	char text[256];
	int length, i;

	if (file < 0)
		return 0;
	length = sceIoRead(file, text, sizeof(text) - 1);
	sceIoClose(file);
	if (length <= 0)
		return 0;
	text[length] = '\0';
	{
		char *line = strstr(text, "map_name");
		char *end, *last;

		if (!line)
			return 0;
		line += 8;
		while (*line == ' ' || *line == '\t')
			line++;
		end = line;
		while (*end && *end != '\r' && *end != '\n' && *end != ' ')
			end++;
		*end = '\0';
		last = line;
		for (i = 0; line[i]; i++)
			if (line[i] == '\\' || line[i] == '/')
				last = line + i + 1;
		for (i = 0; i < map_count; i++)
			if (strcmp(maps[i].name, last) == 0)
				return i;
	}
	return 0;
}

static void load_options(void)
{
	SceUID file = sceIoOpen(DATA_ROOT "/keep.txt", PSP_O_RDONLY, 0);
	/* build 116: 512 (256 before), as the game reads keep.txt (review: the
	menu read a long set's first 255 bytes and wrote the cut set back) */
	char text[512], *word, *save = NULL;
	int length, i;

	extra_words[0] = '\0';
	if (file < 0)
		return;
	length = sceIoRead(file, text, sizeof(text) - 1);
	sceIoClose(file);
	if (length <= 0)
		return;
	text[length] = '\0';
	for (i = 0; text[i]; i++)
		if (text[i] == '\n' || text[i] == '\r' || text[i] == '\t' || text[i] == ',')
			text[i] = ' ';
	word = strtok_r(text, " ", &save);
	while (word)
	{
		int known = 0;

		for (i = 0; i < OPTION_COUNT; i++)
			if (strcmp(options[i].word, word) == 0)
			{
				options[i].on = 1;
				known = 1;
			}
		if (!known && strlen(extra_words) + strlen(word) + 2 < sizeof(extra_words))
		{
			strcat(extra_words, word);
			strcat(extra_words, " ");
		}
		word = strtok_r(NULL, " ", &save);
	}
}

static void save_files(int map)
{
	SceUID file = sceIoOpen(DATA_ROOT "/keep.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
	char text[512];
	int length = 0, i;

	if (file >= 0)
	{
		/* build 116: the length held inside the buffer (snprintf answers
		what it would have written, so a long set ran length past the
		buffer and the next size went negative). keep.txt is read into 512
		bytes (psp_platform.c, keep_words) */
		for (i = 0; i < OPTION_COUNT; i++)
			if (options[i].on && length < (int)sizeof(text) - 1)
				length += snprintf(text + length, sizeof(text) - length, "%s ", options[i].word);
		if (length < (int)sizeof(text) - 1)
			length += snprintf(text + length, sizeof(text) - length, "%s\n", extra_words);
		/* the game reads at most 509 bytes of it (psp_platform.c, keep_load) */
		if (length > 509)
			length = 509;
		sceIoWrite(file, text, length);
		sceIoClose(file);
	}
	if (map >= 0 && map < map_count)
	{
		file = sceIoOpen(DATA_ROOT "/init.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);
		if (file >= 0)
		{
			if (maps[map].type == 0)
				length = snprintf(text, sizeof(text), "map_name levels\\%s\\%s\n", maps[map].name, maps[map].name);
			else
				length = snprintf(text, sizeof(text), "map_name levels\\test\\%s\\%s\n", maps[map].name, maps[map].name);
			sceIoWrite(file, text, length);
			sceIoClose(file);
		}
	}
}

static void nub_draw(SceCtrlData const *pad);

static void draw(int page, int cursor, int map, int build, SceCtrlData const *pad)
{
	int i;

	pspDebugScreenSetXY(0, 0);
	pspDebugScreenSetTextColor(0xFFFFFFFF);
	pspDebugScreenPrintf("Halo PSP build %d  preset %-4s  %-32s\n", build, preset_label[0] ? preset_label : "none",
		page == 0 ? "MAPS  (R: options)" : page == 1 ? "OPTIONS  (L: maps, R: presets)" :
		page == 2 ? "PRESETS  (L: options, R: nub)" : "NUB  (L: presets)");
	pspDebugScreenPrintf(page == 3 ? "START plays\n\n" : page == 2 ? "up/down move, cross applies, START plays\n\n" :
		"up/down move, left/right/cross toggle, START plays\n\n");
	if (page == 3)
		nub_draw(pad);
	else if (page == 2)
	{
		for (i = 0; i < preset_count; i++)
		{
			pspDebugScreenSetTextColor(i == cursor ? 0xFF00FFFF : 0xFFFFFFFF);
			pspDebugScreenPrintf(" %c %-.62s\n", i == preset_applied ? '*' : ' ', presets[i].name);
		}
		if (!preset_count)
			pspDebugScreenPrintf(" no presets (ms0:/halo/presets.txt: one a line, \"name: keep words\")\n");
		else
		{
			/* the cursor's words, padded so a shorter set covers the last one */
			const char *words = cursor >= 0 && cursor < preset_count ? presets[cursor].words : "";
			int row, length = (int)strlen(words);

			pspDebugScreenSetTextColor(0xFFAAAAAA);
			pspDebugScreenPrintf("\n");
			for (row = 0; row < PRESET_WORD_ROWS; row++)
				pspDebugScreenPrintf("   %-64.64s\n", row * 64 < length ? words + row * 64 : "");
			if (presets_left_out)
				pspDebugScreenSetTextColor(0xFF4040FF);
			pspDebugScreenPrintf(" %-66.66s\n", presets_left_out ? "presets.txt holds more than this page: those past it left out" :
				preset_applied >= 0 ? "applied: the options page shows its words" : "");
		}
	}
	else if (page == 0)
	{
		for (i = 0; i < map_count; i++)
		{
			pspDebugScreenSetTextColor(i == cursor ? 0xFF00FFFF : 0xFFFFFFFF);
			pspDebugScreenPrintf(" %c %-24s %-12s %s\n", i == map ? '*' : ' ', maps[i].name,
				maps[i].type == 1 ? "multiplayer" : maps[i].type == 0 ? "campaign" : "?",
				maps[i].has_pack ? "textures" : "NO TEXTURE PACK");
		}
		if (!map_count)
			pspDebugScreenPrintf(" no maps in " PSP_MAPS_ROOT "\n");
	}
	else
	{
		/* build 91: the flag first, then what it does; and a scrolling
		window, since the list is longer than the 34-row screen (rows past
		the bottom wrapped to the top and wrote over the header and the
		controls line, which flickered as the cursor moved) */
		int first = cursor - OPTION_ROWS / 2;

		if (first > OPTION_COUNT - OPTION_ROWS)
			first = OPTION_COUNT - OPTION_ROWS;
		if (first < 0)
			first = 0;
		pspDebugScreenSetTextColor(0xFFAAAAAA);
		{
			char note[40] = "";

			if (first > 0)
				snprintf(note, sizeof(note), "... %d more above", first);
			pspDebugScreenPrintf("     %-62s\n", note);
		}
		for (i = first; i < OPTION_COUNT && i < first + OPTION_ROWS; i++)
		{
			pspDebugScreenSetTextColor(i == cursor ? 0xFF00FFFF : 0xFFFFFFFF);
			pspDebugScreenPrintf(" [%c] %-16s %-45.45s\n", options[i].on ? 'x' : ' ', options[i].word, options[i].label);
		}
		pspDebugScreenSetTextColor(0xFFAAAAAA);
		{
			char note[40] = "";

			if (i < OPTION_COUNT)
				snprintf(note, sizeof(note), "... %d more below", OPTION_COUNT - i);
			pspDebugScreenPrintf("     %-62s\n", note);
		}
		pspDebugScreenPrintf(" other words kept: %-49.49s", extra_words);
	}
	pspDebugScreenSetTextColor(0xFFFFFFFF);
}

/* ---------- the NUB page: the nub measured for ms0:/halo/nub.txt, which
psp_sdl.c reads (nub_calibrate_load; docs/game_feature_survey.md, section
4.6). Every PSP's nub rests and reaches differently, so it is measured, not
assumed: let go (its rest point), roll it round its edge (its reach each way),
let go again (where it comes back to: how far it lands from its rest sets the
dead radius, so a nub that settles off-centre doesn't drift), then save.
Circle removes the file, and the nub maps as it does without one */
#define NUB_PATH DATA_ROOT "/nub.txt"
#define NUB_SAMPLES 128
/* the least samples a let-go step takes (about a second at the menu's 60 ms) */
#define NUB_SETTLE 16
enum { NUB_IDLE, NUB_REST, NUB_EDGE, NUB_RETURN, NUB_DONE };
static struct
{
	/* first: the samples the first let-go took (at most half) */
	int step, count, first;
	unsigned char x[NUB_SAMPLES], y[NUB_SAMPLES];
	int low[2], high[2];
	float rest[2], dead, reach[4];
	char saved[96], message[64];
} nub;

static void nub_read_saved(void)
{
	FILE *file = fopen(NUB_PATH, "r");
	char line[48];
	int at = 0;

	nub.saved[0] = 0;
	if (!file)
	{
		snprintf(nub.saved, sizeof(nub.saved), "none: the nub maps as it always has");
		return;
	}
	while (fgets(line, sizeof(line), file) && at < (int)sizeof(nub.saved) - 2)
	{
		line[strcspn(line, "\r\n")] = 0;
		at += snprintf(nub.saved + at, sizeof(nub.saved) - at, "%s%s", at ? ", " : "", line);
	}
	fclose(file);
}

/* the rest point is the mean of both let-go steps; the dead radius covers
the furthest any let-go sample sat from it, with a margin, so neither the
nub's jitter nor where it settles moves the player */
static void nub_work_out(void)
{
	float sx = 0.0f, sy = 0.0f, far = 0.0f;
	int i;

	for (i = 0; i < nub.count; i++)
	{
		sx += nub.x[i];
		sy += nub.y[i];
	}
	nub.rest[0] = sx / nub.count;
	nub.rest[1] = sy / nub.count;
	for (i = 0; i < nub.count; i++)
	{
		float dx = nub.x[i] - nub.rest[0], dy = nub.y[i] - nub.rest[1], d = dx * dx + dy * dy;

		if (d > far)
			far = d;
	}
	nub.dead = sqrtf(far) + 4.0f;
	nub.dead = nub.dead < 8.0f ? 8.0f : nub.dead > 40.0f ? 40.0f : nub.dead;
	/* a little short of the furthest seen, so full scale is reached without
	pressing as hard as the measuring did */
	nub.reach[0] = nub.rest[0] - nub.low[0] - 2.0f;
	nub.reach[1] = nub.high[0] - nub.rest[0] - 2.0f;
	nub.reach[2] = nub.rest[1] - nub.low[1] - 2.0f;
	nub.reach[3] = nub.high[1] - nub.rest[1] - 2.0f;
}

static void nub_save(void)
{
	FILE *file = fopen(NUB_PATH, "w");

	if (!file)
	{
		snprintf(nub.message, sizeof(nub.message), "couldn't write %s", NUB_PATH);
		return;
	}
	fprintf(file, "rest %.1f %.1f\ndead %.1f\nreach %.1f %.1f %.1f %.1f\n", (double)nub.rest[0], (double)nub.rest[1],
		(double)nub.dead, (double)nub.reach[0], (double)nub.reach[1], (double)nub.reach[2], (double)nub.reach[3]);
	fclose(file);
	snprintf(nub.message, sizeof(nub.message), "saved: the game uses it from this launch");
}

/* one pass of the menu's loop on the NUB page: the pad's fresh presses */
static void nub_page(SceCtrlData const *pad, unsigned int pressed)
{
	int i;

	if (nub.step == NUB_REST || nub.step == NUB_RETURN)
	{
		if (nub.count < (nub.step == NUB_REST ? NUB_SAMPLES / 2 : NUB_SAMPLES))
		{
			nub.x[nub.count] = pad->Lx;
			nub.y[nub.count] = pad->Ly;
			nub.count++;
		}
	}
	else if (nub.step == NUB_EDGE)
	{
		unsigned char now[2] = { pad->Lx, pad->Ly };

		for (i = 0; i < 2; i++)
		{
			if (now[i] < nub.low[i])
				nub.low[i] = now[i];
			if (now[i] > nub.high[i])
				nub.high[i] = now[i];
		}
	}
	if (pressed & PSP_CTRL_CIRCLE)
	{
		if (nub.step == NUB_IDLE)
		{
			sceIoRemove(NUB_PATH);
			snprintf(nub.message, sizeof(nub.message), "removed: the nub maps as it always has");
			nub_read_saved();
		}
		else
			snprintf(nub.message, sizeof(nub.message), "left as it was");
		nub.step = NUB_IDLE;
		return;
	}
	if (!(pressed & PSP_CTRL_CROSS))
		return;
	nub.message[0] = 0;
	switch (nub.step)
	{
	case NUB_IDLE:
		nub.count = 0;
		nub.step = NUB_REST;
		break;
	case NUB_REST:
		if (nub.count < NUB_SETTLE)
			return;
		nub.first = nub.count;
		nub.low[0] = nub.low[1] = 255;
		nub.high[0] = nub.high[1] = 0;
		nub.step = NUB_EDGE;
		break;
	case NUB_EDGE:
		/* the edge reached each way, well past any dead radius */
		if (nub.high[0] - nub.low[0] < 120 || nub.high[1] - nub.low[1] < 120)
		{
			snprintf(nub.message, sizeof(nub.message), "roll it further: all the way round its edge");
			return;
		}
		nub.step = NUB_RETURN;
		/* the second let-go's samples follow the first's */
		break;
	case NUB_RETURN:
		if (nub.count < nub.first + NUB_SETTLE)
			return;
		nub_work_out();
		nub.step = NUB_DONE;
		break;
	default:
		nub_save();
		nub_read_saved();
		nub.step = NUB_IDLE;
		break;
	}
}

static void nub_draw(SceCtrlData const *pad)
{
	static const char *const steps[] = {
		"cross: measure the nub   circle: remove nub.txt",
		"1/3  let go of the nub, wait a second, then cross",
		"2/3  roll the nub round its edge a few times, then cross",
		"3/3  let go again, wait a second, then cross",
		"cross: save   circle: leave it as it was",
	};

	pspDebugScreenPrintf(" nub now: x %3d  y %3d   (rest is near 128; 0 to 255 each way)\n\n", pad->Lx, pad->Ly);
	pspDebugScreenSetTextColor(0xFF00FFFF);
	pspDebugScreenPrintf(" %-66s\n\n", steps[nub.step]);
	pspDebugScreenSetTextColor(0xFFFFFFFF);
	if (nub.step == NUB_REST || nub.step == NUB_RETURN)
		pspDebugScreenPrintf(" samples: %d\n", nub.step == NUB_RETURN ? nub.count - nub.first : nub.count);
	if (nub.step == NUB_EDGE)
		pspDebugScreenPrintf(" seen: x %d to %d, y %d to %d\n", nub.low[0], nub.high[0], nub.low[1], nub.high[1]);
	if (nub.step == NUB_DONE)
		pspDebugScreenPrintf(" rest %.1f %.1f, dead %.1f\n reach left %.0f, right %.0f, up %.0f, down %.0f\n",
			(double)nub.rest[0], (double)nub.rest[1], (double)nub.dead, (double)nub.reach[0], (double)nub.reach[1],
			(double)nub.reach[2], (double)nub.reach[3]);
	pspDebugScreenSetTextColor(0xFFAAAAAA);
	pspDebugScreenPrintf("\n nub.txt: %-.58s\n", nub.saved);
	pspDebugScreenPrintf(" %-66s\n", nub.message);
	pspDebugScreenPrintf("\n the game maps the nub's measured range onto Halo's own dead zone\n and full travel, so its aim curve and turn speed-up work as tuned\n");
}

static void toggle(int i)
{
	int j;

	if (preset_label[0] && !strchr(preset_label, '+') && strlen(preset_label) + 1 < sizeof(preset_label))
		strcat(preset_label, "+");

	if (options[i].group)
	{
		int was = options[i].on;

		for (j = 0; j < OPTION_COUNT; j++)
			if (options[j].group == options[i].group)
				options[j].on = 0;
		options[i].on = !was;
	}
	else
		options[i].on = !options[i].on;
}

void psp_menu(int build)
{
	SceCtrlData pad;
	unsigned int last = 0;
	int page = 0, cursor = 0, map;
	int done = 0, repeat = 0;

	if (file_exists(DATA_ROOT "/input.txt") || file_exists(DATA_ROOT "/nomenu.txt"))
	{
		platform_log("front door: a scripted run (input.txt or nomenu.txt): init.txt as it is");
		return;
	}
	/* held as the game starts, L would also count as the menu's first
	press (a page back): the held buttons are the starting point */
	last = buttons_held();
	if (last & PSP_CTRL_LTRIGGER)
		platform_log("front door: the launch menu (L held)");
	else if (file_exists(DATA_ROOT "/menu.txt") || file_exists(DATA_ROOT "/menudump.txt"))
		platform_log("front door: the launch menu (menu.txt or menudump.txt)");
	else
	{
		front_door_main_menu = 1;
		platform_log("front door: the main menu (L held at start-up or " DATA_ROOT "/menu.txt: the launch menu)");
		/* keep.txt is the launch menu's last choices, so the main menu
		plays with them. A stick without one (fresh, or cleared) would
		otherwise get none of the words a build ships with: those are the
		first preset, which only the launch menu applies */
		if (!file_exists(DATA_ROOT "/keep.txt"))
		{
			load_presets();
			if (preset_count)
			{
				apply_preset(0);
				save_files(-1);
				platform_log("front door: no keep.txt; the first preset's words written to it (%s: %s)",
					presets[0].name, presets[0].words);
			}
			free(presets);
			presets = NULL;
			preset_count = 0;
		}
		return;
	}
	front_door_launch_menu = 1;
	int back = 0, dump_menu = -1;

	scan_maps();
	load_options();
	load_presets(); /* build 114 */
	nub_read_saved();
	map = current_map();
	cursor = map;
	pspDebugScreenInit();
	pspDebugScreenSetBackColor(0xFF201008);
	pspDebugScreenClear();
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
	sceCtrlReadBufferPositive(&pad, 1);
	while (!done)
	{
		int count = page == 0 ? map_count : page == 1 ? OPTION_COUNT : page == 2 ? preset_count : 0;
		unsigned int pressed;
		static int drawn_page = -1, drawn_cursor = -1, drawn_map = -1, drawn_preset = -2;
		static unsigned int drawn_options = 0;
		unsigned int options_now = 0;
		int k;

		/* build 71: redrawn only when something changed, at the start of a
		vertical blank. The debug screen draws straight into the picture on
		show, so clearing and redrawing it 16 times a second flickered
		(b70) */
		/* build 114: a hash of every option (b71-b113 packed them as bits of
		one int, and options past the 32nd wrapped onto the first: toggling
		one could leave the screen unchanged) */
		for (k = 0; k < OPTION_COUNT; k++)
			options_now = options_now * 31u + (unsigned int)options[k].on + 1u;
		/* the NUB page shows the nub live, so it is drawn every pass */
		if (page != drawn_page || cursor != drawn_cursor || map != drawn_map || options_now != drawn_options ||
			preset_applied != drawn_preset || page == 3)
		{
			/* build 92: drawn whole into the buffer not on show, then shown
			at the next vertical blank. b91's scrolling list redrew every
			line on each move straight into the picture on show, which takes
			longer than a blank: the D-pad flicker (b91) */
			back = !back;
			pspDebugScreenSetBase((u32 *)(MENU_VRAM + back * MENU_BUFFER_BYTES));
			pspDebugScreenClear();
			draw(page, cursor, map, build, &pad);
			sceKernelDcacheWritebackAll();
			sceDisplayWaitVblankStart();
			sceDisplaySetFrameBuf((void *)(MENU_VRAM + back * MENU_BUFFER_BYTES), 512, PSP_DISPLAY_PIXEL_FORMAT_8888,
				PSP_DISPLAY_SETBUF_NEXTFRAME);
			drawn_page = page;
			drawn_cursor = cursor;
			drawn_map = map;
			drawn_options = options_now;
			drawn_preset = preset_applied;
		}
		sceCtrlReadBufferPositive(&pad, 1);
		pressed = pad.Buttons & ~last;
		/* held up/down repeats after a moment */
		if ((pad.Buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN)) && (pad.Buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN)) == (last & (PSP_CTRL_UP | PSP_CTRL_DOWN)))
		{
			if (++repeat > 8)
			{
				pressed |= pad.Buttons & (PSP_CTRL_UP | PSP_CTRL_DOWN);
				repeat = 6;
			}
		}
		else
			repeat = 0;
		last = pad.Buttons;
		if ((pressed & PSP_CTRL_UP) && count)
			cursor = (cursor + count - 1) % count;
		if ((pressed & PSP_CTRL_DOWN) && count)
			cursor = (cursor + 1) % count;
		/* four pages, R forward and L back (triangle: forward) */
		if (pressed & (PSP_CTRL_RTRIGGER | PSP_CTRL_TRIANGLE))
		{
			page = (page + 1) % 4;
			cursor = page == 0 ? map : 0;
		}
		if (pressed & PSP_CTRL_LTRIGGER)
		{
			page = (page + 3) % 4;
			cursor = page == 0 ? map : 0;
		}
		if (page == 3)
			nub_page(&pad, pressed);
		if (pressed & (PSP_CTRL_CROSS | PSP_CTRL_LEFT | PSP_CTRL_RIGHT))
		{
			if (page == 0 && map_count)
				map = cursor;
			else if (page == 1)
				toggle(cursor);
			else if (page == 2 && preset_count && (pressed & PSP_CTRL_CROSS))
				apply_preset(cursor);
		}
		if (pressed & PSP_CTRL_START)
			done = 1;
		/* build 92: ms0:/halo/menudump.txt (emulator test): the options page
		with the cursor 15 down, the picture on show written raw (512 x 272,
		32-bit) to menudump.raw, and on */
		if (dump_menu < 0)
			dump_menu = file_exists(DATA_ROOT "/menudump.txt");
		if (dump_menu == 1)
		{
			page = 1;
			cursor = 15;
			dump_menu = 2;
			continue;
		}
		if (dump_menu == 2)
		{
			SceUID file = sceIoOpen(DATA_ROOT "/menudump.raw", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0666);

			if (file >= 0)
			{
				sceIoWrite(file, (void *)(MENU_VRAM + back * MENU_BUFFER_BYTES), MENU_BUFFER_BYTES);
				sceIoClose(file);
			}
			done = 1;
		}
		sceKernelDelayThread(60000);
	}
	save_files(map);
	free(presets); /* build 114 */
	presets = NULL;
	preset_count = 0;
	pspDebugScreenSetBase((u32 *)(MENU_VRAM + back * MENU_BUFFER_BYTES));
	pspDebugScreenClear();
	pspDebugScreenSetXY(0, 0);
	pspDebugScreenPrintf("Loading %s...\n", map_count ? maps[map].name : "the game");
}
