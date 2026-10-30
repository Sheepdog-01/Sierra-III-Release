/*
PSP_SDL.C

The few SDL3 functions the Linux platform layer calls (sdl_platform.c,
xinput_sdl.c, dsound_sdl.c, gl_functions.c), implemented directly on the
PSP's system calls. The PSP toolchain does ship SDL3, but linking it costs
2.4 MB (1.35 MB of code and a 1 MB display list for its renderer) out of
the PSP-1000's 24 MB, for a window, an OpenGL context and audio the
bring-up build doesn't use.

- No window or OpenGL: SDL_CreateWindow fails, so the renderer runs as
  the null renderer.
- One gamepad, read from the PSP's buttons and analog stick
  (SDL_GetGamepadButton/Axis), mapped one way in menus and another in play.
- No audio device: sound goes out through psp_dsound.c.
*/

#include <SDL3/SDL.h>
#include <pspctrl.h>
#include <pspiofilemgr.h>
#include <pspkernel.h>
#include <psprtc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "psp_paths.h"   /* ms0:/halo or ms0:/halomp, and the maps */

void platform_log(const char *format, ...);

#define PSP_GAMEPAD_ID 1
static int controller_ready = 0;
static int walk_test = 0;

/* scripted input, for tests with no hands (and in the emulator, which has
none): ms0:/halo/input.txt holds lines of "seconds buttons stick_x stick_y",
the buttons in hex as the PSP reports them (start 0008, select 0001,
triangle 1000, R 0200...), each held from its time until the next line's,
the times counted from the first time the game reads the controls */
#define MAXIMUM_SCRIPT_LINES 64
static struct { float seconds; unsigned int buttons; int x, y; } input_script[MAXIMUM_SCRIPT_LINES];
static int input_script_count = 0;
static SceInt64 input_script_start = 0;
static int input_script_game_time = -1;
int psp_keep(const char *name);
int psp_keep_loaded(void);

static void input_script_load(void)
{
	FILE *file = fopen(PSP_DATA_ROOT "/input.txt", "r");
	char line[128];

	if (!file)
		return;
	while (input_script_count < MAXIMUM_SCRIPT_LINES && fgets(line, sizeof(line), file))
	{
		float seconds;
		unsigned int buttons;
		int x = 128, y = 128;

		if (sscanf(line, "%f %x %d %d", &seconds, &buttons, &x, &y) >= 2)
		{
			input_script[input_script_count].seconds = seconds;
			input_script[input_script_count].buttons = buttons;
			input_script[input_script_count].x = x;
			input_script[input_script_count].y = y;
			input_script_count++;
		}
	}
	fclose(file);
	platform_log("input script: %d lines from input.txt", input_script_count);
}

static void input_script_apply(SceCtrlData *pad)
{
	float seconds;
	int index, current = -1;

	if (!input_script_count)
		return;
	/* "inputgametime": the times in game time (ticks / 30), as a route's
	are, so two builds whose frames cost differently still press each
	button on the same tick (an exactness pair needs that) */
	if (input_script_game_time < 0 && psp_keep_loaded())
		input_script_game_time = psp_keep("inputgametime") != 0;
	if (input_script_game_time > 0)
	{
		extern unsigned char game_time_initialized(void);
		extern long game_time_get(void);

		if (!game_time_initialized())
			return;
		seconds = game_time_get() / 30.0f;
	}
	else
	{
		if (!input_script_start)
			input_script_start = sceKernelGetSystemTimeWide();
		seconds = (sceKernelGetSystemTimeWide() - input_script_start) / 1e6f;
	}
	for (index = 0; index < input_script_count && input_script[index].seconds <= seconds; index++)
		current = index;
	if (current >= 0)
	{
		pad->Buttons |= input_script[current].buttons;
		pad->Lx = (unsigned char)input_script[current].x;
		pad->Ly = (unsigned char)input_script[current].y;
	}
}

static void controller_initialize(void)
{
	if (!controller_ready)
	{
		sceCtrlSetSamplingCycle(0);
		sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);
		/* a test with no hands: with ms0:/halo/walktest.txt present, forward
		(triangle) is held and the stick turns slowly right, so the
		heartbeat's player position shows the input reaching the game */
		{
			SceIoStat status;

			walk_test = sceIoGetstat(PSP_DATA_ROOT "/walktest.txt", &status) >= 0;
			if (walk_test)
				platform_log("walk test: holding forward, turning right (delete walktest.txt to stop)");
		}
		input_script_load();
		controller_ready = 1;
	}
}

/* ---------- core */

