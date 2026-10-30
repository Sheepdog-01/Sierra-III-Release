/* The sleep switch while the game runs (B41).

B41: on b148, a plain sleep from the power switch crashed the PSP-1000 as
it went to sleep, and the crash landed while the firmware was writing its
own settings, which came back corrupted on the next boot. The game readied
itself for the sleep in its power callback (psp_platform.c), which runs on
a user thread the firmware doesn't wait for: the kernel's sleep could go
on with the GE still drawing from the volatile partition the texture cache
had just given back, the Media Engine mid-job and the game thread
mid-frame.

The fix: halo_sleep.prx (psp/sleep) refuses the firmware's suspend query
until the game is ready, and notes the refusal in this file's block. The
game thread sees it at a frame's end (psp_ge.c):
- an in-flight frame's end (psp_sleepguard_notice) only marks the next
  frame not in flight (psp_sleep_frame), so that frame ends as the menus'
  frames do: its list finished and the GE given all of it;
- at that frame's end (psp_sleepguard_frame) the GE is polled idle (bounded),
  the ME stopped, the records' room, the geometry and animation caches and
  the texture cache's volatile partition given back, and only then is the
  game marked ready and the sleep asked for (scePowerRequestSuspend). The
  power callback's suspend half then has nothing left to do; its wake half
  runs as before and lets the waiting game thread go on.
Nothing here waits unbounded. A GE that isn't idle in 500 ms puts the sleep
off to a later frame; after SLEEP_TRIES such frames, or a requested sleep that
doesn't come in 10 s, the switch is held off for the rest of the game (the
2026-10-28 stopgap, scePowerLock), which also holds back a late sleep.

Without the module (not on the stick, a different build's, or the firmware
refused its handler) the stopgap stays from the start. Keep word
"sleephold" keeps the stopgap with the module there; "sleepallowed" leaves
the switch as the firmware has it, for the old path's own tests.

The two calls sit in psp_ge.c's frame end (psp_ge_frame_end); its release
profile (PSP_PGO) must be counted again with them in. */

#include <pspkernel.h>
#include <psppower.h>
#include <pspge.h>
#include <stdio.h>
#include <string.h>

extern int psp_keep(const char *word);
extern void platform_log(const char *format, ...);
extern void psp_log_flush(void);
void psp_tex_suspend(void);
void psp_tex_resume(void);
void psp_geo_suspend(void);
void psp_geo_resume(void);
void psp_me_sleep_stop(void);
void psp_me_wake_request(void);
void psp_ge_inflight_room_before_sleep(void);
void psp_ge_inflight_room_after_sleep(void);

/* halo_sleep.prx's half (psp/sleep/main.c): the same layout and magic */
struct halo_sleep_block
{
	unsigned int magic;
	volatile unsigned int wanted;
	volatile unsigned int ready;
	volatile unsigned int queries, refused, allowed;
	volatile unsigned int last_event;
	volatile unsigned int query_words[4]; /* the last query's first words, for the log */
};
#define HALO_SLEEP_MAGIC 0x48534c32 /* "HSL2" */

#define SLEEP_TRIES 60            /* frames a sleep may be put off for a busy GE */
#define GE_IDLE_MS 500
#define WAKE_WAIT_MS 10000
#define WAKE_QUIET_MS 1000        /* refusals just after a wake are the wake's own */

/* psp_ge.c's: the frame after a switch press isn't in flight */
int psp_sleep_frame = 0;

static struct halo_sleep_block block __attribute__((aligned(64)));
static int held;          /* the stopgap's scePowerLock */
static int deferred;      /* halo_sleep.prx's handler is in: sleeps wait for the frame end */
static int tries;
static volatile int quiet;   /* the game thread has readied the sleep itself */
static volatile int woken;   /* the power callback's wake half has run */
static unsigned long long woke_at; /* sceKernelGetSystemTimeWide: the 32-bit clock wraps in 72 minutes */
static char eboot[256];

/* main, before keep.txt is read: the module is beside the EBOOT */
void psp_sleepguard_eboot(const char *path)
{
	snprintf(eboot, sizeof(eboot), "%s", path ? path : "ms0:/PSP/GAME/halo/EBOOT.PBP");
}

static int module_load(void)
{
	char path[256];
	char *slash;
	SceUID module;
	int status = -1;
	unsigned int address = (unsigned int)&block;

	snprintf(path, sizeof(path), "%s", eboot[0] ? eboot : "ms0:/PSP/GAME/halo/EBOOT.PBP");
	slash = strrchr(path, '/');
	snprintf(slash ? slash + 1 : path, sizeof(path) - (slash ? (size_t)(slash + 1 - path) : 0), "halo_sleep.prx");
	memset(&block, 0, sizeof(block));
	block.magic = HALO_SLEEP_MAGIC;
	module = sceKernelLoadModule(path, 0, NULL);
	if (module >= 0)
		module = sceKernelStartModule(module, sizeof(address), &address, &status, NULL);
	/* the module answers 0 only with its handler in and this block's layout */
	platform_log("power: %s: %s (0x%08x, its handler %s)", path, module >= 0 ? "started" : "not loaded", (unsigned)module,
		module >= 0 && status == 0 ? "in" : "not in");
	return module >= 0 && status == 0;
}

static void hold(const char *why)
{
	int result;

	if (held)
		return;
	result = scePowerLock(0);
	held = result >= 0;
	platform_log("power: the sleep switch held off while the game runs (%s): %08x", why, (unsigned int)result);
}

