/* B41 host test.

psp/src/psp_sleepguard.c and halo_sleep.prx's handler (psp/sleep/main.c) are
compiled unchanged against a model of what they talk to: the firmware's
suspend query and sleep, the power callback (psp_platform.c's, mirrored here
and checked against the source by run.py), the GE, the Media Engine, the
texture and geometry caches and geinflight's records' room. Each scenario
runs one sleep path; at the moment the model's PSP goes to sleep it checks
what B41 needs: the game thread between frames, the GE idle, the ME stopped,
the volatile partition and the records' room given back.

The model is not the PSP. What the firmware does after a refused query, and
whether a held switch asks the same query, are only known from hardware
logs (docs/sleep_b41.md). This test shows the code does what it was designed
to on every path the design names.

Each scenario runs in its own process (the sources keep their state in
statics): b41_host_test <scenario>. run.py builds and runs them all. */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pspge.h"
#include "pspkernel.h"
#include "psppower.h"
#include "pspsysevent.h"

/* psp_sleepguard.c's */
extern int psp_sleep_frame;
void psp_sleepguard_eboot(const char *path);
void psp_sleepguard_start(void);
void psp_sleepguard_end(void);
int psp_sleepguard_quiet(void);
void psp_sleepguard_woken(void);
void psp_sleepguard_notice(void);
void psp_sleepguard_frame(void);
/* run.py defines B41_WAKING when the source has psp_sleepguard_waking */
#ifdef B41_WAKING
void psp_sleepguard_waking(void);
#endif
/* main.c's */
int module_start(SceSize args, void *argp);
int module_stop(SceSize args, void *argp);

#define MS 1000LL
#define SECONDS 1000000LL
#define FRAME_US (33 * MS)
#define NEVER (-1LL)

enum { EV_SWITCH, EV_FORCED, EV_REQUESTED, EV_ECHO };

static struct
{
	/* the scenario's */
	int keep_sleephold, keep_sleepallowed, module_present, register_fails;
	int request_result;
	long long request_delay, sleep_length, echo_after_wake, flick_after_request;
	long long ge_tail;
	int ge_hung, ge_busy_after_me_stop;
	/* the model's state */
	long long now, ge_busy_until;
	int lock, tex_held, geo_suspended, me_running, me_wake_pending, room_away;
	int mid_frame;
	PspSysEventHandler *handler;
	int loads;
	char load_path[256];
	/* what happened */
	int sleeps, unsafe_sleeps, old_path_suspends, held_presses, refused_presses, refused_requests, blocked_late;
	int requests, frames_in_flight, failures;
	long long last_sleep_at, last_wake_at, longest_stall;
	char unsafe_why[256];
} w;

struct event
{
	long long at;
	int kind, live;
};
static struct event events[32];
static int event_count;
static unsigned int query_words[4] = { 0x00000100, 0x11111111, 0x22222222, 0x33333333 };
static int verbose;

#define CHECK(condition, ...) \
	do { \
		if (!(condition)) \
		{ \
			w.failures++; \
			printf("  FAIL %s:%d: ", __FILE__, __LINE__); \
			printf(__VA_ARGS__); \
			printf("\n"); \
		} \
	} while (0)

static void schedule(long long at, int kind)
{
	if (event_count < (int)(sizeof(events) / sizeof(events[0])))
		events[event_count++] = (struct event){ at, kind, 1 };
}

static void fire(int kind);

/* time passes; the events due in it are carried out at their times */
static void advance(long long us)
{
	long long target = w.now + us;

	for (;;)
	{
		struct event *next = NULL;
		int index;

		for (index = 0; index < event_count; index++)
			if (events[index].live && events[index].at <= target && (!next || events[index].at < next->at))
				next = &events[index];
		if (!next)
			break;
		next->live = 0;
		if (next->at > w.now)
			w.now = next->at;
		fire(next->kind);
		if (w.now > target)
			target = w.now;
	}
	w.now = target;
}

/* ----- the SDK as psp_sleepguard.c and main.c call it */