void SDL_SetMainReady(void) {}
bool SDL_Init(SDL_InitFlags flags) { (void)flags; controller_initialize(); return true; }
Uint64 SDL_GetTicks(void) { return (Uint64)(sceKernelGetSystemTimeWide() / 1000); }
static SDL_JoystickID gamepad_list[2];
/* the gamepad list (SDL_GetGamepads, below) is static: never freed */
void SDL_free(void *memory) { if (memory != (void *)gamepad_list) free(memory); }
const char *SDL_GetError(void) { return "not available on the PSP"; }
/* the settings' parser (port_config.c) compares names and "true"/"false" with it: a
stub's 0 would make every name match */
int SDL_strncasecmp(const char *a, const char *b, size_t length) { return strncasecmp(a, b, length); }
bool SDL_SetHint(const char *name, const char *value) { (void)name; (void)value; return true; }
SDL_ThreadID SDL_GetCurrentThreadID(void) { return (SDL_ThreadID)sceKernelGetThreadId(); }
char *SDL_GetClipboardText(void) { char *text = malloc(1); if (text) text[0] = 0; return text; }
bool SDL_SetClipboardText(const char *text) { (void)text; return false; }
bool SDL_PollEvent(SDL_Event *event) { (void)event; return false; }
/* no keyboard: a key named in the bindings is none (ChupathingyCE's xinput_sdl.c) */
SDL_Scancode SDL_GetScancodeFromName(const char *name) { (void)name; return SDL_SCANCODE_UNKNOWN; }
bool SDL_ShowSimpleMessageBox(SDL_MessageBoxFlags flags, const char *title, const char *message, SDL_Window *window)
{
	(void)flags; (void)window;
	platform_log("message box: %s: %s", title ? title : "", message ? message : "");
	return true;
}
bool SDL_ShowAndroidToast(const char *message, int duration, int gravity, int x, int y)
{
	(void)duration; (void)gravity; (void)x; (void)y;
	platform_log("toast: %s", message ? message : "");
	return true;
}

/* ---------- no window, no OpenGL */

SDL_Window *SDL_CreateWindow(const char *title, int w, int h, SDL_WindowFlags flags)
{ (void)title; (void)w; (void)h; (void)flags; return NULL; }
SDL_GLContext SDL_GL_CreateContext(SDL_Window *window) { (void)window; return NULL; }
bool SDL_GL_MakeCurrent(SDL_Window *window, SDL_GLContext context) { (void)window; (void)context; return false; }
bool SDL_GL_SetAttribute(SDL_GLAttr attribute, int value) { (void)attribute; (void)value; return false; }
bool SDL_GL_SetSwapInterval(int interval) { (void)interval; return false; }
bool SDL_GL_SwapWindow(SDL_Window *window) { (void)window; return false; }
SDL_FunctionPointer SDL_GL_GetProcAddress(const char *name) { (void)name; return NULL; }
bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h)
{ (void)window; if (w) *w = 480; if (h) *h = 272; return true; }
bool SDL_SetWindowRelativeMouseMode(SDL_Window *window, bool enabled) { (void)window; (void)enabled; return true; }

/* ---------- no audio device */

SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID device, const SDL_AudioSpec *spec,
	SDL_AudioStreamCallback callback, void *userdata)
{ (void)device; (void)spec; (void)callback; (void)userdata; return NULL; }
bool SDL_PutAudioStreamData(SDL_AudioStream *stream, const void *buffer, int length)
{ (void)stream; (void)buffer; (void)length; return false; }
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *stream) { (void)stream; return false; }

/* ---------- the PSP's controls as one gamepad

The PSP is one Xbox pad to the engine, and the engine's own profile settings
(button and stick presets, look sensitivity, invert) apply on top of it, so
every Halo layout works with this one physical layout
(docs/game_feature_survey.md, section 4.5). In play:
  nub                 -> right stick (look), calibrated if nub.txt says so
  triangle / cross    -> left stick up / down (move forward / back)
  square / circle     -> left stick left / right (strafe)
  R / L               -> right / left trigger (fire / grenade)
  d-pad up            -> A (jump)       d-pad down  -> B (melee)
  d-pad left          -> X: a tap reloads, a hold acts (vehicles, switches)
  d-pad right         -> Y (switch weapon)
  select, tapped      -> right-stick click (zoom)
  select + up         -> left shoulder (flashlight)
  select + down       -> left-stick click (crouch), toggled
  select + L          -> right shoulder (switch grenades)
  select + right      -> a screenshot; select + left -> the debug menu
  start               -> start
With "faceaim" (the launch menu's options page) the nub and the face buttons
trade places, as in the PSP's own shooters (face_aim, below):
  nub                 -> left stick (move and strafe)
  triangle / cross    -> right stick up / down (aim)
  square / circle     -> right stick left / right (aim)
In Halo's menus the buttons are the pad's own (cross A, circle B, square X,
triangle Y; the D-pad and the nub move), and select tapped alone is back
(menu_select_update). */

/* the last controls the game read, for the heartbeat (psp_platform.c) */
volatile unsigned int psp_pad_buttons, psp_pad_reads;
volatile int psp_pad_lx = 128, psp_pad_ly = 128;