/* once, after keep.txt is read */
void psp_sleepguard_start(void)
{
	if (psp_keep("sleepallowed"))
	{
		platform_log("power: the sleep switch left as the firmware has it (sleepallowed); a sleep may crash the PSP (B41)");
		return;
	}
	if (psp_keep("sleephold"))
	{
		hold("keep word sleephold");
		return;
	}
	deferred = module_load();
	if (deferred)
		platform_log("power: the sleep switch works; each sleep waits for a quiet frame end (halo_sleep.prx, B41)");
	else
		hold("B41's stopgap: halo_sleep.prx isn't in");
}

/* every exit: the XMB has its switch back */
void psp_sleepguard_end(void)
{
	if (!held)
		return;
	held = 0;
	scePowerUnlock(0);
}

/* the power callback's suspend half: nothing to do when the game thread
has readied the sleep itself */
int psp_sleepguard_quiet(void)
{
	return quiet;
}

/* the power callback's wake half, first: the game stops being ready before
anything is taken back. Until the game thread runs again a query would
otherwise be let through (a request queued behind a second flick), and the
PSP would sleep again with the partition already taken back (B41's hazard;
sierra-iii/host-test, press-while-asking) */
void psp_sleepguard_waking(void)
{
	block.ready = 0;
	quiet = 0;
}

/* the power callback's wake half, done. No press is left behind: a sleep
this file didn't ready (one the firmware didn't hold back for the refusal)
answered it, and a refusal since the wake is the wake's own */
void psp_sleepguard_woken(void)
{
	block.wanted = 0;
	woke_at = sceKernelGetSystemTimeWide();
	woken = 1;
}

static int wanted(void)
{
	if (!deferred || held || !block.wanted)
		return 0;
	/* the refusal of a query that came with the wake itself (a request
	queued before the sleep) is no press */
	if (woke_at && sceKernelGetSystemTimeWide() - woke_at < WAKE_QUIET_MS * 1000ull)
	{
		block.wanted = 0;
		return 0;
	}
	return 1;
}

/* an in-flight frame's end (psp_ge.c): the next frame ends quiet */
void psp_sleepguard_notice(void)
{
	if (!psp_sleep_frame && wanted())
		psp_sleep_frame = 1;
}

static int ge_idle(void)
{
	unsigned int waited;

	for (waited = 0; sceGeDrawSync(1) != 0; waited++)
	{
		if (waited >= GE_IDLE_MS)
			return 0;
		sceKernelDelayThread(1000);
	}
	return 1;
}

static void wake_steps(void)
{
	psp_tex_resume();
	psp_geo_resume();
	psp_ge_inflight_room_after_sleep();
	psp_me_wake_request();
}

static void give_up(const char *why)
{
	block.wanted = 0;
	psp_sleep_frame = 0;
	hold(why);
}

/* a frame's end with its list finished (psp_ge.c, not in flight): a sleep
the module put off is carried out here */
void psp_sleepguard_frame(void)
{
	unsigned int asked;
	int result, waited;

	if (!wanted())
	{
		psp_sleep_frame = 0;
		return;
	}
	if (!ge_idle())
	{
		/* the GE still has work: a later frame, or the stopgap */
		psp_sleep_frame = 1;
		if (++tries >= SLEEP_TRIES)
			give_up("B41: the GE was never idle for a sleep");
		return;
	}
	tries = 0;
	asked = sceKernelGetSystemTimeLow();
	platform_log("sleep: the switch asked (queries %u, refused %u; query %08x %08x %08x %08x); quieting the ME and the GE",
		block.queries, block.refused, block.query_words[0], block.query_words[1], block.query_words[2],
		block.query_words[3]);
	psp_log_flush();
	/* the ME first: after its stop the main CPU runs its jobs; then the GE,
	which nothing gives work to now, drawn out again */
	psp_me_sleep_stop();
	if (!ge_idle())
	{
		wake_steps();
		give_up("B41: the GE wasn't idle after the ME stopped");
		return;
	}
	psp_ge_inflight_room_before_sleep();
	psp_geo_suspend();
	psp_tex_suspend();
	platform_log("sleep: quiet in %u us; asking for the sleep", sceKernelGetSystemTimeLow() - asked);
	psp_log_flush();
	/* ready only now, the log written: from here a query is let through,
	and nothing of the game's is writing to the stick. The press is taken
	first, so one refused from here on is a new one */
	block.wanted = 0;
	woken = 0;
	quiet = 1;
	block.ready = 1;
	result = scePowerRequestSuspend();
	/* the PSP sleeps while this waits; the callback's wake half ends it */
	for (waited = 0; result >= 0 && !woken && waited < WAKE_WAIT_MS / 10; waited++)
		sceKernelDelayThread(10000);
	block.ready = 0;
	if (result < 0 || !woken)
	{
		/* no sleep came: the switch held off first, while the callback
		still leaves the suspend to this thread, so a sleep still on its way
		waits for the XMB instead of landing mid-game; then the caches and
		the ME back as a wake has them */
		give_up(result < 0 ? "B41: the sleep request was refused" : "B41: no sleep came in 10 s");
		quiet = 0;
		wake_steps();
		platform_log("sleep: %s (%08x); the game goes on with the switch held off", result < 0 ? "refused" : "none came",
			(unsigned int)result);
		return;
	}
	quiet = 0;
	psp_sleep_frame = 0;
	psp_ge_inflight_room_after_sleep();
	platform_log("sleep: awake (queries %u, refused %u, allowed %u)", block.queries, block.refused, block.allowed);
}