int psp_keep(const char *word)
{
	return (!strcmp(word, "sleephold") && w.keep_sleephold) || (!strcmp(word, "sleepallowed") && w.keep_sleepallowed);
}

void platform_log(const char *format, ...)
{
	va_list list;

	if (!verbose)
		return;
	printf("    [%9.3f s] ", w.now / 1e6);
	va_start(list, format);
	vprintf(format, list);
	va_end(list);
	printf("\n");
}

void psp_log_flush(void) {}

SceUID sceKernelLoadModule(const char *path, int flags, void *option)
{
	(void)flags; (void)option;
	w.loads++;
	snprintf(w.load_path, sizeof(w.load_path), "%s", path);
	return w.module_present ? 0x0412 : (SceUID)0x8002012E;
}

int sceKernelStartModule(SceUID module, SceSize args, void *argp, int *status, void *option)
{
	(void)option;
	*status = module_start(args, argp);
	return module;
}

int sceKernelRegisterSysEventHandler(PspSysEventHandler *handler)
{
	if (w.register_fails)
		return (int)0x80020001;
	w.handler = handler;
	return 0;
}

int sceKernelUnregisterSysEventHandler(PspSysEventHandler *handler)
{
	if (w.handler == handler)
		w.handler = NULL;
	return 0;
}

int sceKernelDelayThread(SceUInt microseconds)
{
	advance(microseconds);
	return 0;
}

unsigned int sceKernelGetSystemTimeLow(void)
{
	return (unsigned int)w.now;
}

long long sceKernelGetSystemTimeWide(void)
{
	return w.now;
}

int scePowerLock(int unknown)
{
	(void)unknown;
	w.lock++;
	return 0;
}

int scePowerUnlock(int unknown)
{
	(void)unknown;
	if (w.lock > 0)
		w.lock--;
	return 0;
}

int scePowerRequestSuspend(void)
{
	w.requests++;
	if (w.request_result < 0)
		return w.request_result;
	if (w.request_delay != NEVER)
		schedule(w.now + w.request_delay, EV_REQUESTED);
	if (w.flick_after_request != NEVER)
	{
		schedule(w.now + w.flick_after_request, EV_SWITCH);
		w.flick_after_request = NEVER;
	}
	return 0;
}

int sceGeDrawSync(int mode)
{
	(void)mode;
	return w.ge_hung || w.now < w.ge_busy_until;
}

/* ----- the game's own parts the sources call */

void psp_tex_suspend(void) { w.tex_held = 0; }
void psp_tex_resume(void) { w.tex_held = 1; } /* psp_tex.c: takes it only when it isn't held */
void psp_geo_suspend(void) { w.geo_suspended = 1; }
void psp_geo_resume(void) { w.geo_suspended = 0; }

void psp_me_sleep_stop(void)
{
	w.me_running = 0;
	if (w.ge_busy_after_me_stop)
		w.ge_hung = 1;
}

void psp_me_wake_request(void)
{
	w.me_wake_pending = 1;
}

void psp_ge_inflight_room_before_sleep(void) { w.room_away = 1; }
void psp_ge_inflight_room_after_sleep(void) { w.room_away = 0; }
static void psp_ge_inflight_room_suspend(void) { w.room_away = 1; }
static void psp_ge_inflight_room_resume(void) { w.room_away = 0; }

/* psp_platform.c's power_callback, call for call (run.py checks the order) */
static void power_callback(int flags)
{
	if ((flags & PSP_POWER_CB_SUSPENDING) && !psp_sleepguard_quiet())
	{
		w.old_path_suspends++;
		psp_me_sleep_stop();
		psp_ge_inflight_room_suspend();
		psp_geo_suspend();
		psp_tex_suspend();
	}
	if (flags & PSP_POWER_CB_RESUME_COMPLETE)
	{
#ifdef B41_WAKING
		psp_sleepguard_waking();
#endif
		psp_tex_resume();
		psp_geo_resume();
		psp_ge_inflight_room_resume();
		psp_me_wake_request();
		psp_sleepguard_woken();
	}
}