static int pad_swallowed = 0; /* build 77: the debug menu has the pad */

/* ---------- crouch toggled, reload and action apart, the nub calibrated

Crouch: the engine passes the crouch button on only while the move stick is
short of 98% of its travel (player_control.c), the Xbox's own rule; the
face buttons always move at full travel, so crouch worked only standing
still. Select + down now toggles it, and the engine's port crouch path (the
keyboard's, as Halo PC crouches at any speed) lets it through at any speed.
"crouchhold": held, under the engine's rule, as before.

X: the engine acts on a tap (vehicles, switches) and reloads when there is
nothing to act on, so a reflexive reload could throw the player out of a
vehicle. Now a tap of d-pad left reloads (the engine's reload control) and a
hold past "actionholdN" ms (250 by default) acts, never reloading (the
port's action-only control); picking a weapon up takes the engine's own
swap hold on top. "actiontap": X as the engine reads it, as before.

The nub: see nub_calibrate below. */
static int crouch_toggle_on = -1, crouch_toggled = 0, crouch_chord_held = 0;
/* crouch toggles, reload taps, action holds, and the reloads and actions the
engine took (psp_input_port_*), for the heartbeat */
volatile unsigned int controls_events[5];
static int action_split_on = -1, action_down = 0, action_acting = 0, reload_pending = 0;
static unsigned int action_hold_us = 250000, action_down_at = 0;
/* the nub's rest point, its dead radius and its reach left, right, up and
down, in the PSP's units (0-255, rest near 128); nub_calibrated 0: the
mapping of builds 1-134 */
static int nub_calibrated = -1;
static float nub_rest_x = 128.0f, nub_rest_y = 128.0f, nub_dead = 24.0f, nub_reach[4] = { 127.0f, 127.0f, 127.0f, 127.0f };
/* the furthest the nub has gone each way, for the heartbeat (and for
choosing a calibration) */
volatile int psp_nub_seen[4] = { 128, 128, 128, 128 };

int psp_keep(const char *name);
int psp_keep_number(const char *prefix, int fallback);
static int in_menus(void);

/* ms0:/halo/nub.txt, "rest X Y", "dead D", "reach LEFT RIGHT UP DOWN" (in the
PSP's units), or the keep words "nubdeadN" and "nubreachN" (percent of a
half travel, 128) over the defaults; with neither, the nub maps as before */
static void nub_calibrate_load(void)
{
	FILE *file = fopen("ms0:/halo/nub.txt", "r");
	int dead = psp_keep_number("nubdead", -1), reach = psp_keep_number("nubreach", -1);
	char line[96];

	nub_calibrated = 0;
	if (file)
	{
		while (fgets(line, sizeof(line), file))
		{
			float a, b, c, d;

			if (sscanf(line, "rest %f %f", &a, &b) == 2)
				nub_rest_x = a, nub_rest_y = b, nub_calibrated = 1;
			else if (sscanf(line, "dead %f", &a) == 1)
				nub_dead = a, nub_calibrated = 1;
			else if (sscanf(line, "reach %f %f %f %f", &a, &b, &c, &d) == 4)
				nub_reach[0] = a, nub_reach[1] = b, nub_reach[2] = c, nub_reach[3] = d, nub_calibrated = 1;
		}
		fclose(file);
	}
	if (dead >= 0)
		nub_dead = dead * 1.28f, nub_calibrated = 1;
	if (reach > 0)
		nub_reach[0] = nub_reach[1] = nub_reach[2] = nub_reach[3] = reach * 1.28f, nub_calibrated = 1;
	if (nub_calibrated)
		platform_log("controls: the nub calibrated: rest %.0f %.0f, dead %.0f, reach %.0f %.0f %.0f %.0f (left right up down)",
			(double)nub_rest_x, (double)nub_rest_y, (double)nub_dead, (double)nub_reach[0], (double)nub_reach[1],
			(double)nub_reach[2], (double)nub_reach[3]);
}

/* the calibrated nub as the engine's right stick: a push just past the dead
radius lands just past the engine's own dead zone (9,000 of 32,767 an axis,
input_xbox.c), and the reach on full scale, so the engine's response curve,
its acceleration when pegged and its aim assist see the range they were
tuned for. The dead radius is round, so diagonals aren't cut */
static void nub_calibrated_axes(unsigned char lx, unsigned char ly, Sint16 *x, Sint16 *y)
{
	const float engine_dead = 9001.0f / 32767.0f;
	float dx = (float)lx - nub_rest_x, dy = (float)ly - nub_rest_y;
	float rx = dx < 0.0f ? nub_reach[0] : nub_reach[1], ry = dy < 0.0f ? nub_reach[2] : nub_reach[3];
	float nx = dx / (rx > 1.0f ? rx : 1.0f), ny = dy / (ry > 1.0f ? ry : 1.0f);
	float r = sqrtf(dx * dx + dy * dy), n = sqrtf(nx * nx + ny * ny), reach, amount, ax, ay;

	*x = *y = 0;
	if (r <= nub_dead || n <= 0.0f)
		return;
	/* the reach in this direction (its ellipse), and how far between the
	dead radius and it the nub is */
	reach = r / n;
	amount = reach > nub_dead ? (r - nub_dead) / (reach - nub_dead) : 1.0f;
	amount = amount > 1.0f ? 1.0f : amount;
	ax = nx / n * amount;
	ay = ny / n * amount;
	if (ax > 0.001f || ax < -0.001f)
		*x = (Sint16)((ax < 0.0f ? -1.0f : 1.0f) * (engine_dead + (1.0f - engine_dead) * fabsf(ax)) * 32767.0f);
	if (ay > 0.001f || ay < -0.001f)
		*y = (Sint16)((ay < 0.0f ? -1.0f : 1.0f) * (engine_dead + (1.0f - engine_dead) * fabsf(ay)) * 32767.0f);
}

/* select in the menus is the Xbox's back button, which closes the pause
menu (its handler resumes the game) and steps back through the others.
Taken on the press, select's chords would reach the menus too: select +
right's screenshot would close the pause menu in the frame before the shot,
select + left's debug menu would step back. So back is a tap, as zoom is in
play: sent on select's release when nothing else was pressed with it, and
the D-pad goes to nobody while select is held.
"menuselectpress": back on the press and the D-pad through, as before */
static int menu_select_press = -1, menu_select_held = 0, menu_select_used = 0, menu_back_live = 0;
static unsigned int menu_back_at = 0;

#define MENU_BACK_PULSE_US 150000u
#define PAD_FACE_AND_SHOULDERS (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_LTRIGGER | \
	PSP_CTRL_RTRIGGER | PSP_CTRL_TRIANGLE | PSP_CTRL_CIRCLE | PSP_CTRL_CROSS | PSP_CTRL_SQUARE | PSP_CTRL_START)

static void menu_select_update(unsigned int b, unsigned int now)
{
	if (menu_select_press < 0)
		menu_select_press = psp_keep("menuselectpress") != 0;
	if (pad_swallowed)
		menu_select_used = 1;
	if (b & PSP_CTRL_SELECT)
	{
		if (b & PAD_FACE_AND_SHOULDERS)
			menu_select_used = 1;
		menu_select_held = 1;
	}
	else if (menu_select_held)
	{
		/* (a tap in play is zoom: no back left waiting for the next menu) */
		if (!menu_select_used && in_menus())
		{
			menu_back_at = now;
			menu_back_live = 1;
		}
		menu_select_held = menu_select_used = 0;
	}
	if (menu_back_live && now - menu_back_at >= MENU_BACK_PULSE_US)
		menu_back_live = 0;
}

/* once a fresh read: the crouch toggle and X's tap or hold */
static void controls_update(const SceCtrlData *pad, unsigned int now)
{
	unsigned int b = pad->Buttons;
	int chord, x_down;

	if (crouch_toggle_on < 0)
	{
		int ms = psp_keep_number("actionhold", 180);

		crouch_toggle_on = !psp_keep("crouchhold");
		action_split_on = !psp_keep("actiontap");
		action_hold_us = (unsigned int)(ms < 50 ? 50 : ms > 2000 ? 2000 : ms) * 1000u;
		nub_calibrate_load();
	}
	if (pad->Lx < psp_nub_seen[0]) psp_nub_seen[0] = pad->Lx;
	if (pad->Lx > psp_nub_seen[1]) psp_nub_seen[1] = pad->Lx;
	if (pad->Ly < psp_nub_seen[2]) psp_nub_seen[2] = pad->Ly;
	if (pad->Ly > psp_nub_seen[3]) psp_nub_seen[3] = pad->Ly;
	menu_select_update(b, now);
	if (pad_swallowed || in_menus())
	{
		crouch_chord_held = 0;
		action_down = action_acting = 0;
		return;
	}
	chord = (b & (PSP_CTRL_SELECT | PSP_CTRL_DOWN)) == (PSP_CTRL_SELECT | PSP_CTRL_DOWN);
	if (chord && !crouch_chord_held && crouch_toggle_on)
	{
		crouch_toggled = !crouch_toggled;
		controls_events[0]++;
	}
	crouch_chord_held = chord;
	x_down = (b & PSP_CTRL_LEFT) && !(b & PSP_CTRL_SELECT);
	if (x_down && !action_down)
		action_down_at = now;
	if (x_down && !action_acting && now - action_down_at >= action_hold_us)
	{
		action_acting = 1;
		controls_events[2]++;
	}
	if (!x_down && action_down && !action_acting)
	{
		reload_pending = 1;
		controls_events[1]++;
	}
	if (!x_down)
		action_acting = 0;
	action_down = x_down;
}