/* ----- the firmware */

static int query(void)
{
	int result = 0;

	if (!w.handler || !(w.handler->type_mask & 0x100))
		return 0;
	return w.handler->handler(0x100, "suspend query", query_words, &result);
}

static void go_to_sleep(const char *why)
{
	int ge_busy = sceGeDrawSync(1);

	power_callback(PSP_POWER_CB_SUSPENDING);
	/* what B41 needs at the moment of sleep */
	if (w.mid_frame || ge_busy || w.me_running || w.tex_held || !w.geo_suspended || !w.room_away)
	{
		w.unsafe_sleeps++;
		snprintf(w.unsafe_why, sizeof(w.unsafe_why), "%s:%s%s%s%s%s%s", why, w.mid_frame ? " mid-frame" : "",
			ge_busy ? " GE-busy" : "", w.me_running ? " ME-running" : "", w.tex_held ? " partition-held" : "",
			!w.geo_suspended ? " geo-not-suspended" : "", !w.room_away ? " room-not-away" : "");
	}
	w.sleeps++;
	w.last_sleep_at = w.now;
	if (verbose)
		printf("    [%9.3f s] -- the PSP sleeps (%s)\n", w.now / 1e6, why);
	w.now += w.sleep_length;
	w.last_wake_at = w.now;
	if (verbose)
		printf("    [%9.3f s] -- the PSP wakes\n", w.now / 1e6);
	power_callback(PSP_POWER_CB_RESUME_COMPLETE);
	if (w.echo_after_wake != NEVER)
	{
		schedule(w.now + w.echo_after_wake, EV_ECHO);
		w.echo_after_wake = NEVER;
	}
}

static void fire(int kind)
{
	switch (kind)
	{
	case EV_SWITCH: /* the switch flicked: held off by a lock, else asked */
		if (w.lock)
			w.held_presses++;
		else if (query() < 0)
			w.refused_presses++;
		else
			go_to_sleep("switch");
		break;
	case EV_FORCED: /* the firmware's own (a flat battery): no query */
		go_to_sleep("forced");
		break;
	case EV_REQUESTED: /* scePowerRequestSuspend's sleep */
		if (w.lock)
			w.blocked_late++;
		else if (query() < 0)
			w.refused_requests++;
		else
			go_to_sleep("requested");
		break;
	case EV_ECHO: /* a second query with the wake (a request queued before it) */
		if (!w.lock && query() >= 0)
			go_to_sleep("echo");
		break;
	}
}

/* ----- the game thread: psp_ge_frame_end's order */

static void frames(int count, int in_flight, int frame_open)
{
	while (count-- > 0)
	{
		long long start;
		int handed = 0;

		w.mid_frame = 1;
		advance(FRAME_US);
		w.mid_frame = 0;
		/* geinflight's frame end hands the frame over and goes on */
		if (frame_open && in_flight && !psp_sleep_frame)
		{
			w.frames_in_flight++;
			w.ge_busy_until = w.now + w.ge_tail;
			psp_sleepguard_notice();
			handed = 1;
		}
		/* psp_me_wake_poll: the ME back after a wake */
		if (w.me_wake_pending)
		{
			w.me_wake_pending = 0;
			w.me_running = 1;
		}
		if (!frame_open || handed)
		{
			psp_sleepguard_notice();
			continue;
		}
		/* frame_end_list, frame_end_show: the GE has the frame's last draws */
		w.ge_busy_until = w.now + w.ge_tail;
		start = w.now;
		{
			int sleeps = w.sleeps;
			long long stall;

			psp_sleepguard_frame();
			stall = w.now - start - (w.sleeps > sleeps ? w.sleep_length : 0);
			if (stall > w.longest_stall)
				w.longest_stall = stall;
		}
	}
}

static long long base;

static void setup(void)
{
	memset(&w, 0, sizeof(w));
	w.module_present = 1;
	w.request_delay = 30 * MS;
	w.sleep_length = 10 * SECONDS;
	w.echo_after_wake = NEVER;
	w.flick_after_request = NEVER;
	w.ge_tail = 4 * MS;
	w.tex_held = 1;
	w.me_running = 1;
	w.now = base;
	event_count = 0;
}