/* the PSP glyph for each of the engine's button icons (hud_messaging.c and
ui_widget.c's: A, B, X, Y, black, white, left and right trigger, the D-pad's
four, start, back, the two stick clicks, then the two sticks), in play and in
the menus, which map the buttons differently (above). The sticks' icons
follow the profile's stick preset by themselves (the engine swaps them for
southpaw). The sheet is tools/psp_glyphs.py's, psp/glyphs/psp_glyphs.png;
the prompts are to draw from it (docs/game_feature_survey.md, section 4.7) */
#include "psp_glyphs.h"
#define PSP_BUTTON_ICONS 18
static const unsigned char glyphs_in_play[PSP_BUTTON_ICONS] =
{
	PSP_GLYPH_DPAD_UP, PSP_GLYPH_DPAD_DOWN, PSP_GLYPH_DPAD_LEFT, PSP_GLYPH_DPAD_RIGHT,
	PSP_GLYPH_SELECT_L, PSP_GLYPH_SELECT_UP, PSP_GLYPH_L, PSP_GLYPH_R,
	PSP_GLYPH_DPAD_UP, PSP_GLYPH_DPAD_DOWN, PSP_GLYPH_DPAD_LEFT, PSP_GLYPH_DPAD_RIGHT,
	PSP_GLYPH_START, PSP_GLYPH_SELECT, PSP_GLYPH_SELECT_DOWN, PSP_GLYPH_SELECT,
	PSP_GLYPH_FACE, PSP_GLYPH_NUB,
};
static const unsigned char glyphs_in_menus[PSP_BUTTON_ICONS] =
{
	PSP_GLYPH_CROSS, PSP_GLYPH_CIRCLE, PSP_GLYPH_SQUARE, PSP_GLYPH_TRIANGLE,
	PSP_GLYPH_SELECT_L, PSP_GLYPH_SELECT_UP, PSP_GLYPH_L, PSP_GLYPH_R,
	PSP_GLYPH_DPAD_UP, PSP_GLYPH_DPAD_DOWN, PSP_GLYPH_DPAD_LEFT, PSP_GLYPH_DPAD_RIGHT,
	PSP_GLYPH_START, PSP_GLYPH_SELECT, PSP_GLYPH_SELECT_DOWN, PSP_GLYPH_SELECT,
	PSP_GLYPH_NUB, PSP_GLYPH_NUB,
};

/* the glyph and its cell on the sheet (x, y, width, height in pixels); -1
for an icon the PSP has no glyph for, or with "xboxglyphs" (the Xbox's icons,
as the maps have them). The map is the one the pad is in (in_menus), since a
prompt drawn by the HUD over the pause menu, or a menu's text in play, means
the buttons that work there. "glyphmapfixed" takes the caller's: menus from
the menus' text, play from the HUD's.

The menus have no black, white, triggers, stick clicks or right stick: their
icons keep the play map's glyphs, as the menus name them only when they
describe play's controls (the profile's settings) */
int psp_button_glyph(int icon, int menus, unsigned short cell[4])
{
	static int xbox = -1, fixed = -1;
	int glyph;

	if (xbox < 0)
	{
		xbox = psp_keep("xboxglyphs");
		fixed = psp_keep("glyphmapfixed");
	}
	if (xbox || icon < 0 || icon >= PSP_BUTTON_ICONS)
		return -1;
	if (!fixed)
		menus = in_menus();
	glyph = (menus ? glyphs_in_menus : glyphs_in_play)[icon];
	/* faceaim: the move stick is the nub, the look stick the face buttons */
	if (!menus && icon >= PSP_BUTTON_ICONS - 2 && psp_keep("faceaim"))
		glyph = icon == PSP_BUTTON_ICONS - 2 ? PSP_GLYPH_NUB : PSP_GLYPH_FACE;
	if (cell)
		memcpy(cell, psp_glyph_cells[glyph], sizeof(psp_glyph_cells[glyph]));
	return glyph;
}

/* how much further the text moves on past a glyph than past the icon it
stands for, in the game's screen units: the glyph is drawn at its share of
the icon's height (psp_glyph_height) in its own shape (psp_ge.c, glyph_quad), so a wide one (Select + up)
takes more room than the Xbox's round button, and a glyph the menus draw
larger ("menuglyphN") more again */
float psp_ge_menu_glyph_zoom(int glyph, int menus);

float psp_button_glyph_extra(int glyph, float icon_width, float icon_height)
{
	float wide;

	if (glyph < 0 || glyph >= PSP_GLYPH_COUNT)
		return 0.0f;
	wide = icon_height * psp_glyph_height[glyph] / 100.0f * psp_ge_menu_glyph_zoom(glyph, in_menus()) *
		psp_glyph_cells[glyph][2] / psp_glyph_cells[glyph][3];
	return wide > icon_width ? wide - icon_width : 0.0f;
}

/* the engine's port controls (input_abstraction.c, psp_fixups.py
FIXES_CONTROLS): a reload tap, once; the action held, never reloading; the
crouch toggle let through at any speed */
int psp_input_port_reload(void)
{
	if (!action_split_on || !reload_pending)
		return 0;
	reload_pending = 0;
	controls_events[3]++;
	return 1;
}

int psp_input_port_action_only(void)
{
	/* counted once a hold: the engine asks only while the action bit is
	set, so it never sees the release between two holds */
	static unsigned int counted_hold = 0;
	int now = action_split_on > 0 && action_acting;

	if (now && counted_hold != controls_events[2])
	{
		counted_hold = controls_events[2];
		controls_events[4]++;
	}
	return now;
}

int psp_input_port_crouch(void)
{
	return crouch_toggle_on > 0 && crouch_toggled;
}

/* the profile's controller settings from keep words, until Halo's own menu
can set them (player_ui.c, set_local_player_controls_from_player_profile):
"buttonpresetN" (0-4: default, swap triggers, swap A and left trigger, swap
B and left trigger, swap B and right stick), "stickpresetN" (0-3: default,
southpaw, legacy, legacy southpaw), "looksensN" (1-10), "lookinvert" */
int psp_input_profile_words(void)
{
	return psp_keep_number("buttonpreset", -1) >= 0 || psp_keep_number("stickpreset", -1) >= 0 ||
		psp_keep_number("looksens", -1) >= 0 || psp_keep("lookinvert");
}

void psp_input_profile_overrides(unsigned char *button_preset, unsigned char *joystick_preset,
	unsigned char *look_sensitivity, unsigned char *invert_look)
{
	int n;

	if ((n = psp_keep_number("buttonpreset", -1)) >= 0 && n <= 4)
		*button_preset = (unsigned char)n;
	if ((n = psp_keep_number("stickpreset", -1)) >= 0 && n <= 3)
		*joystick_preset = (unsigned char)n;
	if ((n = psp_keep_number("looksens", -1)) >= 1 && n <= 10)
		*look_sensitivity = (unsigned char)n;
	if (psp_keep("lookinvert"))
		*invert_look = 1;
}

static SceCtrlData read_controls(void)
{
	/* one read serves every button and axis query of the same poll: the
	game asks ~20 times per controller read (build 19) */
	static SceCtrlData cached;
	static unsigned int cached_time;
	static int cached_valid = 0;
	unsigned int now = sceKernelGetSystemTimeLow();
	SceCtrlData pad;

	if (cached_valid && now - cached_time < 2000)
		return cached;
	controller_initialize();
	memset(&pad, 0, sizeof(pad));
	pad.Lx = pad.Ly = 128;
	sceCtrlPeekBufferPositive(&pad, 1);
	if (walk_test)
	{
		pad.Buttons |= PSP_CTRL_TRIANGLE;
		pad.Lx = 170;
	}
	input_script_apply(&pad);
	{
		/* build 77: select + left opens the debug menu (psp_debug.c; the
		level pass modes, select + left with "passes" since build 53, are
		in it now). While it is open the game sees an idle pad */
		extern int psp_debug_input(unsigned int buttons);

		pad_swallowed = psp_debug_input(pad.Buttons);
		if (pad_swallowed)
		{
			pad.Buttons = 0;
			pad.Lx = pad.Ly = 128;
		}
	}
	if (!pad_swallowed)
	{
		/* select + right: a screenshot of the next frame (build 34) */
		extern volatile int psp_screenshot_requested;
		static int shot_held = 0;
		int shot = (pad.Buttons & (PSP_CTRL_SELECT | PSP_CTRL_RIGHT)) == (PSP_CTRL_SELECT | PSP_CTRL_RIGHT);

		if (shot && !shot_held)
			psp_screenshot_requested = 1;
		shot_held = shot;
	}
	controls_update(&pad, now);
	psp_pad_buttons = pad.Buttons;
	psp_pad_lx = pad.Lx;
	psp_pad_ly = pad.Ly;
	psp_pad_reads++;
	cached = pad;
	cached_time = now;
	cached_valid = 1;
	return pad;
}

/* build 105: one fixed list instead of a malloc on every pad read. b104
run 1 ran the heap dry, the 8-byte malloc failed, and the game saw no
gamepad from then on: the controls died while the game kept running.
SDL_free (above) knows not to free it */
static SDL_JoystickID gamepad_list[2] = { PSP_GAMEPAD_ID, 0 };

SDL_JoystickID *SDL_GetGamepads(int *count)
{
	if (count) *count = 1;
	return gamepad_list;
}

SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id) { return id == PSP_GAMEPAD_ID ? (SDL_Gamepad *)&controller_ready : NULL; }
SDL_Gamepad *SDL_GetGamepadFromID(SDL_JoystickID id) { return SDL_OpenGamepad(id); }
SDL_GamepadType SDL_GetGamepadType(SDL_Gamepad *gamepad) { (void)gamepad; return SDL_GAMEPAD_TYPE_XBOXONE; }
bool SDL_RumbleGamepad(SDL_Gamepad *gamepad, Uint16 low, Uint16 high, Uint32 milliseconds)
{ (void)gamepad; (void)low; (void)high; (void)milliseconds; return false; }