static void start(void)
{
	psp_sleepguard_eboot("ms0:/PSP/GAME/SIERRA3_SLEEPTEST/EBOOT.PBP");
	psp_sleepguard_start();
}

static void at(long long offset, int kind)
{
	schedule(base + offset, kind);
}

/* the state a game going on after any path should be in */
static void check_running(int expect_lock)
{
	frames(3, 1, 1); /* the ME back at a frame end */
	CHECK(w.tex_held, "the texture cache has its partition back");
	CHECK(!w.geo_suspended, "the geometry cache is back");
	CHECK(w.me_running, "the ME runs again");
	CHECK(!w.room_away, "the records' room is back");
	CHECK(!psp_sleepguard_quiet(), "the game isn't marked quiet");
	CHECK(w.lock == expect_lock, "the switch lock is %d (expected %d)", w.lock, expect_lock);
	{
		int before = w.frames_in_flight;

		frames(10, 1, 1);
		CHECK(w.frames_in_flight - before >= 9, "frames go in flight again (%d of 10)", w.frames_in_flight - before);
	}
	psp_sleepguard_end();
	CHECK(w.lock == 0, "the XMB has its switch back after the game");
}

static void scenario(const char *name)
{
	setup();
	if (!strcmp(name, "module-rejects"))
	{
		/* main.c's module_start: every path but a valid block answers -1 */
		static struct { unsigned int magic; unsigned int rest[10]; } good = { 0x48534c32, { 0 } }, bad = { 0x48534c31, { 0 } };
		unsigned int address;
		int result;

		CHECK(module_start(0, NULL) == -1, "no argument");
		address = (unsigned int)(size_t)&good;
		CHECK(module_start(2, &address) == -1, "a short argument");
		address = 0x08000000;
		CHECK(module_start(4, &address) == -1, "an address below user memory");
		address = 0x0A000000 - 8;
		CHECK(module_start(4, &address) == -1, "an address past the end of RAM");
		address = (unsigned int)(size_t)&bad;
		CHECK(module_start(4, &address) == -1, "another layout's magic");
		CHECK(w.handler == NULL, "no handler from a refused start");
		w.register_fails = 1;
		address = (unsigned int)(size_t)&good;
		CHECK(module_start(4, &address) == -1, "the firmware refusing the handler");
		CHECK(w.handler == NULL, "no handler after a refused register");
		w.register_fails = 0;
		CHECK((size_t)&good >= 0x08800000 && (size_t)&good < 0x0A000000, "run.py links this test at 0x08800000");
		result = module_start(4, &address);
		CHECK(result == 0 && w.handler, "a valid block: the handler in");
		CHECK(w.handler->handler(0x4000, "other", NULL, &result) == 0 && good.rest[0] == 0, "other events pass through");
		CHECK(query() < 0 && good.rest[0] == 1, "a query while not ready is refused and noted");
		good.rest[1] = 1; /* ready */
		CHECK(query() == 0, "a query while ready is let through");
		/* rest: wanted, ready, queries, refused, allowed, last_event, query_words[4] */
		CHECK(good.rest[2] == 2 && good.rest[3] == 1 && good.rest[4] == 1, "the queries are counted");
		CHECK(good.rest[5] == 0x100 && good.rest[6] == query_words[0] && good.rest[8] == query_words[2],
			"the query's words are kept for the log");
		module_stop(0, NULL);
		CHECK(w.handler == NULL, "module_stop takes the handler out");
		return;
	}
	if (!strcmp(name, "no-module") || !strcmp(name, "register-fails") || !strcmp(name, "sleephold"))
	{
		w.module_present = strcmp(name, "no-module") != 0;
		w.register_fails = !strcmp(name, "register-fails");
		w.keep_sleephold = !strcmp(name, "sleephold");
		at(1 * SECONDS, EV_SWITCH);
		start();
		CHECK(w.lock == 1, "the stopgap holds the switch");
		CHECK(!w.keep_sleephold || w.loads == 0, "sleephold loads no module");
		frames(100, 1, 1);
		CHECK(w.sleeps == 0 && w.held_presses == 1, "the press is held off (sleeps %d)", w.sleeps);
		CHECK(w.requests == 0, "nothing asked for");
		check_running(1);
		return;
	}
	if (!strcmp(name, "sleepallowed"))
	{
		/* b148's own path, kept for its tests: the harness must see B41's hazard */
		w.keep_sleepallowed = 1;
		at(1 * SECONDS, EV_SWITCH);
		start();
		CHECK(w.lock == 0 && w.loads == 0, "the switch as the firmware has it");
		frames(100, 1, 1);
		CHECK(w.sleeps == 1 && w.old_path_suspends == 1, "the old path sleeps");
		CHECK(w.unsafe_sleeps == 1, "this test sees the old path's hazard (%s)", w.unsafe_why);
		return;
	}
	/* from here halo_sleep.prx is in */
	if (!strcmp(name, "play") || !strcmp(name, "menu") || !strcmp(name, "ge-slow") || !strcmp(name, "clock-wrap"))
	{
		int in_flight = strcmp(name, "menu") != 0;

		if (!strcmp(name, "ge-slow"))
			w.ge_tail = 200 * MS;
		at(1 * SECONDS + 7 * MS, EV_SWITCH);
		start();
		CHECK(w.lock == 0 && w.handler, "the module in, the switch free");
		CHECK(strstr(w.load_path, "ms0:/PSP/GAME/SIERRA3_SLEEPTEST/halo_sleep.prx") != NULL, "the module is loaded from beside the EBOOT (%s)", w.load_path);
		frames(400, in_flight, 1);
		CHECK(w.sleeps == 1 && w.requests == 1, "one sleep (sleeps %d, requests %d)", w.sleeps, w.requests);
		CHECK(w.refused_presses == 1, "the press was put off once");
		CHECK(w.unsafe_sleeps == 0, "the sleep was quiet (%s)", w.unsafe_why);
		CHECK(w.old_path_suspends == 0, "the callback's suspend half had nothing to do");
		CHECK(w.last_sleep_at - (base + 1 * SECONDS) < 300 * MS, "the sleep came soon after the press (%lld ms)", (w.last_sleep_at - base) / MS - 1000);
		CHECK(w.longest_stall < 600 * MS, "no frame end stalled long (%lld ms)", w.longest_stall / MS);
		CHECK(psp_sleep_frame == 0, "no frame left out of flight");
		check_running(0);
		return;
	}
	if (!strcmp(name, "load"))
	{
		at(500 * MS, EV_SWITCH);
		start();
		frames(90, 1, 0); /* a level loading: no frame open for 3 s */
		CHECK(w.sleeps == 0, "no sleep during the load");
		frames(60, 1, 1);
		CHECK(w.sleeps == 1 && w.unsafe_sleeps == 0, "the sleep after the load, quiet (%s)", w.unsafe_why);
		CHECK(w.last_sleep_at >= base + 90 * FRAME_US, "the sleep waited for the load");
		check_running(0);
		return;
	}
	if (!strcmp(name, "ge-hung") || !strcmp(name, "ge-busy-after-me"))
	{
		at(1 * SECONDS, EV_SWITCH);
		/* the next press after the stopgap: the 60 tries take 30 s */
		at(!strcmp(name, "ge-hung") ? 45 * SECONDS : 10 * SECONDS, EV_SWITCH);
		start();
		frames(31, 1, 1);
		if (!strcmp(name, "ge-hung"))
			w.ge_hung = 1;
		else
			w.ge_busy_after_me_stop = 1;
		frames(500, 1, 1);
		w.ge_hung = 0;
		CHECK(w.sleeps == 0 && w.requests == 0, "no sleep with the GE busy");
		CHECK(w.held_presses == 1, "the next press held off by the stopgap");
		CHECK(w.longest_stall <= 1100 * MS, "each frame end waits a bounded time (%lld ms)", w.longest_stall / MS);
		check_running(1);
		return;
	}
	if (!strcmp(name, "request-refused") || !strcmp(name, "no-sleep-comes") || !strcmp(name, "late-sleep"))
	{
		if (!strcmp(name, "request-refused"))
			w.request_result = (int)0x80000023;
		else
			w.request_delay = !strcmp(name, "no-sleep-comes") ? NEVER : 12 * SECONDS;
		at(1 * SECONDS, EV_SWITCH);
		start();
		frames(500, 1, 1);
		CHECK(w.sleeps == 0 && w.requests == 1, "asked once, no sleep");
		CHECK(!strcmp(name, "late-sleep") ? w.blocked_late == 1 : 1, "the late sleep held off by the stopgap");
		CHECK(w.longest_stall <= 10600 * MS, "the wait is bounded (%lld ms)", w.longest_stall / MS);
		check_running(1);
		return;
	}
	if (!strcmp(name, "wake-echo") || !strcmp(name, "twice"))
	{
		w.echo_after_wake = !strcmp(name, "wake-echo") ? 200 * MS : NEVER;
		at(1 * SECONDS, EV_SWITCH);
		at(16 * SECONDS, EV_SWITCH);
		start();
		frames(1000, 1, 1);
		CHECK(w.sleeps == 2 && w.requests == 2, "two presses, two sleeps (sleeps %d, requests %d)", w.sleeps, w.requests);
		CHECK(w.unsafe_sleeps == 0, "both quiet (%s)", w.unsafe_why);
		check_running(0);
		return;
	}
	if (!strcmp(name, "press-while-asking"))
	{
		/* a second flick after the game has asked and before the sleep: the
		firmware's request still on its way comes after the wake */
		w.request_delay = 40 * MS;
		w.flick_after_request = 10 * MS;
		at(1 * SECONDS, EV_SWITCH);
		start();
		frames(500, 1, 1);
		CHECK(w.unsafe_sleeps == 0, "every sleep quiet (%s)", w.unsafe_why);
		CHECK(w.sleeps == 1, "one sleep for the two flicks (sleeps %d)", w.sleeps);
		check_running(0);
		return;
	}
	if (!strcmp(name, "forced") || !strcmp(name, "forced-after-press"))
	{
		/* the firmware's own sleep, no query: b148's path, with no press left after */
		at(1 * SECONDS, !strcmp(name, "forced") ? EV_FORCED : EV_SWITCH);
		if (strcmp(name, "forced"))
			at(1 * SECONDS + 1 * MS, EV_FORCED);
		start();
		frames(500, 1, 1);
		CHECK(w.sleeps == 1 && w.old_path_suspends == 1, "the old path's one sleep");
		CHECK(w.requests == 0, "no second sleep asked for after the wake");
		check_running(0);
		return;
	}
	if (!strcmp(name, "five"))
	{
		int index;

		for (index = 0; index < 5; index++)
			at((1 + index * 15) * SECONDS + index * 3 * MS, EV_SWITCH);
		start();
		frames(3000, 1, 1);
		CHECK(w.sleeps == 5 && w.requests == 5 && w.unsafe_sleeps == 0, "five quiet sleeps (sleeps %d, unsafe %d)", w.sleeps, w.unsafe_sleeps);
		check_running(0);
		return;
	}
	printf("  unknown scenario %s\n", name);
	w.failures++;
}

int main(int argc, char **argv)
{
	if (argc < 2)
	{
		printf("usage: b41_host_test <scenario> [-v] [--clock-wrap]\n");
		return 2;
	}
	verbose = argc > 2 && !strcmp(argv[2], "-v");
	/* the 32-bit clock wraps 0.5 s after the start */
	base = !strcmp(argv[1], "clock-wrap") ? 4294967296LL - 500 * MS : 5 * SECONDS;
	scenario(argv[1]);
	printf("%s %s\n", w.failures ? "FAIL" : "ok  ", argv[1]);
	return w.failures != 0;
}