/* menus and play want different controls (build 27): while the game's
menus are up (the main menu, the pause menu, dialogs) the buttons are the
pad's own (cross A, circle B, square X, triangle Y, the D-pad and the nub to
move); in play, the face buttons move, the nub looks, the D-pad is A, B, X
and Y (jump, melee, action/reload, switch weapon), R fires, L throws a
grenade, select tapped zooms, and select held shifts the D-pad and L (below) */
extern unsigned char ui_widgets_active(void);

static int in_menus(void)
{
	return ui_widgets_active() != 0;
}

bool SDL_GetGamepadButton(SDL_Gamepad *gamepad, SDL_GamepadButton button)
{
	unsigned int b = read_controls().Buttons;

	(void)gamepad;
	if (in_menus())
	{
		/* the D-pad with select held is select's chord's (menu_select_update) */
		unsigned int dpad = menu_select_press || !(b & PSP_CTRL_SELECT) ? b : 0;

		switch (button)
		{
		case SDL_GAMEPAD_BUTTON_SOUTH: return (b & PSP_CTRL_CROSS) != 0;
		case SDL_GAMEPAD_BUTTON_EAST: return (b & PSP_CTRL_CIRCLE) != 0;
		case SDL_GAMEPAD_BUTTON_WEST: return (b & PSP_CTRL_SQUARE) != 0;
		case SDL_GAMEPAD_BUTTON_NORTH: return (b & PSP_CTRL_TRIANGLE) != 0;
		case SDL_GAMEPAD_BUTTON_DPAD_UP: return (dpad & PSP_CTRL_UP) != 0;
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return (dpad & PSP_CTRL_DOWN) != 0;
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return (dpad & PSP_CTRL_LEFT) != 0;
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return (dpad & PSP_CTRL_RIGHT) != 0;
		case SDL_GAMEPAD_BUTTON_START: return (b & PSP_CTRL_START) != 0;
		case SDL_GAMEPAD_BUTTON_BACK:
			if (menu_select_press)
				return (b & PSP_CTRL_SELECT) != 0;
			return menu_back_live && sceKernelGetSystemTimeLow() - menu_back_at < MENU_BACK_PULSE_US;
		default: return false;
		}
	}
	{
		/* select is a shift: with it held, the D-pad gives the flashlight (up)
		and crouch (down), and L the grenade switch (build 77; was left); tapped
		alone, it zooms (build 29). Select + right takes a screenshot, select +
		left opens the debug menu (read_controls; builds 34, 77) */
		static int select_held = 0, select_used = 0;
		/* build 89: when the tap was, and whether one is live. b29-b88 kept
		only zoom_until (0 at start) and tested (int)(zoom_until - now) > 0,
		which turns true by itself once the 32-bit microsecond clock passes
		2^31 (35.8 minutes after the PSP booted) or 35.8 minutes after the
		last tap: a phantom zoom held until the next select tap (review) */
		static unsigned int zoom_at = 0;
		static int zoom_live = 0;
		unsigned int now = sceKernelGetSystemTimeLow();
		int shift = (b & PSP_CTRL_SELECT) != 0;

		/* build 77: select held for the debug menu isn't a zoom tap */
		if (pad_swallowed)
			select_used = 1;
		if (shift)
		{
			if (b & (PSP_CTRL_UP | PSP_CTRL_DOWN | PSP_CTRL_LEFT | PSP_CTRL_RIGHT | PSP_CTRL_LTRIGGER))
				select_used = 1;
			select_held = 1;
		}
		else if (select_held)
		{
			if (!select_used)
			{
				zoom_at = now;
				zoom_live = 1;
			}
			select_held = select_used = 0;
		}
		switch (button)
		{
		case SDL_GAMEPAD_BUTTON_SOUTH: return !shift && (b & PSP_CTRL_UP) != 0;
		case SDL_GAMEPAD_BUTTON_EAST: return !shift && (b & PSP_CTRL_DOWN) != 0;
		case SDL_GAMEPAD_BUTTON_WEST: return action_split_on > 0 ? action_acting : !shift && (b & PSP_CTRL_LEFT) != 0;
		case SDL_GAMEPAD_BUTTON_NORTH: return !shift && (b & PSP_CTRL_RIGHT) != 0;
		case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return shift && (b & PSP_CTRL_UP) != 0;
		case SDL_GAMEPAD_BUTTON_LEFT_STICK: return crouch_toggle_on > 0 ? crouch_toggled : shift && (b & PSP_CTRL_DOWN) != 0;
		/* build 77: the grenade switch is select + L (select + left opens
		the debug menu) */
		case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return shift && (b & PSP_CTRL_LTRIGGER) != 0;
		case SDL_GAMEPAD_BUTTON_START: return (b & PSP_CTRL_START) != 0;
		case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
			if (zoom_live && now - zoom_at >= 150000)
				zoom_live = 0;
			return zoom_live;
		default: return false;
		}
	}
}

static Sint16 stick(unsigned char value)
{
	int centred = (int)value - 128;

	if (centred > -24 && centred < 24)
		return 0;
	return (Sint16)(centred * 256 > 32767 ? 32767 : centred * 256 < -32768 ? -32768 : centred * 256);
}

/* ---------- "faceaim": the layout of the PSP's own shooters (Quake's ports,
Halo Revamped): the nub moves and strafes, the face buttons aim. A face
button starts the look at 35% of a full turn rate (past the engine's dead
zone, 9,000 of 32,767) and reaches full rate after a third of a second held,
so a tap nudges the aim and a hold turns; the profile's look sensitivity and
invert apply as before. Off unless keep.txt names it */
#define FACE_AIM_START 0.35f
#define FACE_AIM_RAMP_US 330000u

static Sint16 face_aim_axis(unsigned int buttons, unsigned int negative, unsigned int positive, unsigned int *since)
{
	unsigned int now = sceKernelGetSystemTimeLow();
	int direction = (buttons & negative) ? -1 : (buttons & positive) ? 1 : 0;
	float share;

	if (!direction)
	{
		*since = 0;
		return 0;
	}
	if (!*since)
		*since = now | 1u;
	share = FACE_AIM_START + (1.0f - FACE_AIM_START) * (float)(now - *since) / (float)FACE_AIM_RAMP_US;
	if (share > 1.0f)
		share = 1.0f;
	return (Sint16)((float)direction * share * 32767.0f);
}

/* the nub as a stick, calibrated if nub.txt says so */
static Sint16 nub_axis(const SceCtrlData *pad, int y_axis)
{
	if (nub_calibrated > 0)
	{
		Sint16 x, y;

		nub_calibrated_axes(pad->Lx, pad->Ly, &x, &y);
		return y_axis ? y : x;
	}
	return stick(y_axis ? pad->Ly : pad->Lx);
}

Sint16 SDL_GetGamepadAxis(SDL_Gamepad *gamepad, SDL_GamepadAxis axis)
{
	static unsigned int aim_x_since, aim_y_since;
	SceCtrlData pad = read_controls();

	(void)gamepad;
	if (in_menus())
	{
		/* the nub moves through the menus */
		switch (axis)
		{
		case SDL_GAMEPAD_AXIS_LEFTX: return stick(pad.Lx);
		case SDL_GAMEPAD_AXIS_LEFTY: return stick(pad.Ly);
		default: return 0;
		}
	}
	if (psp_keep("faceaim"))
		switch (axis)
		{
		case SDL_GAMEPAD_AXIS_LEFTX: return nub_axis(&pad, 0);
		case SDL_GAMEPAD_AXIS_LEFTY: return nub_axis(&pad, 1);
		case SDL_GAMEPAD_AXIS_RIGHTX:
			return face_aim_axis(pad.Buttons, PSP_CTRL_SQUARE, PSP_CTRL_CIRCLE, &aim_x_since);
		case SDL_GAMEPAD_AXIS_RIGHTY:
			return face_aim_axis(pad.Buttons, PSP_CTRL_TRIANGLE, PSP_CTRL_CROSS, &aim_y_since);
		default:
			break;
		}
	switch (axis)
	{
	case SDL_GAMEPAD_AXIS_LEFTX:
		return (pad.Buttons & PSP_CTRL_SQUARE) ? -32767 : (pad.Buttons & PSP_CTRL_CIRCLE) ? 32767 : 0;
	case SDL_GAMEPAD_AXIS_LEFTY:
		return (pad.Buttons & PSP_CTRL_TRIANGLE) ? -32767 : (pad.Buttons & PSP_CTRL_CROSS) ? 32767 : 0;
	case SDL_GAMEPAD_AXIS_RIGHTX:
	case SDL_GAMEPAD_AXIS_RIGHTY:
		if (nub_calibrated > 0)
		{
			Sint16 x, y;

			nub_calibrated_axes(pad.Lx, pad.Ly, &x, &y);
			return axis == SDL_GAMEPAD_AXIS_RIGHTX ? x : y;
		}
		return stick(axis == SDL_GAMEPAD_AXIS_RIGHTX ? pad.Lx : pad.Ly);
	/* select + L switches grenades, without throwing one (build 77) */
	case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return (pad.Buttons & PSP_CTRL_LTRIGGER) && !(pad.Buttons & PSP_CTRL_SELECT) ? 32767 : 0;
	case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return (pad.Buttons & PSP_CTRL_RTRIGGER) ? 32767 : 0;
	default: return 0;
	}
}
