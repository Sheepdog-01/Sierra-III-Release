/*
PSP_PLATFORM.C

What the PSP needs besides the Linux platform layer, which it otherwise
reuses as is (port/linux/src, built against the PSP toolchain's SDL3 and
pthreads):
- the program entry: module info, a fixed-size heap, exit handling, the
  log file, the data and save roots;
- no network in the campaign build: posix_net.c's sockets fail cleanly
  (the multiplayer build's are psp_net.c's), and internet play's helpers
  (posix_update.c, posix_upnp.c, p2p.c) report nothing.
*/

#include "platform.h"
#include "posix.h"

#include <pspkernel.h>
/* the build number in the log (bump with every package) */
#define PSP_BUILD_NUMBER 143
/* the engine base the build was made from, for the log */
#define PSP_ENGINE_BASE "ChupathingyCE"
/* build 77: the last heartbeat's budget, for the debug overlay (psp_debug.c) */
float psp_budget_last[7];
#include <psputils.h>
#include <psppower.h>
#include "psp_bench.h"
#include <pspdebug.h>
#include <pspdisplay.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include "psp_me.h" /* build 118: PSP_ME_WORK */
#define SDL_MAIN_HANDLED
#include <SDL3/SDL_main.h>

PSP_MODULE_INFO("halo", PSP_MODULE_USER, 0, 1);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
PSP_MAIN_THREAD_STACK_SIZE_KB(512);
/* fixed, so the heap ends below the game state and tag window
(psp_build.py sets it from the linked program's size) */
PSP_HEAP_SIZE_KB(PSP_HEAP_KB);

/* ms0:/halo, or the multiplayer build's ms0:/halomp (psp_paths.h) */
#include "psp_paths.h"
#include "psp_metrics.h"
#include "psp_lanetrace.h"
#define DATA_ROOT PSP_DATA_ROOT
/* in a folder of its own: the game's own files (debug.txt, stderr.txt,
gamestate.txt) are written next to each other in ms0:/halo, and a file's
size lives in its folder's directory entries, so the log stays out of
that folder's way (build 6-8's logs all stopped at the same line) */
#define LOG_FOLDER DATA_ROOT "/log"
#define LOG_PATH LOG_FOLDER "/log.txt"

int halo_main(void);
long psp_game_time_peek(void);

/* build 106: the garbage collector's counts (objects.c, psp_fixups.py) */
long psp_garbage_runs[3], psp_garbage_removed, psp_garbage_bipeds_removed;
/* build 107: the most lens flares a frame has submitted (rasterizer_lights.c, psp_fixups.py) */
long psp_lens_flare_peak;
/* the level's lens flare markers (rasterizer_lights.c, psp_fixups.py FIXES_LFS) */
unsigned long psp_lfs_frames, psp_lfs_markers, psp_lfs_kept, psp_lfs_hits, psp_lfs_skipped, psp_lfs_wrong;

/* ---------- logging (psp_fixups.py's changes call this) */

void platform_psp_log(const char *format, ...)
{
	char line[512];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	platform_log("%s", line);
}

/* ---------- the log: every line opened, appended and closed, so a crash
can't lose lines still waiting for the Memory Stick's directory entry to be
updated (xbox_kernel.c's platform_log calls this on the PSP) */

/* every line is also drawn on the screen, before it goes to the file: if the
system dies, the screen keeps the last lines for a photo, and if the screen
keeps going while the file stops, the Memory Stick is what hung. The text
sits at the top of VRAM (16-bit, 512 x 272), clear of the null renderer's
targets at its start (psp_fixups.py's d3d8_resources.c change) */
#define SCREEN_BASE ((void *)0x441B0000)
#define SCREEN_COLUMNS 68
#define SCREEN_ROWS 34

static SceUID log_lock = -1;
/* the descriptor log.txt last opened as (descriptors are handed out lowest
free first, so it counts the files open), and its last failure */
static volatile int log_descriptor = -1, log_failure = 0;
static int screen_ready = 0, screen_row = 0;

static void screen_row_text(const char *text, int length)
{
	char row[SCREEN_COLUMNS + 1];

	memset(row, ' ', SCREEN_COLUMNS);
	memcpy(row, text, length);
	row[SCREEN_COLUMNS] = 0;
	pspDebugScreenSetXY(0, screen_row);
	pspDebugScreenPuts(row);
	screen_row = (screen_row + 1) % SCREEN_ROWS;
	/* a marker on the next row shows where the newest line is */
	pspDebugScreenSetXY(0, screen_row);
	pspDebugScreenPuts("----------------------------------------------------------------    ");
}

/* once the GE shows its own frames the log's screen is never displayed, and
its VRAM becomes texture cache (psp_tex.c; build 21) */
static volatile int screen_stopped = 0;

/* build 104: for the crash handler: whether the log's screen is the one
shown (loading), and holding it still so later lines don't cover a report */
int psp_screen_log_showing(void)
{
	return !screen_stopped;
}

void psp_screen_log_freeze(void)
{
	screen_stopped = 1;
}

static void screen_write(const char *text);

/* build 104: a line for the screen only, taking no lock and touching no
file: for code that is waiting on a lock (psp_trace.c), where a log line
could wait on the same lock */
void psp_screen_note(const char *text)
{
	screen_write(text);
}

void psp_screen_log_stop(void)
{
	extern void psp_trace_stop(const char *why);

	screen_stopped = 1;
	/* build 96: the load trace ends with the loading (its lines are written
	at once, milliseconds each: during play they cost whole frames) */
	psp_trace_stop("the first frame on the screen");
	{
		extern void psp_crumbs_arm(void);

		psp_crumbs_arm();
	}
}

static void screen_write(const char *text)
{
	int length = (int)strlen(text);

	if (screen_stopped)
		return;
	if (!screen_ready)
	{
		pspDebugScreenInitEx(SCREEN_BASE, PSP_DISPLAY_PIXEL_FORMAT_565, 1);
		screen_ready = 1;
	}
	do
	{
		int part = length > SCREEN_COLUMNS ? SCREEN_COLUMNS : length;

		screen_row_text(text, part);
		text += part;
		length -= part;
	}
	while (length > 0);
}

/* once the game runs, log lines are batched in memory and written once per
heartbeat (build 19): opening and closing the log for every line cost the
game several milliseconds a line, from a thread that outranks it. Start-up
keeps writing each line at once, so a crash there still leaves the log;
later, the screen keeps the last lines */
/* build 102: 8 KB each for the batch and the flush's copy, as up to b97;
the blackbox's 32 KB pair (b98-b101 had it always, 48 KB off the game's
heap) comes from the system's free memory when the blackbox starts */
static char log_batch_small[8192], log_copy_small[8192];
static char *log_batch = log_batch_small, *log_copy = log_copy_small;
static int log_batch_size = sizeof(log_batch_small);
static int log_batch_length = 0, log_batch_dropped = 0;
static volatile int log_batching = 0;
/* a batch three quarters full wakes the heartbeat to write it, so the game
thread never waits on the stick for the log: a write and a close can take
over a second each on a busy stick, and the game's reads queue behind them.
Created with the heartbeat; "logflushgame": the game thread writes it, as
before */
static SceUID log_wake = -1;
static SceUID log_wake_thread = -1;

static void log_flush(void);
static int log_lock_take(void);
/* build 86: for psp_meaudio.c, around the Media Engine's start (a hang there
would otherwise lose the lines before it) */
void psp_log_flush(void)
{
	log_flush();
}

/* build 99: the blackbox batches from the first frame (b98 wrote each
traced line at once until 20 s in: b98's run 2 started very slowly), and
the game thread writes the batch at every frame's end, so a freeze loses at
most the frame it happened in */
static volatile int log_frame_flush = 0;

static void log_batch_grow(void);

void psp_log_batch_now(void)
{
	log_batch_grow();
	log_batching = 1;
	log_frame_flush = 1;
}

/* build 89: one flush at a time (the heartbeat's, the exit's, the ME
start's): they shared the static copy below and could write each other's
lines twice or garble them (review) */
static SceUID flush_lock = -1;
static volatile int exiting;
static void log_flush_body(void);

/* build 116 (R11): the log holds its writes while a checkpoint is written
and for a moment after (psp_fixups.py, psp_state_write), so the sound
reader has the Memory Stick first. b115 test 1 [M]: the heartbeat's write
right after a checkpoint took ~600 ms, the sound reader waited behind it,
and the sound dropped out for up to 0.8 s. A batch nearly full is written
anyway, and nothing is held while the game exits or after a crash. Keep
word "lognohold": as before */
static volatile unsigned long long log_hold_until = 0;
static unsigned long log_holds = 0, log_holds_reported = 0;
int psp_keep(const char *name);

void psp_log_hold(unsigned int milliseconds)
{
	log_hold_until = sceKernelGetSystemTimeWide() + (unsigned long long)milliseconds * 1000;
}

static int log_held(void)
{
	static int off = -1;

	if (!log_hold_until || exiting || log_batch_length >= log_batch_size * 7 / 8)
		return 0;
	if (sceKernelGetSystemTimeWide() >= log_hold_until)
	{
		log_hold_until = 0;
		return 0;
	}
	if (off < 0)
		off = psp_keep("lognohold");
	if (off)
		return 0;
	log_holds++;
	return 1;
}

static void log_flush(void)
{
	int flushing = 0;

	if (log_held())
		return;

	if (flush_lock >= 0)
	{
		SceUInt timeout = 100000;

		flushing = sceKernelWaitSema(flush_lock, 1, exiting ? &timeout : NULL) >= 0;
	}
	log_flush_body();
	if (flushing)
		sceKernelSignalSema(flush_lock, 1);
}

static void log_flush_body(void)
{
	char *copy = log_copy;
	int length, dropped;
	SceUID file;

	int locked = log_lock_take();

	length = log_batch_length;
	dropped = log_batch_dropped;
	memcpy(copy, log_batch, length);
	log_batch_length = log_batch_dropped = 0;
	if (locked)
		sceKernelSignalSema(log_lock, 1);
	if (!length && !dropped)
		return;
	file = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if (file >= 0)
	{
		sceIoWrite(file, copy, length);
		if (dropped)
		{
			char line[64];

			snprintf(line, sizeof(line), "(log: %d lines dropped, the batch was full)\n", dropped);
			sceIoWrite(file, line, strlen(line));
		}
		sceIoClose(file);
	}
}

/* build 102: the crash handler (psp_crash.c) sets this too: the crashed
thread may have held the log's locks */
void psp_log_exiting(void);

/* build 88: while the game exits (HOME), every wait for the log's lock is
limited to 100 ms: no b70-b87 run's log has the exit callback's lines, nor
was the learned-textures file ever written there, and a lock held by a thread
the system has stopped would explain both */
static volatile int exiting = 0;  /* (declared above log_flush) */

void psp_log_exiting(void)
{
	exiting = 1;
}
/* build 104: and in play, at most half a second: b103 run 1 hung
loading The Library with the game thread holding this lock while it waited
on another (psp_trace.c's handle table), and the heartbeat waited here
forever behind it. A wait that times out goes on without the lock (two
lines may then interleave on the screen) and is counted; the holder is
named in the heartbeat's report */
static volatile SceUID log_lock_holder = 0;
static volatile unsigned long log_lock_timeouts = 0;
static volatile SceUID log_lock_last_holder = 0;

static int log_lock_take(void)
{
	SceUInt timeout = exiting ? 100000 : 500000;
	int taken;

	if (log_lock < 0)
		return 0;
	taken = sceKernelWaitSema(log_lock, 1, &timeout) >= 0;
	if (taken)
		log_lock_holder = sceKernelGetThreadId();
	else
	{
		char note[80];

		log_lock_timeouts++;
		log_lock_last_holder = log_lock_holder;
		if (log_lock_timeouts <= 3)
		{
			extern const char *psp_thread_name(SceUID id, char *buffer, int size);
			char name[32];

			snprintf(note, sizeof(note), "LOCK: the log's lock waited out; held by %s", psp_thread_name(log_lock_holder, name, sizeof(name)));
			screen_write(note);
		}
	}
	return taken;
}

/* build 104: the lock waits that timed out, for the heartbeat */
void psp_lock_report(void)
{
	extern void psp_handles_lock_line(char *line, int size);
	char line[200];

	if (log_lock_timeouts)
	{
		SceKernelThreadInfo info;
		const char *name = "?";

		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (log_lock_last_holder && sceKernelReferThreadStatus(log_lock_last_holder, &info) >= 0)
			name = info.name;
		platform_log("locks: the log's lock waited out %lu times (last held by %s)", log_lock_timeouts, name);
	}
	psp_handles_lock_line(line, sizeof(line));
	if (line[0])
		platform_log("%s", line);
}

/* build 102: the blackbox's larger batch (32 KB and its copy), from the
user partition's free memory (not the game's heap); kept at 8 KB if there
is none */
static void log_batch_grow(void)
{
	SceUID block;
	char *big;

	if (log_batch_size > (int)sizeof(log_batch_small))
		return;
	block = sceKernelAllocPartitionMemory(2, "halo log batch", PSP_SMEM_High, 2 * 32768, NULL);
	if (block < 0)
	{
		platform_log("log: no free memory for the blackbox's batch (0x%08x); 8 KB", (unsigned)block);
		return;
	}
	big = sceKernelGetBlockHeadAddr(block);
	/* nothing may flush (it reads the copy) or add lines while they change */
	if (flush_lock >= 0)
		sceKernelWaitSema(flush_lock, 1, NULL);
	{
		int locked = log_lock_take();

		memcpy(big, log_batch, log_batch_length);
		log_batch = big;
		log_copy = big + 32768;
		log_batch_size = 32768;
		if (locked)
			sceKernelSignalSema(log_lock, 1);
	}
	if (flush_lock >= 0)
		sceKernelSignalSema(flush_lock, 1);
	platform_log("log: the blackbox's batch, 32 KB from free memory");
}

/* "logeachline": each line written at once until 20 s after launch, its
newline in a write of its own (the way to compare against) */
int psp_keep_loaded(void);

static int log_each_line(void)
{
	static int word = -1;

	if (word < 0 && psp_keep_loaded())
		word = psp_keep("logeachline") != 0;
	return word > 0;
}

/* a line and its newline, joined for one write; only touched under the
log's lock */
static char log_line_joined[640];

void psp_log_write(const char *text)
{
	SceUID file;
	int locked = log_lock_take();

	screen_write(text);
	if (log_batching)
	{
		int length = strlen(text);

		if (log_batch_length + length + 1 <= log_batch_size)
		{
			memcpy(log_batch + log_batch_length, text, length);
			log_batch_length += length;
			log_batch[log_batch_length++] = '\n';
		}
		else
			log_batch_dropped++;
		if (locked)
			sceKernelSignalSema(log_lock, 1);
		/* build 99: a batch three quarters full is written at once (the
		blackbox's first frames stream hundreds of textures in one frame) */
		if (log_batch_length > log_batch_size * 3 / 4)
		{
			if (log_wake >= 0 && sceKernelGetThreadId() != log_wake_thread)
				sceKernelSignalSema(log_wake, 1); /* (a second signal while one waits is refused: harmless) */
			else
				log_flush();
		}
		return;
	}
	file = sceIoOpen(LOG_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	if (file >= 0)
	{
		int length = strlen(text);

		/* each write rewrites a sector of the stick, so a separate newline
		nearly doubled a line's cost */
		if (locked && length < (int)sizeof(log_line_joined) && !log_each_line())
		{
			memcpy(log_line_joined, text, length);
			log_line_joined[length] = '\n';
			sceIoWrite(file, log_line_joined, length + 1);
		}
		else
		{
			sceIoWrite(file, text, length);
			sceIoWrite(file, "\n", 1);
		}
		sceIoClose(file);
	}
	else if (file != log_failure)
	{
		char line[80];

		snprintf(line, sizeof(line), "log.txt: can't open it (0x%08x)", (unsigned)file);
		screen_write(line);
	}
	if (file >= 0)
		log_descriptor = file;
	else
		log_failure = file;
	if (locked)
		sceKernelSignalSema(log_lock, 1);
}

/* ---------- breadcrumbs: psp_fixups.py puts a step before each call of the
game's start-up, so the log (and the heartbeat) say how far it got */

static const char *volatile last_step = "platform start-up";

/* every step is remembered for the heartbeat; the first STEP_LOG_LIMIT
are logged too (start-up and the first frames), numbered. 0 for profiling:
logging a step takes milliseconds */
#define STEP_LOG_LIMIT 0
static volatile int step_count = 0;

/* ---------- a flat profile from the breadcrumbs: the time from one step to
the next is charged to the first. A step comes before every call in the main
loop, game_tick, main_game_render, render_frame and render_window
(psp_fixups.py), so each call's time lands on its own name, and the time a
call spends in calls that have steps of their own lands on those. The
heartbeat thread writes and clears the table every 10 s */
#define PROFILE_SLOTS 512
struct profile_slot
{
	const char *name;
	unsigned long microseconds;
	unsigned long calls;
};
/* build 108: in the scratchpad (psp_scratch.c), with the front table below:
the model draws reach for them ~2,800 times a second, and in b30's beach they
had 2.5% of the drawing's data misses (b108l, docs/overnight_b108.md). Taken
when the renderer starts (psp_profile_tables_take, from psp_ge.c's
ge_initialize, after keep.txt is read); steps before then are kept for the
crash report but not timed, and no profile window has begun by then */
static struct profile_slot *profile_slots = NULL; /* [PROFILE_SLOTS] */
static const char *profile_last_name = NULL;
static unsigned int profile_last_time = 0;

/* build 108: the slots in the order their names first came, so the few
dozen a frame uses sit together (the slots used to be placed by a hash of the
name, one cache line each: the emulator's cache model put 3.4% of the
drawing's data misses on this table, b30's beach), found through a small
table by name in front (512 bytes). The names are string constants: one
address each */
#define PROFILE_FRONT 64
struct profile_front_entry
{
	const char *name;
	struct profile_slot *slot;
};
static struct profile_front_entry *profile_front = NULL; /* [PROFILE_FRONT] */
static int profile_used = 0;

/* The steps are timed in one frame of every few, and the profile's figures
scaled up by the frames they stand for. A busy frame passes 2,000 steps or
more (b30's beach), each a clock read (a system call) and a table lookup,
and only the profile reads what they measure. "stepsall" (keep.txt) times
every frame */
#define STEPS_TIMED_EVERY 4
static int steps_every = STEPS_TIMED_EVERY;
static int steps_timed = 1;
static unsigned long steps_frame = 0, steps_window_frames = 0, steps_window_timed = 0;

void *psp_scratch_take(unsigned int bytes, const char *what);

void psp_profile_tables_take(void)
{
	if (profile_slots)
		return;
	profile_slots = psp_scratch_take(PROFILE_SLOTS * sizeof(*profile_slots), "the profiler's slots");
	profile_front = psp_scratch_take(PROFILE_FRONT * sizeof(*profile_front), "the profiler's front table");
	if (!profile_slots || !profile_front)
		profile_slots = NULL;
}

static void profile_forget(void)
{
	if (!profile_slots)
		return;
	memset(profile_slots, 0, PROFILE_SLOTS * sizeof(*profile_slots));
	memset(profile_front, 0, PROFILE_FRONT * sizeof(*profile_front));
	profile_used = 0;
	steps_window_frames = steps_window_timed = 0;
}

static struct profile_slot *profile_slot_for(const char *name)
{
	unsigned int front = ((unsigned int)name >> 2) % PROFILE_FRONT;
	int index;

	if (profile_front[front].name == name)
		return profile_front[front].slot;
	for (index = 0; index < profile_used; index++)
		if (profile_slots[index].name == name)
			break;
	if (index == profile_used)
	{
		/* a new name: the next slot, or the last one shared by every name
		past the table's size */
		if (profile_used < PROFILE_SLOTS)
			profile_used++;
		else
			index = PROFILE_SLOTS - 1;
		profile_slots[index].name = name;
	}
	profile_front[front].name = name;
	profile_front[front].slot = &profile_slots[index];
	return &profile_slots[index];
}

/* build 106: "notimers" (keep.txt): the per-step and per-object timers off
(each reads the system clock, a system call, and there are thousands a
frame: the steps, the objects' updates, the renderer's parts). The profile
windows and the per-type object lines then read 0; the heartbeat's frame and
tick times stay. Set when keep.txt is read */
int psp_timers_off = 0;

#if defined(PSP_RECON) && defined(PSP_TICK_STEPS)
/* "objsteps" (below): each step's time charged to the object being updated
or drawn as well */
static int objsteps_depth = 0;
static unsigned long objsteps_frames = 0, objsteps_timed_frames = 0;
static void objsteps_charge(const char *step, unsigned int microseconds);
#define OBJSTEPS_CHARGE(step, microseconds) do { if (objsteps_depth > 0) objsteps_charge(step, microseconds); } while (0)
#else
#define OBJSTEPS_CHARGE(step, microseconds) do { } while (0)
#endif

/* at each frame's end: whether the next frame's steps are timed. The step
running across the edge is closed or opened here, so each timed frame's
time is all charged and an untimed frame's none */
static void steps_frame_edge(void)
{
	int next = ++steps_frame % steps_every == 0;
	unsigned int now;

#if defined(PSP_RECON) && defined(PSP_TICK_STEPS)
	objsteps_frames++;
	if (steps_timed)
		objsteps_timed_frames++;
#endif
	steps_window_frames++;
	if (next)
		steps_window_timed++;
	if (next == steps_timed || psp_timers_off)
		return;
	now = sceKernelGetSystemTimeLow();
	if (!next && profile_last_name && profile_slots)
	{
		struct profile_slot *slot = profile_slot_for(profile_last_name);

		slot->microseconds += now - profile_last_time;
		slot->calls++;
		OBJSTEPS_CHARGE(profile_last_name, now - profile_last_time);
	}
	profile_last_name = next ? last_step : NULL;
	profile_last_time = now;
	steps_timed = next;
}

void psp_step(const char *name)
{
	unsigned int now;

	if (psp_timers_off)
	{
		/* the step is still kept for the crash report and the crumbs */
		last_step = name;
		{
			extern int psp_crumb_mark(const char *what, const char *detail);
			extern int psp_crumbs_on;

			if (psp_crumbs_on > 0)
				psp_crumb_mark("step", name);
		}
		return;
	}
	if (steps_timed)
	{
		now = sceKernelGetSystemTimeLow();
		if (profile_last_name && profile_slots)
		{
			struct profile_slot *slot = profile_slot_for(profile_last_name);

			slot->microseconds += now - profile_last_time;
			slot->calls++;
			OBJSTEPS_CHARGE(profile_last_name, now - profile_last_time);
		}
		profile_last_name = name;
		profile_last_time = now;
	}
	last_step = name;
	if (step_count < STEP_LOG_LIMIT)
		platform_log("step %d: %s", ++step_count, name);
	/* build 96: the load trace's breadcrumbs (psp_trace.c) */
	{
		extern volatile int psp_trace;
		extern void psp_trace_line(const char *format, ...);

		/* build 99: in play (the blackbox), each step the first time it
		runs: a freeze after something new happens (b97/b98: the first
		shot at an infection form) shows which code it entered */
		static const char *seen[2048];

		if (psp_trace)
		{
			unsigned int slot = ((unsigned int)name >> 2) & 2047, n;
			int fresh = 1;

			for (n = 0; n < 2048 && seen[slot]; n++, slot = (slot + 1) & 2047)
				if (seen[slot] == name)
				{
					fresh = 0;
					break;
				}
			if (fresh && n < 2048)
				seen[slot] = name;
			if (psp_trace == 1)
				psp_trace_line("step %s", name);
			else if (fresh)
				psp_trace_line("new step %s", name);
		}
	}
	/* build 101: "crumbs": the step drawn on the screen (psp_trace.c) */
	{
		extern int psp_crumb_mark(const char *what, const char *detail);

		psp_crumb_mark("step", name);
	}
}

/* ---------- objects_update by object type (build 19): objects.c times each
object's update (psp_fixups.py) and the profile window reports the sums per
tick, so a slow tick says which kind of object it went to */

#define OBJECT_ACCOUNTS 27
#define OBJECT_ACCOUNTS_FIRST_LINE 20 /* the machines' parts go on a line of their own */
static const char *const object_account_names[OBJECT_ACCOUNTS] = {
	"biped", "vehicle", "weapon", "equipment", "garbage", "projectile", "scenery", "machine",
	"control", "light", "placeholder", "sound scenery", "activation", "garbage collection",
	"(of the vehicles: type update", "damage", "node matrices)",
	"(of the bipeds: type update", "damage", "node matrices)",
	/* "machinetime" (psp_fixups.py): the relink is part of the type update,
	and only machines with lights on them count in the lights */
	"(of the machines: type update", "damage", "node matrices", "the relink", "functions and colours", "lights",
	"children and the rest)"};
static unsigned long object_microseconds[OBJECT_ACCOUNTS], object_updates[OBJECT_ACCOUNTS];

void psp_object_account(int type, unsigned long microseconds)
{
	if (type >= 0 && type < OBJECT_ACCOUNTS)
	{
		object_microseconds[type] += microseconds;
		object_updates[type]++;
	}
}

/* an object updated on a tick that isn't timed (psp_object_sample): counted,
not timed */
void psp_object_count(int type)
{
	if (type >= 0 && type < OBJECT_ACCOUNTS)
		object_updates[type]++;
}

/* Each object's update is timed for the per-type report by a pair of clock
reads, system calls of ~0.56 us each, and the object loop visits hundreds of
objects a tick, sleepers included. The report averages ~600 ticks, so timing
one tick in OBJECT_SAMPLE and scaling by it keeps it unbiased for 1/8 of the
calls. "objecttimeall": every tick timed, as before. Returns the scale for
this tick's times, or 0 for a tick that isn't timed */
#define OBJECT_SAMPLE 8
static unsigned long object_scale_now;
unsigned long psp_object_sample(void)
{
	static int every_tick = -1;
	static unsigned int tick = 0;

	{
		/* texlinecheck's tick word and heapwalk's walk (psp_tex.c, psp_libc.c) */
		extern void psp_texline_tick(void);
		extern void psp_heap_walk(const char *where);

		psp_texline_tick();
		psp_heap_walk("a tick");
	}
	if (every_tick < 0)
		every_tick = psp_keep("objecttimeall") != 0;
	if (every_tick)
		return object_scale_now = 1;
	return object_scale_now = (tick++ % OBJECT_SAMPLE) == 0 ? OBJECT_SAMPLE : 0;
}

#if PSP_DEBUG_TOOLS
/* "vehiclecost": the vehicles' updates timed by definition, so a flier's or a
Warthog's tick cost reads apart from the rest (the per-type line sums every
vehicle). Timed on the sampled ticks only, so counts are scaled as times are */
#define VEHICLE_ACCOUNTS 16
static struct { long definition_index; unsigned long microseconds, updates; } vehicle_accounts[VEHICLE_ACCOUNTS];
static int vehicle_cost = -1;

void psp_vehicle_account(long definition_index, unsigned long microseconds)
{
	int k;

	if (vehicle_cost < 0)
		vehicle_cost = psp_keep("vehiclecost") != 0;
	if (!vehicle_cost)
		return;
	for (k = 0; k < VEHICLE_ACCOUNTS && vehicle_accounts[k].updates && vehicle_accounts[k].definition_index != definition_index; k++)
		;
	if (k == VEHICLE_ACCOUNTS)
		return;
	vehicle_accounts[k].definition_index = definition_index;
	vehicle_accounts[k].microseconds += microseconds;
	vehicle_accounts[k].updates += object_scale_now;
}

static void vehicle_report(unsigned long ticks)
{
	char *tag_get_name(long tag_index);
	char line[512];
	int length, k;

	if (vehicle_cost <= 0 || !ticks)
		return;
	length = snprintf(line, sizeof(line), "vehiclecost: ms per tick (updates per tick, us each):");
	for (k = 0; k < VEHICLE_ACCOUNTS && vehicle_accounts[k].updates; k++)
	{
		char const *name = tag_get_name(vehicle_accounts[k].definition_index), *last = strrchr(name, '\\');

		if (length < (int)sizeof(line) - 64)
			length += snprintf(line + length, sizeof(line) - length, " %s %.2f (%.2f, %lu);", last ? last + 1 : name,
				vehicle_accounts[k].microseconds / 1000.0 / ticks, (double)vehicle_accounts[k].updates / ticks,
				vehicle_accounts[k].microseconds / vehicle_accounts[k].updates);
	}
	platform_log("  %s", line);
	memset(vehicle_accounts, 0, sizeof(vehicle_accounts));
}
#endif

/* build 113: render_objects.c's counts (psp_fixups.py), since the last report */
long psp_rendered_views, psp_rendered_full, psp_rendered_peak, psp_rendered_capacity, psp_render_states_new, psp_render_states_evicted;

static void object_report(SceUID file, unsigned long ticks)
{
	char line[1024];
	int length, type;

#if PSP_DEBUG_TOOLS
	vehicle_report(ticks);
#endif
	length = snprintf(line, sizeof(line), "objects_update by type, ms per tick (updates per tick):");
	for (type = 0; type < OBJECT_ACCOUNTS_FIRST_LINE && length < (int)sizeof(line) - 64; type++)
	{
		if (object_updates[type])
			length += snprintf(line + length, sizeof(line) - length, " %s %.2f (%.1f);", object_account_names[type],
				ticks ? object_microseconds[type] / 1000.0 / ticks : 0.0, ticks ? (double)object_updates[type] / ticks : 0.0);
		object_microseconds[type] = object_updates[type] = 0;
	}
	{
		extern long psp_objects_asleep, psp_objects_sleep_checked, psp_vehicles_asleep, psp_corpses_asleep; /* objects.c (psp_fixups.py) */
		extern long psp_objects_restless_skipped; /* build 118: the restless back-off */
		extern long psp_items_asleep; /* resting weapons and equipment */
		/* weapons and equipment with attached lights: updated, offered to the
		sleep rule (nothing else keeps them awake), asleep. On a line of their
		own: the line above is cut at the log's 512 bytes on busy maps */
		extern long psp_items_lit_updated, psp_items_lit_offered, psp_items_lit_asleep, psp_items_lit_reason[6];
		extern long psp_weapons_ground, psp_weapons_asleep; /* weapons not held, lit or not */

		if (length < (int)sizeof(line) - 160)
			length += snprintf(line + length, sizeof(line) - length, " asleep %.1f of %.1f per tick (%.1f vehicles, %.1f corpses, %.1f items); restless, unchecked %.1f",
				ticks ? (double)psp_objects_asleep / ticks : 0.0, ticks ? (double)psp_objects_sleep_checked / ticks : 0.0,
				ticks ? (double)psp_vehicles_asleep / ticks : 0.0, ticks ? (double)psp_corpses_asleep / ticks : 0.0,
				ticks ? (double)psp_items_asleep / ticks : 0.0,
				ticks ? (double)psp_objects_restless_skipped / ticks : 0.0);
		platform_log("  %s", line);
		platform_log("  lit items per tick: %.2f updated, %.2f offered to sleep, %.2f asleep; kept awake: held %.2f, not at rest %.2f, on an object %.2f, tracking %.2f, breakable surface %.2f",
			ticks ? (double)psp_items_lit_updated / ticks : 0.0, ticks ? (double)psp_items_lit_offered / ticks : 0.0,
			ticks ? (double)psp_items_lit_asleep / ticks : 0.0,
			ticks ? (double)psp_items_lit_reason[1] / ticks : 0.0, ticks ? (double)psp_items_lit_reason[2] / ticks : 0.0,
			ticks ? (double)psp_items_lit_reason[3] / ticks : 0.0, ticks ? (double)psp_items_lit_reason[4] / ticks : 0.0,
			ticks ? (double)psp_items_lit_reason[5] / ticks : 0.0);
		platform_log("  weapons on the ground per tick: %.2f updated, %.2f asleep",
			ticks ? (double)psp_weapons_ground / ticks : 0.0, ticks ? (double)psp_weapons_asleep / ticks : 0.0);
		{
			extern long psp_ws_passed; /* objects.c (psp_fixups.py FIXES_WS) */

			platform_log("  sleeping scenery passed over in the walk per tick: %.2f", ticks ? (double)psp_ws_passed / ticks : 0.0);
			psp_ws_passed = 0;
		}
		memset(psp_items_lit_reason, 0, sizeof(psp_items_lit_reason));
		psp_weapons_ground = psp_weapons_asleep = 0;
		psp_objects_asleep = psp_objects_sleep_checked = psp_vehicles_asleep = psp_corpses_asleep = 0;
		psp_objects_restless_skipped = psp_items_asleep = 0;
		psp_items_lit_updated = psp_items_lit_offered = psp_items_lit_asleep = 0;
	}
	{
		/* "machinetime": a machine's parts; and the relink memo's lookups
		(cluster_partitions.c, psp_fixups.py FIXES_121) */
		extern unsigned long psp_relink_hits, psp_relink_misses, psp_relink_wrong;

		length = snprintf(line, sizeof(line), "machine parts, ms per tick (updates per tick):");
		for (type = OBJECT_ACCOUNTS_FIRST_LINE; type < OBJECT_ACCOUNTS && length < (int)sizeof(line) - 64; type++)
		{
			if (object_updates[type])
				length += snprintf(line + length, sizeof(line) - length, " %s %.3f (%.1f);", object_account_names[type],
					ticks ? object_microseconds[type] / 1000.0 / ticks : 0.0, ticks ? (double)object_updates[type] / ticks : 0.0);
			object_microseconds[type] = object_updates[type] = 0;
		}
		if (length < (int)sizeof(line) - 128)
			length += snprintf(line + length, sizeof(line) - length, " relink lookups per tick: %.1f remembered, %.1f worked out, %lu wrong",
				ticks ? (double)psp_relink_hits / ticks : 0.0, ticks ? (double)psp_relink_misses / ticks : 0.0, psp_relink_wrong);
		psp_relink_hits = psp_relink_misses = 0;
		platform_log("  %s", line);
	}
	{
		/* build 113: the views drawn (a frame each, as a rule), how full
		their rendered-object list got, and the render-state cache's misses
		(render_objects.c, psp_fixups.py): a miss works an object's lighting
		out whole, and an eviction first scans the whole cache for the oldest */
		if (psp_rendered_views)
			platform_log("  objects drawn: %ld views; the list's peak %ld of %ld, full in %ld views (objects past it not drawn) | "
				"render states: %.1f worked out whole a view, %.1f of them pushing out the oldest",
				psp_rendered_views, psp_rendered_peak, psp_rendered_capacity, psp_rendered_full,
				(double)psp_render_states_new / psp_rendered_views, (double)psp_render_states_evicted / psp_rendered_views);
		psp_rendered_views = psp_rendered_full = psp_rendered_peak = 0;
		psp_render_states_new = psp_render_states_evicted = 0;
	}
	{
		/* build 107: the most lens flares a frame has submitted since the
		start (rasterizer_lights.c, psp_fixups.py), against the 1,024 kept */
		if (psp_lens_flare_peak)
			platform_log("  lens flares: peak %ld a frame of the 1024 kept", psp_lens_flare_peak);
		/* the markers' axes worked out only for the flares kept, from a table
		by their direction bytes, and the markers past a full frame skipped
		(FIXES_LFS; lfscheck counts answers that differ, 0 expected) */
		if (psp_lfs_markers && psp_lfs_frames)
			platform_log("  lens flare axes: %.1f level markers a frame, %.1f kept (axes asked), %.1f of those from the table, "
				"%.1f past a full frame skipped; lfscheck: %lu differed",
				(double)psp_lfs_markers / psp_lfs_frames, (double)psp_lfs_kept / psp_lfs_frames,
				(double)psp_lfs_hits / psp_lfs_frames, (double)psp_lfs_skipped / psp_lfs_frames, psp_lfs_wrong);
		psp_lfs_frames = psp_lfs_markers = psp_lfs_kept = psp_lfs_hits = psp_lfs_skipped = 0;
	}
	{
		/* build 107: keep.txt's "sceneryhalf" (psp_fixups.py, objects.c):
		sleeping scenery checked on alternate ticks */
		extern long psp_scenery_unchecked, psp_scenery_woken;

		if (psp_scenery_unchecked)
			platform_log("  sleeping scenery, checked every other tick: %.1f left unchecked per tick, %.1f woken per tick (every wake, checked tick or not: a change is noticed up to a tick late)",
				ticks ? (double)psp_scenery_unchecked / ticks : 0.0, ticks ? (double)psp_scenery_woken / ticks : 0.0);
		psp_scenery_unchecked = psp_scenery_woken = 0;
	}
	{
		/* build 106: the corpse-sleep rule's stages (psp_fixups.py), when any
		biped was dead in the window */
		extern long psp_corpse_stage[4], psp_corpses_holding, psp_corpses_placed_dead;
		extern long psp_corpses_off_level, psp_corpse_ground_tests;

		/* build 112: "placed dead": bodies the level placed dead, which go on
		to the same stages as garbage (at rest, idle, offered) */
		if (psp_corpse_stage[0])
			platform_log("  corpses, per tick: %.1f dead, %.1f of them garbage, %.1f placed dead, %.1f at rest, %.1f idle past 2 s (offered to sleep), %.1f of those still holding something, %.1f held up by something other than the level (kept awake; %ld ground tests)",
				ticks ? (double)psp_corpse_stage[0] / ticks : 0.0, ticks ? (double)psp_corpse_stage[1] / ticks : 0.0,
				ticks ? (double)psp_corpses_placed_dead / ticks : 0.0,
				ticks ? (double)psp_corpse_stage[2] / ticks : 0.0, ticks ? (double)psp_corpse_stage[3] / ticks : 0.0,
				ticks ? (double)psp_corpses_holding / ticks : 0.0, ticks ? (double)psp_corpses_off_level / ticks : 0.0,
				psp_corpse_ground_tests);
		memset(psp_corpse_stage, 0, sizeof(psp_corpse_stage));
		psp_corpses_holding = psp_corpses_placed_dead = 0;
		psp_corpses_off_level = psp_corpse_ground_tests = 0;
	}
	{
		/* build 106: the garbage collector (psp_fixups.py, objects.c) */
		extern long psp_garbage_runs[3], psp_garbage_removed, psp_garbage_bipeds_removed;

		if (psp_garbage_runs[0] || psp_garbage_runs[1] || psp_garbage_runs[2])
			platform_log("  garbage collector: %ld runs forced (a script or the console), %ld for space, %ld for too many active; %ld objects removed, %ld of them bipeds",
				psp_garbage_runs[0], psp_garbage_runs[1], psp_garbage_runs[2], psp_garbage_removed, psp_garbage_bipeds_removed);
		memset(psp_garbage_runs, 0, sizeof(psp_garbage_runs));
		psp_garbage_removed = psp_garbage_bipeds_removed = 0;
	}
	{
		/* build 106: the game's model part draws (rasterizer.c's hook,
		psp_fixups.py): before the GE (texture keys, colours) and in
		psp_model_draw (the geometry locks and the GE's own work) */
		extern unsigned long psp_model_draw_us[3];

		if (psp_model_draw_us[2])
			platform_log("  model part draws: %lu, %.1f us each before the GE, %.1f in psp_model_draw",
				psp_model_draw_us[2], (double)psp_model_draw_us[0] / psp_model_draw_us[2],
				(double)psp_model_draw_us[1] / psp_model_draw_us[2]);
		memset(psp_model_draw_us, 0, sizeof(psp_model_draw_us));
	}
	{
		/* build 107: cluster partition unlinks, direct against walked
		(cluster_partitions.c; psp_fixups.py) */
		extern void psp_cluster_unlink_counts(unsigned long *direct, unsigned long *walked, unsigned long *checked,
			unsigned long *wrong);
		unsigned long direct, walked, checked, wrong;

		psp_cluster_unlink_counts(&direct, &walked, &checked, &wrong);
		if (direct || walked)
			platform_log("  cluster unlinks, per tick: %.1f direct, %.1f walked the list",
				ticks ? (double)direct / ticks : 0.0, ticks ? (double)walked / ticks : 0.0);
		if (checked)
			platform_log("  unlinkcheck: %lu direct unlinks compared with the walk since the start, %lu removed a different element",
				checked, wrong);
	}
	{
		/* build 107: objects rejected on their sphere's copy, before their
		datum is read (collisions.c; psp_fixups.py) */
		extern unsigned long psp_sphere_tests, psp_sphere_rejects, psp_sphere_checked, psp_sphere_wrong;

		if (psp_sphere_tests)
			platform_log("  object spheres, per tick: %.1f tested from the copy, %.1f rejected without reading the object",
				ticks ? (double)psp_sphere_tests / ticks : 0.0, ticks ? (double)psp_sphere_rejects / ticks : 0.0);
		if (psp_sphere_checked)
			platform_log("  spherecheck: %lu copies compared with the object since the start, %lu differed",
				psp_sphere_checked, psp_sphere_wrong);
		psp_sphere_tests = psp_sphere_rejects = 0;
	}
	if (file >= 0)
	{
		sceIoWrite(file, line, strlen(line));
		sceIoWrite(file, "\n", 1);
	}
}

/* build 106: rasterizer.c's model part draws (object_report) */
unsigned long psp_model_draw_us[3];

void psp_sleepguard_start(void);

/* ---------- what the null path keeps (build 19). With the GE drawing, the
old Direct3D path's shadows, see-through parts, particles, decals, HUD and
texture loads cost CPU time and draw nothing, and render interpolation
blends between ticks the PSP can't render more often than anyway: all are
off unless ms0:/halo/keep.txt names them (shadows transparent particles
decals hud textures interpolation), to measure what each costs */

/* keep.txt's text, read whole up to KEEP_FILE_BYTES (the launch menu writes
no more): a preset with the menu's own words needs ~250, and words past the
limit are lost silently. KEEP_DEFAULTS_ROOM more holds the campaign's
default words (keep_defaults_add), however full the file is */
#define KEEP_FILE_BYTES 509
#define KEEP_DEFAULTS_ROOM 256
static char keep_words[KEEP_FILE_BYTES + 3 + KEEP_DEFAULTS_ROOM] = "";
/* build 106: bumped whenever keep_words changes, so psp_keep's remembered
answers are asked again */
static int keep_generation = 1;
static SceUID keep_cache_lock = -1; /* made by keep_load, at start-up */

static int keep_search(const char *name);
int psp_keep_number(const char *prefix, int fallback);
extern int psp_timers_off;

#ifndef PSP_MULTIPLAYER
/* The campaign's default words: M6 #2, frames in flight with the GE's calls
carried out on the Media Engine (docs/review_m6default.md), on as if keep.txt
named them, so every psp_keep() asks as before and the features' own code
is untouched. Each has an off word; "m6off" turns all of them off. They play
tick for tick as without them; the PSP-1000 draws b30 faster with them.
texpretouch only with gemeon: it reads in what the ME's replay binds;
without it, textures arrive late and models draw white for a few frames
(about six times the untextured frames on b30's route).
geshow3 (docs/review_geshow3.md): the ME replays the next frame into a
second list while the GE ends this one; it takes effect only with the
forwarding thread (geinflight, gemeon), and its index ranges come from
geshow2's ring, so the GE never reads a later frame's triangles.
geowindow: the tag window's span no BSP of the map uses becomes the geometry
cache's second stretch (b30 +312 KB, c20 +1.2 MB): memory otherwise idle,
which plays the same (only reads change); both PSPs' ladders ran every map
with it, no level change or frame rate different (PSP runs).
Left out whole with the instruments that measure or split the frame as they
did before (mebench, gemeoverlap, geserial): those words mean what they did.
The multiplayer build has none of them: none was played against a server */
static const struct
{
	const char *word, *off;
} keep_defaults[] = {
	{ "gedefer", "gedeferoff" },
	{ "geinflight", "geinflightoff" },
	{ "gemeon", "gemeonoff" },
	{ "mevram", "mevramoff" },
	{ "texpretouch", "notexpretouch" },
	{ "mepoll", "mepolloff" },
	{ "geshow3", "geshow3off" }, { "geowindow", "geowindowoff" },
	/* the dither centred for the GE's truncating writes: without it dark
	scenes draw about 4 levels darker than the PC on every channel (a10's
	opening against ChupathingyCE's frames); a restoration */
	{ "ditherlift", "noditherlift" },
	/* a light's gel lit by its six faces, so the alert lights' beams reach the
	walls (psp_fixups.py, gelfaces), or sampled from the gel's own cube at
	each lit vertex where the pack keeps it (nogelcube: by the faces); on
	provisionally while its cost in a10's hallway (+8-15%) is weighed */
	{ "gelfaces", "nogelfaces" },
	/* Chief's visor drawn gold from its own sphere map (psp_ge.c, chiefvisor):
	folded into the lighting it stayed near black; a restoration */
	{ "chiefvisor", "nochiefvisor" },
	/* Chief's armour as the Xbox draws it: its reflection added over the lit,
	change-coloured base where the multipurpose map marks it (psp_ge.c,
	chiefsheen), and its detail map over textures kept at their own size
	(chiefdetail; texpack.py bakes Chief at full size by default); restorations */
	{ "chiefsheen", "nochiefsheen" },
	{ "chiefdetail", "nochiefdetail" },
	/* the frame handed to the ME in pieces as it is written, gated on the main
	CPU's wait for the ME (psp_ge.c, mechunk): where the frame waits for the
	ME the replay runs beside the writing (a30 7.7 -> 12.7 fps, d20 11.4 ->
	13.7 on the PSP-3000's ladder120); where it doesn't, the gate leaves the
	frame whole (b30 6.5 -> 6.6) */
	{ "mechunk", "mechunkoff" }, { "flipwait", "flipwaitoff" }, PSP_RELEASE_DEFAULTS
};
static char keep_defaults_added[KEEP_DEFAULTS_ROOM] = "";

static void keep_default_add(const char *word)
{
	size_t used = strlen(keep_words), length = strlen(word);

	if (keep_search(word) || used + length + 2 > sizeof(keep_words))
		return;
	/* keep_words ends in a space: the word, then the space a whole-word
	search needs after it */
	memcpy(keep_words + used, word, length);
	keep_words[used + length] = ' ';
	keep_words[used + length + 1] = '\0';
	/* the log's list of added words: kept within its buffer, whatever the
	defaults grow to */
	if (strlen(keep_defaults_added) + length + 2 <= sizeof(keep_defaults_added))
	{
		strcat(keep_defaults_added, " ");
		strcat(keep_defaults_added, word);
	}
}

static void keep_defaults_add(void)
{
	int k;

	keep_defaults_added[0] = '\0';
	/* the motion sensor's target in the depth buffer (docs/review_vrbroom.md):
	exact, and its 16 KB of VRAM go to the model copies; apart from M6 #2,
	so m6off and the instruments leave it on */
	if (!keep_search("blipdepthoff"))
		keep_default_add("blipdepth");
	/* AI on alternate ticks with the timeslice kept whole (docs/review_aicad.md):
	a compromise, on so it can be felt in play; aicad3off is the other
	side, and aicad2 still wins when asked */
	if (!keep_search("aicad3off") && !keep_search("aicad2"))
		keep_default_add("aicad3");
	if (keep_search("m6off") || keep_search("mebench") || keep_search("gemeoverlap") || keep_search("geserial"))
		return;
	for (k = 0; k < (int)(sizeof(keep_defaults) / sizeof(keep_defaults[0])); k++)
	{
		if (keep_search(keep_defaults[k].off))
			continue;
		if (!strcmp(keep_defaults[k].word, "texpretouch") && !keep_search("gemeon"))
			continue;
		keep_default_add(keep_defaults[k].word);
	}
	/* b30's busiest frames fill the 128 KB buffer, and the ME then waits
	for its parts (design_inflightkb.md); a size given in keep.txt wins */
	if (keep_search("geinflight") && psp_keep_number("inflightkb", -1) < 0)
		keep_default_add("inflightkb224");
}
#endif

static void keep_load(void)
{
	SceUID file = sceIoOpen(DATA_ROOT "/keep.txt", PSP_O_RDONLY, 0);
	int length = 0;

	if (file >= 0)
	{
		length = sceIoRead(file, keep_words + 1, KEEP_FILE_BYTES);
		sceIoClose(file);
	}
	if (length < 0)
		length = 0;
	keep_words[0] = ' ';
	keep_words[length + 1] = ' ';
	keep_words[length + 2] = '\0';
	for (file = 0; keep_words[file]; file++)
		if (keep_words[file] == '\n' || keep_words[file] == '\r' || keep_words[file] == '\t' || keep_words[file] == ',')
			keep_words[file] = ' ';
#ifndef PSP_MULTIPLAYER
	keep_defaults_add();
#endif
	keep_generation++;
	/* build 114: "lean" (E2, optimization_ranking.md): the play set-up with
	the measuring off, notimers and nooverlay in one word */
	psp_timers_off = keep_search("notimers") || keep_search("lean");
	/* a release (PSP_METRICS=0) starts with notimers among its defaults
	(PSP_RELEASE_DEFAULTS, psp_post.h, in keep_defaults above): the game's
	instrument timers (a clock call per model part, per moving particle,
	per physics update...) feed only statistics no release prints, and cost
	b30 ~3% of its game speed on the PSP-3000. "timerson" keeps them. The
	default rides in the table, not here, so this function's code is the
	same in both builds and the profile counted on the measuring build fits
	the release's too */
	steps_every = keep_search("stepsall") ? 1 : STEPS_TIMED_EVERY;
	if (keep_cache_lock < 0)
		keep_cache_lock = sceKernelCreateSema("halo keep cache", 0, 1, 1, NULL);
	platform_log("null path keeps:%s(keep.txt; skipped otherwise: transparent particles textures interpolation; also: noshadows nodecals nodetail nocull cullflip nooverlays fullparticles novfpu hwprofile hud100 hud115 hud130 hud145 hud160 hudnopush vehiclesawake flaresexact nohud noparticles fulllod awake sleepdebug audiodump savetest menutest ...)",
		strlen(keep_words) > 2 ? keep_words : " nothing ");
	/* B41's stopgap (psp_sleepguard.c): the sleep switch held off, once
	the keep words can say otherwise */
	psp_sleepguard_start();
	/* build 89: in two lines (the one line passed the log's 512 characters) */
	platform_log("keep words, continued: vramlargest clearmagenta nomip listuncached stalleach near2 near4 mipbias1 mipbias2 decaloffset passes overlayoffset decalbig detailbias15 detailbias2 decalall detailnear mipauto detailauto noangle visdump modeldrop overlaynoslope overlaynowall overlayslope1 overlayslope4 overlayfreefar skydump flatshade tdump");
	platform_log("keep words, continued: nearauto walldump decalnoslope vblankthrottle noboxtest detailallfar verifytags texsqueeze texvramonly colltest geoasync nudgewide decalbatch menushot geserial vramcold uvwide nooverlay meaudio nomeaudio benchrun levelcompact meclassify noskincut novblankthrottle");
	/* build 115 */
	platform_log("keep words, continued: iostats gereads vramtexN edramcheck overlayfreefar inflightkbN inflightroom inflighttextold");
	/* build 116: the restorations are on by default; these words turn each off */
	platform_log("keep words, restorations off: overlayfreenear clearblue seethroughflat senvflat planarfogoff notint waterwhite flaresnobipeds nodither detailgrey80 flashnearest nearseatedfar nearseatcine nometers lognohold statepages");
	/* build 117: its own restorations, each turned off by these; and the
	speed defaults' off words */
	platform_log("keep words, build 117's restorations off: hudonemap glassflat lightcutone geoshare nopointlights ditherall; speed: vrammodels0; tests: ladderN exitatN");
	/* build 118: its restoration and fix, each turned off by these; its exact
	speed-ups' off words; their self-checks; and the instruments */
	platform_log("keep words, build 118's: restoration off: overlaywallnear; fix off: mapfilesopen; speed off: restlesscheck nearspherefull multiscan verifyvectors tcutfull; tests: spherememo (the sphere memo, off by default since 2026-10-22); checks: spherememocheck nearspherecheck multicheck tcutcheck; instruments: mebench, gemeoverlap (opt-in)");
	/* build 119: its exact speed-ups' off words, their self-checks, the
	clock fix's off word, and the PSP probes */
	platform_log("keep words, build 119's: speed off: nearclusterfull skinaxisloop rigidrunsfull surfaceqsort; checks: nearclustercheck skinaxischeck rigidrunscheck surfacesortcheck; fix off: vblankslow; instruments: probes (opt-in)");
	platform_log("keep words, build 120's: speed off: itemsawake; check: itemsleepcheck; Media Engine (opt-in): mehalt mefinishslow");
	platform_log("keep words, build 123's: speed off: aiprofilefull; speed, opt-in: meaudiofirst meraysbatch meaudioranges mepushdrop mestash meraysminN weapontracksleep; restorations off: cutroomoff (cutroom: the larger cut buffer from the start); lit items: litemsawake; recon: activecensus");
	platform_log("keep words, the controls: off: crouchhold (crouch held, under the engine's 98%% rule), actiontap (the X button as the engine reads it), menuselectpress (select in the menus is back on its press, chords and all, not a tap); actionholdN (ms before d-pad left acts, 180); the nub: nubdeadN nubreachN (percent of half travel; or ms0:/halo/nub.txt); the profile until Halo's menu sets it: buttonpresetN (0-4) stickpresetN (0-3) looksensN (1-10) lookinvert");
	platform_log("keep words, build 124's: speed off: weapontrackawake (weapontracksleep retired); retired: cutroom cutroomoff (see the cut stream's line); restorations off: levelexactoff (the level drawn from the eye as the GE rounds it), machinefuncsheld (sleeping machines hold their functions); checks: funccheck; tests: shotrunN (N frames in a row at each shotevery moment)");
	platform_log("keep words, the scene ladder: sceneladder (each campaign map's opening cutscene, one after another; the game plays on after the last); sceneladderexit (quits after the last); ladderskip<map> (that map passed over, by both ladders); phasemark (a log line at each phase edge, the heartbeat's window cut there; the time map)");
	platform_log("keep words, the transition check (opt-in): transwalk (each loading zone of the level in turn, then any BSP none reaches, forced), transwalkN (N s at each), transwaitN (not before N s in), transwon (then the mission's end, game_won, and 25 s into what follows)");
	platform_log("keep words, the level copies' room: fix off: streamroomoff (a material with no room for its copy tried again on every draw); tests: streamsqueezeN (the copies held to N KB of the arena, none from the general heap); levelcarveN (KB of the texture RAM cache for the copies past the arena, on maps whose geometry cache is carved there; 768 by default, levelcarve0 none); heap: heaproom256 (256 KB of the arena's room in the level carve, from the geometry cache; on by default with geinflight and geo2 or geo15), heaproomN (N KB, 128 to 768 in 128s, the same way), heaproomoff (not even then); campaign only");
	platform_log("keep words, build 122's: speed off: percold surfacetestold machinesawake meslantoff posingold animlockcall; restorations off: noreopen drawnmapscale; HUD back to b120's: hudnosnap hud150 hudnopushup; checks: perccheck surfacetestcheck meslantcheck meringtest relinkcheck posingcheck; opt-in: relinkmemo posingmemo; recon: machinetime reopentest");
	platform_log("keep words, build 122's: Media Engine: meraysoff (perception's rays back on the main CPU), meraysfrontN (checked by perccheck)");
	platform_log("keep words, build 122's look calls (opt-in, for an A/B): seethroughvfpu lightmerge foliagetwopass sceneryhalf");
	platform_log("keep words, build 122's behaviour calls (opt-in, for an A/B): aihalf aihalfdistN aihalfview; campaign only: machinesfar posehalf");
	platform_log("keep words, compromises: lodpspoff (models' detail picked at the PSP's size, not 1.76x nearer; on by default, lodpsp still accepted), lodpspall (the nearer pick with no floor for models whose lower levels lose a part); recon: lodpspscan (every model's floor logged as a map's first model is drawn)");
	platform_log("keep words, compromises (opt-in, for an A/B): campaign only: aicad2 (each actor thinks on every other tick, staggered), aicad3 (the same, the timeslice at full rate); recon: aioutcome");
	platform_log("keep words, the Media Engine's wake: opt-in: mewake (the ME started again after a sleep; mewakeoff wins over it), mewakeearly (with mewake: released in the kernel's wake handler, not at a frame end), mewakeprobe (with mewake: at the restart the ME tries each kind of job work before its loop, marked step by step), mewakefw (with mewake: the restart lets go of the AVC and AW resets launch found let go); fix off: meresetwhole (the reset register written whole); recon: mewaketest; test: mebcc1 (the ME unit word m-cid finds fastest)");
	platform_log("keep words, the level given from an origin near the eye: restoration off: levellocaloff (the level's copies from the world's origin, as before); instrument (opt-in): geprecision (the GE's precision measured as the first frame begins: the log's geprecision lines)");
	platform_log("keep words, see-through as the Xbox draws it: restorations off: nolevelglow (the level's self-illumination pass), seethroughpush (the port's depth nudges for surfaces that aren't decals), seethroughwrap (every map repeated), waterunfogged (added water unfogged); look call (opt-in): ditherhalf, ditherlift on by default (offsets +4; noditherlift turns it off), dithershift (detail and sky passes dithered, the pattern moved each pass) dithershiftall (every blend); recon: noglowpass nobackpass ditherprobe (the GE's rounding, logged at start-up)");
	platform_log("keep words, build 126's: speed off: pathclearfull (a path search's whole state cleared before it, RV2), deletedwalkall (a deleted object's fix-up walks every object, RV1), aipathold (the nearby firing positions test searches first, smoothing scans from the first, RV4); checks: pathclearpoison deletedcheck aipathcheck; restorations off: weaponstateold (the HUD's weapon state left as the stack held it, RB1), aimchangeold (a still unit with no aiming limit at full aiming change, RH1)");
	platform_log("keep words, build 126's instrument (opt-in): frametrace (every frame timed and split; a line each 300 frames: the slow frames, the game time lost, the run's 99%% and 99.9%% frame times; and a framelate line: the late frames, held or not, and their gaps); hblow (the heartbeat thread below the game thread)");
	platform_log("keep words, build 127's: speed off: logeachline (each log line written at once for the first 20 s, its newline apart)");
	platform_log("keep words, build 128's: speed off: obscurertest (the camera's ray through every object as each model pass begins, which only a debug display reads), nearautofull (nearauto's level spheres gathered whole, not stopped at the first surface); check: nearautocheck");
	platform_log("keep words, build 129's: speed off: flaresfull (a flare's ray tests the objects even when the level hides it), fractiondouble (the animations' and periodic functions' fractions in software double); check: flarescheck, fractioncheck");
	platform_log("keep words, build 130's instruments (opt-in): posepredict (how often a tick's base pose is the frame after its last), framecrc (each finished picture hashed and logged with the game time; tools/framecmp.py compares two runs), readswait (every cache file read waited for as it is asked, so pictures do not depend on read timing; slower)");
	platform_log("keep words, build 130's default change (2026-10-15): cutless on (triangles and cut pieces wholly off the screen left out); cutlessoff cuts and draws them as before");
	platform_log("keep words, build 131's (opt-in while proven): poseahead (next tick's base poses made ahead on the main CPU), poseaheadme (made ahead on the Media Engine during the frame's drawing, for animations in the tags or resident in the animation cache), poseaheadcheck (made ahead and decoded anyway, compared bit for bit)");
	platform_log("keep words, build 131's: speed off: surfaceboundoff (the level sphere walk tests every surface it reaches, out of reach or not); check: surfacetestcheck");
	platform_log("keep words, build 132's: speed off: poseaheadoff (the Media Engine pose job off: on by default where the ME runs the job ring); as before: memarksatlaunch (the launch's register read also reads the ME's own two words while it runs)");
	platform_log("keep words, build 132's recon (emulator): nmreaders (who first reads each object's node matrices after they're computed, by nodes, and the call sites of other objects' reads)");
	platform_log("keep words, build 136's: wake-on-write on in local games (a sleeping object checksummed only when something marked it since its last check); speed off: wakeonwriteoff (every sleeper summed every tick); opt-in: wakeonwrite (on in networked games too); check: wakecheck (every sleeper summed anyway, an unmarked change logged with the words that changed). wakeonwrite and wakecheck win over the off word");
	platform_log("keep words, the profile: stepsall (every frame's steps timed; otherwise one frame in %d, and profile.txt's figures scaled by the frames each stands for)", STEPS_TIMED_EVERY);
	platform_log("keep words, models as the Xbox draws them: restorations off: seethroughtwosided (see-through parts drawn from both sides), nolayers (shaders' extra layers left out), nearseatcine (a seated player's cinematics keep the near plane out), noselfocclude (Cortana's back layers drawn over her front, her parts sorted with the rest); look calls (opt-in, for an A/B): camoshimmer (active camouflage as its own map added faintly), genericnoise (a generic shader's second map drawn again over it, moving); instruments: neardump (what set the near plane, on shot frames), shotfromN (shots start at N s), scriptsdump (scenario scripts at load; recon builds)");
	platform_log("keep words, shelved (docs/shelf.md): nwbatch and its kin built in only with PSP_SHELF=1; still built but shelved, opt-in: meaudiofirst meraysbatch meaudioranges mepushdrop mestash (the b123 ME hygiene), mewake, mewarmtest, mefinishslow");
	platform_log("keep words, the heap at each level change (opt-in): heapmap (its gaps and blocks of 16 KB or more, with what asked for each); heapcountall (requests refused with a fallback, the level copies' among them, count toward HEAP FULL again); fix off: overlaymemooff (a see-through list refused heap for its answers asked again for each triangle, every frame, as before); recon builds only: poolpeaks (each game-state array's peak against its maximum)");
	platform_log("keep words, the memory census (opt-in; docs/memory_census.md): memcensus (the geometry cache's working set and re-reads by distance, the tag window's free span); geowindow (the tag window's span no BSP of the map uses, past the animation cache's, as the geometry cache's second region)");
	platform_log("keep words, the HUD's textures: fix off: hudpinmeold (with the Media Engine replaying the frame, the HUD's and the motion sensor's textures were neither pinned nor read past the frame's budget, and lost to a full texture cache)");
	platform_log("keep words, sound requests (SNDNOYIELD): speed off: soundyieldold (a request for a sound not loaded yet yields the thread even when its caller won't wait for it, as before; by default only a waiting caller yields); counts on the sound cache's line either way");
	platform_log("keep words, the sound mixer's lock (MIXLOCK): speed off: mixlockold (the audio thread holds the mixer's lock through the Media Engine's job and the game's sound calls wait for it, as before; by default they never wait)");
	platform_log("keep words, a pool compaction (COMPACTSKIP): speed off: compactalways (a pool already compact is walked anyway, as before); check: compactcheck (walks, and counts blocks a skip would have left out of place on the metrics compact line)");
	platform_log("keep words, small wins batch 1 (COMPACTSKIP, GCHEAD): all off: smallwins1old; each off: compactalways (pool compaction walked even when compact); opt-in: gchead (the garbage collector keeps each player's head for a collection; off by default after the PSP-3000 removed garbage in view with it on)");
	platform_log("keep words, small wins batch 2 (VIS): all off: smallwins2old; each off: visrectold (every portal clipped against the region seen, as before); check: visrectcheck (clips the skipped ones anyway and counts any not empty)");
#ifndef PSP_MULTIPLAYER
	platform_log("keep words, the campaign's defaults (M6 #2, docs/review_m6default.md): added:%s; off: m6off (all of M6 #2), gedeferoff geinflightoff gemeonoff mevramoff notexpretouch mepolloff, inflightkbN (another buffer size; 128 as before); none of M6 #2 with mebench gemeoverlap geserial; blipdepthoff (the sensor target in its own 16 KB)",
		keep_defaults_added[0] ? keep_defaults_added : " none");
#endif
}

/* build 102: whether keep.txt has been read yet (psp_trace.c's stick lock
asks before it caches its answer) */
int psp_keep_loaded(void)
{
	return keep_words[0] != 0;
}

/* The fractions' path (halo_math.h's psp_fraction_of, psp_fixups.py):
single precision unless "fractiondouble"; "fractioncheck" makes both and
compares them. Read once keep.txt is in; until then, single */
int psp_fraction_word = -1;
static unsigned long fraction_checked = 0, fraction_differed = 0;

int psp_fraction_word_read(void)
{
	if (!psp_keep_loaded())
		return 0;
	psp_fraction_word = psp_keep("fractioncheck") ? 2 : psp_keep("fractiondouble") ? 1 : 0;
	return psp_fraction_word;
}

/* the double path's answer, after counting whether the single one has its bits */
float psp_fraction_compared(float x, float single, int floor_of_abs)
{
	float twice = floor_of_abs ? (float)(floor)((fabs)((double)x)) : (float)(fmod)((double)x, 1.0);
	union { float f; unsigned int u; } of, a, b;

	of.f = x;
	a.f = single;
	b.f = twice;
	fraction_checked++;
	if (a.u != b.u)
	{
		if (fraction_differed < 8)
			platform_log("fractioncheck: %s of %08x: single %08x, double %08x",
				floor_of_abs ? "floor" : "fraction", of.u, a.u, b.u);
		fraction_differed++;
	}
	return twice;
}

/* build 106: the cache file thread's priority, set from that thread when it
first wakes after keep.txt is read (psp_fixups.py, cache_files_windows.c).
0x1c: above the game thread (0x20) and below the heartbeat (0x18), the
audio (0x12) and the callbacks (0x11). Why: psp_threads_report, psp_trace.c.
"readerslow" in keep.txt leaves it as it was, for comparing */
#define PSP_READER_PRIORITY 0x1c
int psp_keep(const char *name);
void psp_cache_thread_priority(void)
{
	static int done = 0;
	SceKernelThreadInfo info;

	if (done || !psp_keep_loaded())
		return;
	done = 1;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	sceKernelReferThreadStatus(0, &info);
	if (psp_keep("readerslow"))
	{
		platform_log("cache file thread: priority 0x%x left as it is (readerslow)", info.currentPriority);
		return;
	}
	if (sceKernelChangeThreadPriority(0, PSP_READER_PRIORITY) < 0)
		platform_log("cache file thread: priority 0x%x; the change to 0x%x was refused", info.currentPriority, PSP_READER_PRIORITY);
	else
		platform_log("cache file thread (%s): priority 0x%x, now 0x%x (above the game thread's: readerslow undoes it)",
			info.name, info.currentPriority, PSP_READER_PRIORITY);
}

/* build 119: the port's vertical blank thread (d3d8_gl.c), above the game
thread so it counts each blank on time (psp_fixups.py, FIXES_119 d3d8_gl.c
#vblank): 0x1e, under the cache file thread's 0x1c. "vblankslow" leaves it
at its own (0x3c) */
#define PSP_VBLANK_PRIORITY 0x1e
void psp_vblank_thread_priority(void)
{
	static int done = 0;
	SceKernelThreadInfo info;

	if (done || !psp_keep_loaded())
		return;
	done = 1;
	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	sceKernelReferThreadStatus(0, &info);
	if (psp_keep("vblankslow"))
	{
		platform_log("vertical blank thread: priority 0x%x left as it is (vblankslow)", info.currentPriority);
		return;
	}
	if (sceKernelChangeThreadPriority(0, PSP_VBLANK_PRIORITY) < 0)
		platform_log("vertical blank thread: priority 0x%x; the change to 0x%x was refused", info.currentPriority, PSP_VBLANK_PRIORITY);
	else
		platform_log("vertical blank thread (%s): priority 0x%x, now 0x%x (above the game thread's: vblankslow undoes it)",
			info.name, info.currentPriority, PSP_VBLANK_PRIORITY);
}

/* build 106: the answers remembered, by the word itself. Many asks are made
per object or per draw each frame ("fulllod" for every model's detail
level...): b30's beach asked 13,000 times a second, each a snprintf (under
psp_libc.c's formatting lock) and a search of keep.txt's text, 6.6% of the
game thread in the emulator's PC samples */
#define KEEP_CACHE 128
#define KEEP_CACHE_NAME 24
static struct
{
	/* written by any thread, one at a time (keep_cache_lock): sequence is
	odd while a slot is being written, and a reader trusts only what it read
	between two equal, even values */
	volatile unsigned int sequence;
	int generation, answer;
	char name[KEEP_CACHE_NAME];
} keep_cache[KEEP_CACHE];

static int keep_search(const char *name)
{
	size_t length = strlen(name);
	const char *at = keep_words;

	if (!length)
		return 0;
	while ((at = strstr(at, name)) != NULL)
	{
		/* a whole word: a space on each side (keep_load puts one at each end) */
		if (at > keep_words && at[-1] == ' ' && at[length] == ' ')
			return 1;
		at++;
	}
	return 0;
}

int psp_keep(const char *name)
{
	unsigned int hash = 2166136261u, slot;
	const char *c;
	int answer;

	for (c = name; *c && c - name < KEEP_CACHE_NAME; c++)
		hash = (hash ^ (unsigned char)*c) * 16777619u;
	if (c - name >= KEEP_CACHE_NAME)
		return keep_search(name); /* too long to remember */
	slot = hash % KEEP_CACHE;
	{
		unsigned int before = keep_cache[slot].sequence;

		if (!(before & 1) && keep_cache[slot].generation == keep_generation && strcmp(keep_cache[slot].name, name) == 0)
		{
			answer = keep_cache[slot].answer;
			if (keep_cache[slot].sequence == before)
				return answer;
		}
	}
	answer = keep_search(name);
	/* build 112: not remembered while the work is marked as the Media
	Engine's (gemesim, psp_ge.c): the slot's lock is a system call */
	{
		extern volatile unsigned int psp_me_context;

		if (PSP_ME_WORK())
			return answer;
	}
	/* the last one asked for in a slot is the one remembered there (before
	keep.txt is read, nothing is remembered) */
	if (keep_cache_lock >= 0 && sceKernelWaitSema(keep_cache_lock, 1, NULL) >= 0)
	{
		keep_cache[slot].sequence++;
		keep_cache[slot].generation = keep_generation;
		keep_cache[slot].answer = answer;
		strcpy(keep_cache[slot].name, name);
		keep_cache[slot].sequence++;
		sceKernelSignalSema(keep_cache_lock, 1);
	}
	return answer;
}

/* build 106: a keep word made of a prefix and a number ("anim384"): the
number, or fallback when keep.txt has no such word */
int psp_keep_number(const char *prefix, int fallback)
{
	char word[40];
	const char *at = keep_words;
	size_t length;

	/* build 118: " prefix" put together by hand. snprintf takes the printf
	lock (psp_libc.c: a system call), and the GE layer asks for its numbers
	lazily, the first time from the Media Engine perhaps (gemeoverlap; the
	emulator's ME-strict check found tskip's) */
	word[0] = ' ';
	for (length = 1; prefix[length - 1] && length < sizeof(word) - 1; length++)
		word[length] = prefix[length - 1];
	word[length] = 0;
	while ((at = strstr(at, word)) != NULL)
	{
		at += length;
		if (*at >= '0' && *at <= '9')
			return atoi(at);
	}
	return fallback;
}

/* build 107: the sound cache's size, chosen once at start-up (the cache is
made before the map loads, from the heap): keep.txt's "soundN" in KB
("sound1664"), else SOUND_CACHE_DEFAULT_KB, in 4 KB pages, 512 KB to 4 MB.
Why it grew: b106 on the PSP failed 97 (The Library) to 204 (b30's beach)
sound loads for room with 1,152 KB, each a sound that didn't play. d40's
opening in the emulator (the most sound of the maps tried), to game time
6,650: 1,152 KB failed 5,993 loads; 1,536 KB 144; 1,920 KB none. It also read
less: 1,720 loads, 882, 733.
build 114: 1,792 KB ("sound1536" for b113's). b113's Test 2 on the PSP, with
the placement too: no sound dropped by game time 4,000, against 108 */
#define SOUND_CACHE_DEFAULT_KB 1792
unsigned long psp_sound_cache_bytes(void)
{
	static unsigned long bytes = 0;

	if (!bytes)
	{
		int kb = psp_keep_number("sound", SOUND_CACHE_DEFAULT_KB);

		/* "soundedram" (psp_dsound.c): the eDRAM's room at most */
		extern unsigned long psp_sound_edram_bytes(void);
		unsigned long room = psp_sound_edram_bytes();

		if (kb < 512)
			kb = 512;
		if (kb > 4096)
			kb = 4096;
		bytes = ((unsigned long)kb * 1024) & ~0xFFFul;
		if (room && bytes > room)
			bytes = room;
		platform_log("sound cache: %lu KB (%s)", bytes / 1024,
			room ? "soundedram: the ME's eDRAM" : kb == SOUND_CACHE_DEFAULT_KB ? "the default" : "keep.txt's sound word");
	}
	return bytes;
}

static int profile_compare(const void *a, const void *b)
{
	const struct profile_slot *x = a, *y = b;

	return x->microseconds < y->microseconds ? 1 : x->microseconds > y->microseconds ? -1 : 0;
}

/* writes the window's table to log/profile.txt (and its top lines to the log)
and starts a new one; frames is how many frames the window rendered */
#ifdef PSP_RECON
/* ---------- the measurement round's counters (PSP_RECON=1;
docs/plan_measurement_round.md, the new counters). Their hooks sit in hot
code (each object's update and draw, each particle's birth), so only recon
builds carry them, each behind its keep word */
char *tag_get_name(long tag_index);

/* a tag's name without its folders, for lines that must stay short. A tag
index carries its salt in the high half, and a map's salts are 0x8000 and
over, so a real tag index is negative as a long: NONE (-1) is the only "no
tag", here and in the counters below */
static const char *tag_short_name(long tag)
{
	const char *name = tag != -1 ? tag_get_name(tag) : NULL;
	const char *slash;

	if (!name)
		return "?";
	slash = strrchr(name, '\\');
	return slash ? slash + 1 : name;
}

/* "objcost": each object definition's cost, its updates on the tick (timed
on the sampled ticks, objects.c, as the per-type line) and its drawing
(render_objects.c's per-object call, with the objects it carries, and its
shadow), FIXES_RECON #objcost. Which vehicle costs what (the Pelican, the
dropship, the Banshee against the Warthog and the Scorpion) */
#define OBJCOST_SLOTS 128
static struct { long definition; int type; unsigned long tick_us, updates, draw_us, draws; } objcost[OBJCOST_SLOTS];
static int objcost_on = -1;

void psp_objcost(long definition, int type, int drawing, unsigned long microseconds, unsigned long count)
{
	unsigned int k, n;

	if (objcost_on < 0)
		objcost_on = psp_keep("objcost") != 0;
	if (!objcost_on || definition == -1)
		return;
	k = ((unsigned long)definition * 2654435761u) >> 25;
	for (n = 0; n < OBJCOST_SLOTS; n++, k = (k + 1) % OBJCOST_SLOTS)
	{
		if (!objcost[k].updates && !objcost[k].draws)
		{
			objcost[k].definition = definition;
			objcost[k].type = type;
		}
		if (objcost[k].definition == definition)
		{
			if (drawing)
			{
				objcost[k].draw_us += microseconds;
				objcost[k].draws += count;
			}
			else
			{
				objcost[k].tick_us += microseconds;
				objcost[k].updates += count;
			}
			return;
		}
	}
}

#ifdef PSP_TICK_STEPS
/* "objsteps" (R1 builds: PSP_TICK_STEPS=1 PSP_RECON=1; timemap's request,
lab psp/sierra/timemap/objsteps_request.md): each step's time charged to
the object definition being updated or drawn as well as to the step, so the
profile says what a grunt's tick or a flying Pelican's goes on. The object
is a stack: object_update pushes itself on entry and pops on exit, so a
rider updated inside its vehicle is charged to itself; render_object's call
does the same for drawing. A push or pop closes the step's interval so far
(charged to the object below), so each microsecond goes to the object whose
code ran it. Cells are (definition, step, update or draw): 128 definitions,
the last for any past them, and 48 steps, the last for any past them, in
microseconds of the timed frames, scaled up as the profile's are. Cleared
each profile window and at each map load (definition indices are a map's) */
#define OBJSTEPS_DEFINITIONS 128
#define OBJSTEPS_STEPS 48
#define OBJSTEPS_DEPTH 8
static int objsteps_on = -1;
static struct { long definition; int drawing; } objsteps_stack[OBJSTEPS_DEPTH];
static long objsteps_definition[OBJSTEPS_DEFINITIONS];
static signed char objsteps_type[OBJSTEPS_DEFINITIONS];
static const char *objsteps_step[OBJSTEPS_STEPS];
static unsigned long (*objsteps_us)[OBJSTEPS_STEPS][2] = NULL; /* [OBJSTEPS_DEFINITIONS], from the heap */
static unsigned long objsteps_deep = 0;

static void objsteps_clear(void)
{
	if (objsteps_us)
		memset(objsteps_us, 0, OBJSTEPS_DEFINITIONS * sizeof(*objsteps_us));
	memset(objsteps_definition, 0, sizeof(objsteps_definition));
	memset(objsteps_type, -1, sizeof(objsteps_type));
	memset(objsteps_step, 0, sizeof(objsteps_step));
	objsteps_frames = objsteps_timed_frames = 0;
	objsteps_deep = 0;
}

static int objsteps_ready(void)
{
	if (objsteps_on < 0)
	{
		objsteps_on = psp_keep("objsteps") != 0;
		if (objsteps_on)
		{
			objsteps_us = malloc(OBJSTEPS_DEFINITIONS * sizeof(*objsteps_us));
			if (!objsteps_us)
			{
				platform_log("objsteps: no room for its table (%u KB): off", (unsigned int)(OBJSTEPS_DEFINITIONS * sizeof(*objsteps_us) / 1024));
				objsteps_on = 0;
			}
			else
				objsteps_clear();
		}
	}
	return objsteps_on;
}

static void objsteps_charge(const char *step, unsigned int microseconds)
{
	long definition;
	unsigned int row, column, n;

	if (objsteps_depth > OBJSTEPS_DEPTH || !objsteps_us || !step)
		return;
	definition = objsteps_stack[objsteps_depth - 1].definition;
	row = (((unsigned long)definition * 2654435761u) >> 25) % (OBJSTEPS_DEFINITIONS - 1);
	for (n = 0; n < OBJSTEPS_DEFINITIONS - 1; n++, row = (row + 1) % (OBJSTEPS_DEFINITIONS - 1))
		if (objsteps_type[row] < 0 || objsteps_definition[row] == definition)
			break;
	if (n == OBJSTEPS_DEFINITIONS - 1)
		row = OBJSTEPS_DEFINITIONS - 1;
	else if (objsteps_type[row] < 0)
	{
		objsteps_definition[row] = definition;
		objsteps_type[row] = 12; /* unknown until the push names it */
	}
	for (column = 0; column < OBJSTEPS_STEPS - 1; column++)
		if (!objsteps_step[column] || objsteps_step[column] == step)
			break;
	if (column < OBJSTEPS_STEPS - 1 && !objsteps_step[column])
		objsteps_step[column] = step;
	objsteps_us[row][column][objsteps_stack[objsteps_depth - 1].drawing] += microseconds;
}

/* the step running so far charged to the object on top, before the stack
changes */
static void objsteps_split(void)
{
	unsigned int now;

	if (psp_timers_off || !steps_timed || !profile_last_name || !profile_slots)
		return;
	now = sceKernelGetSystemTimeLow();
	profile_slot_for(profile_last_name)->microseconds += now - profile_last_time;
	if (objsteps_depth > 0)
		objsteps_charge(profile_last_name, now - profile_last_time);
	profile_last_time = now;
}

void psp_objsteps_push(long definition, int type, int drawing)
{
	if (!objsteps_ready())
		return;
	objsteps_split();
	if (objsteps_depth < OBJSTEPS_DEPTH)
	{
		objsteps_stack[objsteps_depth].definition = definition;
		objsteps_stack[objsteps_depth].drawing = drawing != 0;
	}
	else
		objsteps_deep++;
	objsteps_depth++;
	if (objsteps_depth <= OBJSTEPS_DEPTH && objsteps_us)
	{
		/* the type, for the report's rows by type: on the row charge would use */
		unsigned int row = (((unsigned long)definition * 2654435761u) >> 25) % (OBJSTEPS_DEFINITIONS - 1), n;

		for (n = 0; n < OBJSTEPS_DEFINITIONS - 1; n++, row = (row + 1) % (OBJSTEPS_DEFINITIONS - 1))
			if (objsteps_type[row] < 0 || objsteps_definition[row] == definition)
			{
				objsteps_definition[row] = definition;
				objsteps_type[row] = (signed char)(type >= 0 && type < 12 ? type : 12);
				break;
			}
	}
}

void psp_objsteps_pop(void)
{
	if (objsteps_on <= 0 || objsteps_depth <= 0)
		return;
	objsteps_split();
	objsteps_depth--;
}

/* at each map load (psp_showcase_refresh) */
static void objsteps_map_loaded(void)
{
	if (objsteps_on > 0)
		objsteps_clear();
	objsteps_depth = 0;
	memset(objcost, 0, sizeof(objcost));
}

/* one side of a row: its largest steps, in ms a tick (update) or a frame (draw) */
static int objsteps_side(char *out, int room, unsigned long (*cells)[2], int drawing, double scale, double per)
{
	int length = 0, shown, column;
	unsigned char taken[OBJSTEPS_STEPS] = { 0 };

	for (shown = 0; shown < 8; shown++)
	{
		int best = -1;

		for (column = 0; column < OBJSTEPS_STEPS; column++)
			if (!taken[column] && cells[column][drawing] && (best < 0 || cells[column][drawing] > cells[best][drawing]))
				best = column;
		if (best < 0 || length > room - 64)
			break;
		taken[best] = 1;
		length += snprintf(out + length, room - length, "%s %s %.3f", shown ? "," : "",
			best == OBJSTEPS_STEPS - 1 ? "(other steps)" : objsteps_step[best] ? objsteps_step[best] : "?",
			per > 0.0 ? cells[best][drawing] * scale / 1000.0 / per : 0.0);
	}
	return length;
}

static void objsteps_report(SceUID file, unsigned long frames, unsigned long ticks)
{
	static const char *const types[13] = { "biped", "vehicle", "weapon", "equipment", "garbage", "projectile", "scenery",
		"machine", "control", "light", "placeholder", "sound scenery", "?" };
	static unsigned long by_type[13][OBJSTEPS_STEPS][2];
	static char line[1024];
	double scale = objsteps_timed_frames ? (double)objsteps_frames / objsteps_timed_frames : 1.0;
	unsigned char listed[OBJSTEPS_DEFINITIONS] = { 0 };
	int shown, row, length, column;

	if (objsteps_on <= 0 || !objsteps_us || !(frames || ticks))
		return;
	length = snprintf(line, sizeof(line), "\nobjsteps (ms a tick updating | ms a frame drawing, by step, the costliest definitions first; %lu of %lu frames timed, %lu pushes past %d deep):\n",
		objsteps_timed_frames, objsteps_frames, objsteps_deep, OBJSTEPS_DEPTH);
	if (file >= 0)
		sceIoWrite(file, line, length);
	platform_log("%.*s", length - 2, line + 1);
	for (shown = 0; shown < 16; shown++)
	{
		int best = -1;
		double best_cost = 0.0;

		for (row = 0; row < OBJSTEPS_DEFINITIONS; row++)
		{
			double cost = 0.0;

			if (listed[row] || objsteps_type[row] < 0)
				continue;
			for (column = 0; column < OBJSTEPS_STEPS; column++)
				cost += (ticks ? objsteps_us[row][column][0] / (double)ticks : 0.0) + (frames ? objsteps_us[row][column][1] / (double)frames : 0.0);
			if (cost > best_cost)
			{
				best = row;
				best_cost = cost;
			}
		}
		if (best < 0)
			break;
		listed[best] = 1;
		length = snprintf(line, sizeof(line), "objsteps: %s (%s) | tick:", best == OBJSTEPS_DEFINITIONS - 1 ? "(other definitions)" :
			tag_short_name(objsteps_definition[best]), types[objsteps_type[best] >= 0 && objsteps_type[best] <= 12 ? objsteps_type[best] : 12]);
		length += objsteps_side(line + length, sizeof(line) - length, objsteps_us[best], 0, scale, (double)ticks);
		length += snprintf(line + length, sizeof(line) - length, "; draw:");
		length += objsteps_side(line + length, sizeof(line) - length, objsteps_us[best], 1, scale, (double)frames);
		platform_log("%s", line);
		if (file >= 0 && length < (int)sizeof(line) - 1)
		{
			line[length++] = '\n';
			sceIoWrite(file, line, length);
		}
	}
	/* the rest by type, so the table sums to the whole */
	memset(by_type, 0, sizeof(by_type));
	for (row = 0; row < OBJSTEPS_DEFINITIONS; row++)
		if (!listed[row] && objsteps_type[row] >= 0)
			for (column = 0; column < OBJSTEPS_STEPS; column++)
			{
				by_type[objsteps_type[row]][column][0] += objsteps_us[row][column][0];
				by_type[objsteps_type[row]][column][1] += objsteps_us[row][column][1];
			}
	for (row = 0; row < 13; row++)
	{
		int any = 0;

		for (column = 0; column < OBJSTEPS_STEPS; column++)
			any |= by_type[row][column][0] || by_type[row][column][1];
		if (!any)
			continue;
		length = snprintf(line, sizeof(line), "objsteps: the other %s | tick:", types[row]);
		length += objsteps_side(line + length, sizeof(line) - length, by_type[row], 0, scale, (double)ticks);
		length += snprintf(line + length, sizeof(line) - length, "; draw:");
		length += objsteps_side(line + length, sizeof(line) - length, by_type[row], 1, scale, (double)frames);
		platform_log("%s", line);
		if (file >= 0 && length < (int)sizeof(line) - 1)
		{
			line[length++] = '\n';
			sceIoWrite(file, line, length);
		}
	}
	objsteps_clear();
}
#endif

/* "particlecensus": particles asked for, by the effect that asked (or, for
a particle made outside an effect, by the particle), and what became of
each: made, left out unseen at birth, refused over the budget, thinned out
of a burst, or no room in the pool (particles.c, effects.c; FIXES_RECON
#particlecensus) */
#define PARTICLE_CENSUS_SLOTS 64
#define PARTICLE_OUTCOMES 5
static const char *const particle_outcomes[PARTICLE_OUTCOMES] = { "made", "unseen", "budget", "thinned", "pool full" };
static struct { long tag; int from_effect; unsigned long count[PARTICLE_OUTCOMES]; } particle_census[PARTICLE_CENSUS_SLOTS];
static int particle_census_on = -1;
/* the effect whose particle is being made (effects.c), -1 for none */
long psp_particle_effect = -1;

void psp_particle_census(long effect, long particle, int outcome)
{
	long tag = effect != -1 ? effect : particle;
	unsigned int k, n;

	if (particle_census_on < 0)
		particle_census_on = psp_keep("particlecensus") != 0;
	if (!particle_census_on || tag == -1 || outcome < 0 || outcome >= PARTICLE_OUTCOMES)
		return;
	k = ((unsigned long)tag * 2654435761u) >> 26;
	for (n = 0; n < PARTICLE_CENSUS_SLOTS; n++, k = (k + 1) % PARTICLE_CENSUS_SLOTS)
	{
		int o, empty = 1;

		for (o = 0; o < PARTICLE_OUTCOMES; o++)
			if (particle_census[k].count[o])
				empty = 0;
		if (empty)
		{
			particle_census[k].tag = tag;
			particle_census[k].from_effect = effect != -1;
		}
		if (particle_census[k].tag == tag && particle_census[k].from_effect == (effect != -1))
		{
			particle_census[k].count[outcome]++;
			return;
		}
	}
}

static void recon_counters_report(SceUID file, double seconds, unsigned long frames, unsigned long ticks)
{
	static char report[6144];
	char line[512];
	int report_length = 0, length, k, shown;

	if (objcost_on > 0 && (frames || ticks))
	{
		/* the costliest first: ms a tick updating plus ms a frame drawing */
		static const char *const types[] = { "biped", "vehicle", "weapon", "equipment", "garbage", "projectile", "scenery",
			"machine", "control", "light", "placeholder", "sound scenery" };
		double type_tick[12] = { 0 }, type_draw[12] = { 0 };

		report_length += snprintf(report + report_length, sizeof(report) - report_length,
			"objcost: %-36s %-10s %9s %9s %9s %9s\n", "definition", "type", "tick ms", "updates", "frame ms", "draws");
		length = snprintf(line, sizeof(line), "objcost (ms a tick updating, updates a tick | ms a frame drawing, with what it carries, draws a frame):");
		for (k = 0; k < OBJCOST_SLOTS; k++)
			if (objcost[k].type >= 0 && objcost[k].type < 12)
			{
				type_tick[objcost[k].type] += ticks ? objcost[k].tick_us / 1000.0 / ticks : 0.0;
				type_draw[objcost[k].type] += frames ? objcost[k].draw_us / 1000.0 / frames : 0.0;
			}
		for (shown = 0; shown < 24; shown++)
		{
			int best = -1;
			double best_cost = 0.0;

			for (k = 0; k < OBJCOST_SLOTS; k++)
			{
				double cost = (ticks ? objcost[k].tick_us / 1000.0 / ticks : 0.0) + (frames ? objcost[k].draw_us / 1000.0 / frames : 0.0);

				if ((objcost[k].updates || objcost[k].draws) && cost > best_cost)
				{
					best = k;
					best_cost = cost;
				}
			}
			if (best < 0)
				break;
			report_length += snprintf(report + report_length, sizeof(report) - report_length, "objcost: %-36.36s %-10s %9.3f %9.2f %9.3f %9.2f\n",
				tag_short_name(objcost[best].definition), objcost[best].type >= 0 && objcost[best].type < 12 ? types[objcost[best].type] : "?",
				ticks ? objcost[best].tick_us / 1000.0 / ticks : 0.0, ticks ? (double)objcost[best].updates / ticks : 0.0,
				frames ? objcost[best].draw_us / 1000.0 / frames : 0.0, frames ? (double)objcost[best].draws / frames : 0.0);
			if (shown < 6 && length < (int)sizeof(line) - 80)
				length += snprintf(line + length, sizeof(line) - length, " %.24s %.2f, %.1f | %.2f, %.1f;", tag_short_name(objcost[best].definition),
					ticks ? objcost[best].tick_us / 1000.0 / ticks : 0.0, ticks ? (double)objcost[best].updates / ticks : 0.0,
					frames ? objcost[best].draw_us / 1000.0 / frames : 0.0, frames ? (double)objcost[best].draws / frames : 0.0);
			objcost[best].updates = objcost[best].draws = 0; /* shown: out of the running */
		}
		platform_log("%s", line);
		length = snprintf(line, sizeof(line), "objcost by type (ms a tick updating | ms a frame drawing):");
		for (k = 0; k < 12 && length < (int)sizeof(line) - 48; k++)
			if (type_tick[k] > 0.0 || type_draw[k] > 0.0)
				length += snprintf(line + length, sizeof(line) - length, " %s %.2f | %.2f;", types[k], type_tick[k], type_draw[k]);
		platform_log("%s", line);
		memset(objcost, 0, sizeof(objcost));
	}
	if (particle_census_on > 0)
	{
		unsigned long totals[PARTICLE_OUTCOMES] = { 0 };
		int o;

		for (k = 0; k < PARTICLE_CENSUS_SLOTS; k++)
			for (o = 0; o < PARTICLE_OUTCOMES; o++)
				totals[o] += particle_census[k].count[o];
		length = snprintf(line, sizeof(line), "particlecensus over %.1f s: made %lu, unseen %lu, budget %lu, thinned %lu, pool full %lu; by effect (made/unseen/budget/thinned/full), the most asked first:",
			seconds, totals[0], totals[1], totals[2], totals[3], totals[4]);
		report_length += snprintf(report + report_length, sizeof(report) - report_length,
			"particlecensus: %-36s %6s %8s %8s %8s %8s %8s\n", "effect (or particle)", "effect", particle_outcomes[0], particle_outcomes[1],
			particle_outcomes[2], particle_outcomes[3], particle_outcomes[4]);
		for (shown = 0; shown < 20; shown++)
		{
			int best = -1;
			unsigned long best_asked = 0;

			for (k = 0; k < PARTICLE_CENSUS_SLOTS; k++)
			{
				unsigned long asked = 0;

				for (o = 0; o < PARTICLE_OUTCOMES; o++)
					asked += particle_census[k].count[o];
				if (asked > best_asked)
				{
					best = k;
					best_asked = asked;
				}
			}
			if (best < 0)
				break;
			report_length += snprintf(report + report_length, sizeof(report) - report_length, "particlecensus: %-36.36s %6s %8lu %8lu %8lu %8lu %8lu\n",
				tag_short_name(particle_census[best].tag), particle_census[best].from_effect ? "yes" : "no",
				particle_census[best].count[0], particle_census[best].count[1], particle_census[best].count[2],
				particle_census[best].count[3], particle_census[best].count[4]);
			if (shown < 6 && length < (int)sizeof(line) - 72)
				length += snprintf(line + length, sizeof(line) - length, " %.24s %lu/%lu/%lu/%lu/%lu;", tag_short_name(particle_census[best].tag),
					particle_census[best].count[0], particle_census[best].count[1], particle_census[best].count[2],
					particle_census[best].count[3], particle_census[best].count[4]);
			memset(particle_census[best].count, 0, sizeof(particle_census[best].count));
		}
		platform_log("%s", line);
		memset(particle_census, 0, sizeof(particle_census));
	}
	if (file >= 0 && report_length)
		sceIoWrite(file, report, report_length);
}
#endif

static void profile_report(double seconds, unsigned long frames, unsigned long ticks)
{
	static struct profile_slot copy[PROFILE_SLOTS];
	static int window = 0;
	static char report[8192];
	int report_length;
	unsigned long total = 0;
	int index, used = 0;
	SceUID file;
	char line[160];

	double scale;

	if (!profile_slots)
		return;
	memcpy(copy, profile_slots, sizeof(copy));
	for (index = 0; index < PROFILE_SLOTS; index++)
	{
		profile_slots[index].microseconds = 0;
		profile_slots[index].calls = 0;
	}
	/* the untimed frames' share, as the timed ones spent it */
	scale = steps_window_timed ? (double)steps_window_frames / steps_window_timed : 1.0;
	steps_window_frames = steps_window_timed = 0;
	for (index = 0; index < PROFILE_SLOTS; index++)
	{
		if (copy[index].name && copy[index].microseconds)
		{
			copy[used] = copy[index];
			copy[used].microseconds = (unsigned long)(copy[index].microseconds * scale + 0.5);
			copy[used].calls = (unsigned long)(copy[index].calls * scale + 0.5);
			total += copy[used].microseconds;
			used++;
		}
	}
	qsort(copy, used, sizeof(copy[0]), profile_compare);
	window++;
	file = sceIoOpen(LOG_FOLDER "/profile.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_APPEND, 0777);
	snprintf(line, sizeof(line), "\nwindow %d: %.1f s, %lu frames, %lu ticks, %.1f%% of the time between steps\n"
		"%-44s %9s %7s %9s %9s\n", window, seconds, frames, ticks, total / (seconds * 1e4),
		"step (time until the next)", "ms/frame", "% time", "calls/s", "us/call");
	/* the table goes to the file in one write (build 19) */
	report_length = snprintf(report, sizeof(report), "%s", line);
	for (index = 0; index < used && index < 60; index++)
	{
		snprintf(line, sizeof(line), "%-44s %9.2f %7.1f %9.1f %9.1f\n", copy[index].name,
			frames ? copy[index].microseconds / 1000.0 / frames : 0.0,
			copy[index].microseconds / (seconds * 1e4), copy[index].calls / seconds,
			copy[index].calls ? (double)copy[index].microseconds / copy[index].calls : 0.0);
		if (report_length + (int)strlen(line) < (int)sizeof(report))
			report_length += snprintf(report + report_length, sizeof(report) - report_length, "%s", line);
		if (index < 8)
			platform_log("  %-30.30s %6.2f ms/frame %5.1f%%", copy[index].name,
				frames ? copy[index].microseconds / 1000.0 / frames : 0.0, copy[index].microseconds / (seconds * 1e4));
	}
	if (file >= 0)
		sceIoWrite(file, report, report_length);
	object_report(file, ticks);
#ifdef PSP_RECON
	recon_counters_report(file, seconds, frames, ticks);
#ifdef PSP_TICK_STEPS
	objsteps_report(file, frames, ticks);
#endif
#endif
	if (file >= 0)
		sceIoClose(file);
}

/* ---------- the FPU: a PSP thread starts with its FPU control register at
0x00000e00, the overflow, divide-by-zero and invalid operation traps
enabled. The Xbox (and every other port) runs with all of them masked, and
the game counts on it: in Blood Gulch's first tick, unit_update divides by
a unit's aiming_velocity_maximum, which is 0 for some units, and 0/0 is an
invalid operation. Unmasked, that trap is taken, nothing handles it, and the
PSP freezes and switches itself off (PPSSPP keeps the register but takes no
FPU traps, so it ran on). Every thread masks them: the main thread here, the
game's threads as they start (psp_fixups.py's xbox_kernel.c change) */

void psp_fpu_mask_traps(void)
{
	unsigned int control;

	__asm__ __volatile__("cfc1 %0, $31" : "=r"(control));
	/* enable bits 7-11: inexact, underflow, overflow, divide by zero, invalid */
	control &= ~0x00000F80u;
	__asm__ __volatile__("ctc1 %0, $31" : : "r"(control));
}

/* ---------- exit */

void psp_tex_save_learned(void);
void psp_me_exit(const char *why);
void psp_ge_exit_drain(void);

/* builds 88-104 kept ms0:/halo/log/exit.txt open from start-up to record
how far the HOME exit got. It answered its question (b90: ARK-4 Kommander's
Exit, the usual way to quit, never calls the exit callback at all), and
build 105 gives its descriptor back: b104 run 1 showed the PSP lets the
game hold only about ten files at once */
void psp_sleepguard_end(void);
static int exit_callbacks_registered[3] = { 1, 1, 1 };

/* back to the XMB (or ARK-4's menu), saying why in the log */
static void exit_game(const char *why)
{
	exiting = 1;
	/* the GE's frame and list finished before the ME is stopped under it
	(psp_ge.c, psp_ge_exit_drain); only from the game's own thread: HOME
	comes on the callback thread, mid-way through whatever the game does,
	and geinflight's own failure exits from inside the GE's records */
	if (strcmp(why, "HOME") != 0 && strncmp(why, "geinflight", 10) != 0)
		psp_ge_exit_drain();
	/* the Media Engine first, before any Memory Stick work: the XMB reloads
	over the memory it runs from (an ME left running crashed the PSP on
	HOME), and a file write that blocked ahead of it would hang the exit
	there. psp_me_exit leaves it halted or held in reset, the states the
	firmware's own ME boot starts from */
	PSP_METRICS_ME_EXIT(why);
	platform_log("%s: exiting", why);
	log_flush();
	/* the XMB gets its sleep switch back (psp_sleepguard.c) */
	psp_sleepguard_end();
	/* build 89: no texture save here any more. The learned binds are saved
	during play (psp_tex.c, b88); a save here that half worked (the file
	truncated, the data never written) would wipe the one saved in play */
	sceKernelExitGame();
}

/* for the other units: the multiplayer build's own tests (psp_online.c's
onlinedoneN, for the emulator), the bake check (psp_bakecheck.c) and the
transition check's end (psp_fixups.py, FIXES_TRANSITIONS) */
void psp_exit_game(const char *why)
{
	exit_game(why);
}

static int exit_callback(int a, int b, void *c)
{
	(void)a; (void)b; (void)c;
	exit_game("HOME");
	return 0;
}

/* build 108: a level's end asks for the next map (main_change_map_name,
psp_fixups.py). The stick holds the maps baked for the test, not the whole campaign,
and with the next one missing the game showed its "damaged disc" error and
looped there for good; b107's run of b30 crashed in that loop instead. The
level is finished, so the run ends here: the game exits, the log saying
which map it wanted. scenario_path: "levels\b40\b40" (the map file is
its last part) */
void psp_next_map_check(const char *scenario_path)
{
	const char *name = strrchr(scenario_path, '\\');
	char path[96];
	SceIoStat stat;

	name = name ? name + 1 : scenario_path;
	snprintf(path, sizeof(path), PSP_MAPS_ROOT "/%s.map", name);
	if (sceIoGetstat(path, &stat) >= 0)
		return;
	platform_log("level finished: the next map, %s, isn't on the memory stick (%s)", scenario_path, path);
	exit_game("level finished");
}

/* sleep and wake: the texture cache in the volatile partition gives it back
before the PSP sleeps (psp_tex.c; build 22) */
void psp_tex_suspend(void);
void psp_tex_resume(void);
void psp_geo_suspend(void);
void psp_geo_resume(void);
void psp_io_reopen_all(int shift, char *line, int size);
void psp_me_sleep_stop(void);
void psp_me_wake_request(void);
void psp_ge_inflight_room_suspend(void);
void psp_ge_inflight_room_resume(void);
int psp_sleepguard_quiet(void);
void psp_sleepguard_waking(void);
void psp_sleepguard_woken(void);

static int power_callback(int unknown, int flags, void *argument)
{
	(void)unknown; (void)argument;
	/* B41: a sleep halo_sleep.prx put off was readied by the game thread
	itself, at a frame end (psp_sleepguard.c); this half is for a sleep that
	didn't wait (no module, or the firmware's own, as on a flat battery) */
	if ((flags & PSP_POWER_CB_SUSPENDING) && !psp_sleepguard_quiet())
	{
		/* the Media Engine stopped before the sleep: the main CPU runs its
		jobs until the wake's restart has it answering again */
		psp_me_sleep_stop();
		/* geinflight's records, when they live in the partition: the game
		thread holds before its next one (psp_ge.c) */
		psp_ge_inflight_room_suspend();
		/* build 68: the geometry cache's blocks are in the same partition; its
		read in flight is waited for first */
		psp_geo_suspend();
		psp_tex_suspend();
	}
	if (flags & PSP_POWER_CB_RESUME_COMPLETE)
	{
		/* B41: before anything is taken back, no query is let through */
		psp_sleepguard_waking();
		/* first: the files the game streams from died in the sleep, and a
		thread may already be waiting to read (psp_trace.c) */
		if (!psp_keep("noreopen"))
		{
			char line[160];

			psp_io_reopen_all(0, line, sizeof(line));
			platform_log("sleep: awake; %s", line);
		}
		psp_tex_resume();
		psp_geo_resume();
		psp_ge_inflight_room_resume();
		/* not here: this thread can't know the game's threads are out of
		the ME's jobs. The main thread's next frame end starts it */
		psp_me_wake_request();
		/* the game thread, waiting in psp_sleepguard_frame, goes on */
		psp_sleepguard_woken();
	}
	return 0;
}

static int callback_thread(SceSize args, void *argp)
{
	int power, exit;

	(void)args; (void)argp;
	/* build 89: as every other thread (the Xbox ran with the FPU's traps
	masked); this one had them on (review) */
	psp_fpu_mask_traps();
	power = sceKernelCreateCallback("power", power_callback, NULL);
	exit = sceKernelCreateCallback("exit", exit_callback, NULL);
	/* build 89: the results kept for the log (main logs them once the log is
	set up; a line from here would be deleted with the old log) */
	exit_callbacks_registered[0] = scePowerRegisterCallback(-1, power);
	exit_callbacks_registered[1] = exit;
	exit_callbacks_registered[2] = sceKernelRegisterExitCallback(exit);
	sceKernelSleepThreadCB();
	return 0;
}

/* ---------- timing: every game tick and rendered frame is timed
(psp_fixups.py's changes to game_time.c and main.c call these); the
heartbeat reports them every 5 s with the game time, so the log shows how
far the game got, whether it keeps up (150 ticks in 5 s is full speed at
30 Hz), and what a tick and a frame cost on this CPU */

/* the heap's start: how far newlib's heap has grown since (sbrk(0)) is its
high-water mark, since it never gives memory back */
static char *heap_base = NULL;

static volatile unsigned long tick_count, tick_total, tick_maximum;
static volatile unsigned long frame_count, frame_total, frame_maximum;

/* the game's own timers (psp_fixups.py): statistics only, so "notimers"
answers 0 */
unsigned long psp_microseconds(void)
{
	return psp_timers_off ? 0 : sceKernelGetSystemTimeLow();
}

/* build 106: the heartbeat's tick and frame times (game_time.c, main.c;
psp_fixups.py), four reads of the clock a frame, kept with "notimers" */
unsigned long psp_heartbeat_clock(void)
{
	return sceKernelGetSystemTimeLow();
}

/* where the local player is, refreshed on the game's thread once a second
(players.c, appended by psp_fixups.py) for the heartbeat to print */
void psp_player_snapshot(char *text, int size);
unsigned long psp_tag_loaded_calls = 0; /* cache_files.c (psp_fixups.py) */
static char player_text[160] = "player: not yet";
/* the local biped's crouch, 0 to 1 (-1: none), taken with player_text on the
game's thread: the heartbeat reading the player's unit itself raced a
checkpoint revert restoring it, and asserted on a half-loaded unit index.
Read through a call that isn't inlined, so the heartbeat's branches stay
those its profile counted (PSP_PGO_STRICT) */
static volatile float player_crouch_now = -1.0f;

static __attribute__((noinline)) float player_crouch_copy(void)
{
	return player_crouch_now;
}
static unsigned long ticks_since_snapshot;

/* ---------- build 87: "benchrun" (launch menu): a fixed benchmark

Four fixed views of Blood Gulch, each held while a set of switches is tried
one after another, two heartbeat windows (5 s each) per switch: the first
settles (a switch that lets the level's copies go reloads them), the second
is the measurement. Each window starts with a "bench:" line saying what it
is; the heartbeat's own lines (budget, GE, audio, ME) follow it, so
tools/benchparse.py turns a log into a table. About four minutes; the game
carries on as usual after it ("bench: done"). Stand still, touch nothing */
volatile unsigned int psp_heartbeat_windows = 0;
/* phasemark (phase_tick, below): set at a phase edge, it ends the heartbeat's
window there; phase_cut_beat says the beat in hand was one */
volatile int psp_phase_cut = 0;
static int phase_cut_beat = 0;
/* build 89: heartbeats so far (the first 20 are 1 s apart, then 5 s) */
static volatile int heartbeat_beats = 0;
static volatile unsigned long ticks_since_frame;
int psp_audio_setting(int set, int value);
int psp_ge_setting(int which, int set, int value);

static const struct
{
	const char *name;
	float spot[6];
} bench_views[] =
{
	/* build 88: z 0.55 (b87's 0.20 put the feet in the floor: the game
	pushed the player 0.8 units away each time, and b87 placed him again
	every window, which made this view's numbers noisy; b86's spot
	itself reads 0.53) */
	{ "blue base, b86 spot", { 37.74f, -88.22f, 0.55f, 0.25f, 0.97f, 0.0f } },
	{ "red base front", { 95.0f, -172.0f, 0.45f, 0.0f, 1.0f, 0.05f } },
	{ "mid field to blue", { 68.0f, -125.0f, 0.50f, -0.5f, 0.85f, 0.0f } },
	{ "cliff top over the field", { 94.7f, -117.3f, 16.75f, -0.6f, -0.7f, -0.3f } },
};
#define BENCH_VIEWS ((int)(sizeof(bench_views) / sizeof(bench_views[0])))
static const char *const bench_configs[] =
{
	/* build 93: the bus tests. "ME audio off" and "stencils old nudge" are
	settled (no effect, b86-b92) and left out. Each new switch changes only
	what the GE does; the CPU builds every frame the same way */
	/* build 102: "level compact" and "display list in VRAM" settled (no
	effect three times; ~0.5 ms) and left out; in: the texel tests. The
	stand-in texture binds one 16 x 16 texture in VRAM for every draw (or
	only for the lightmap pass), so the GE reads no texels from main RAM;
	"textures one level smaller" is the remedy to try if texels are it */
	"baseline", "GE serial: GE busy time", "GE skips models", "GE skips the level",
	"stand-in texture (no RAM texels)", "stand-in lightmaps", "textures one level smaller", "GE busy on VRAM only",
	/* build 103: the level's vertex copies in VRAM (psp_ge.c's setting 25):
	b102 run 5 put the GE's bus cost on the draws, not their texels, and
	b87/b93's half-size level vertices changed nothing; this takes the
	level's vertex fetches off the main bus altogether. If it recovers what
	"GE skips the level" did, the cost is the fetches themselves */
	"level vertices in VRAM",
};
#define BENCH_LEVEL_VRAM 8
#define BENCH_SERIAL 1
#define BENCH_VRAM_BUSY 7
/* build 93: the GE's busy time at each view (the serial window's wait), the
target for the VRAM-only load */
static float bench_ge_busy_ms[8];
/* build 93: the hardware counters for the drawing phase over the last
heartbeat window (hwprofile_report): memory-wait share, D-cache misses per
million cycles, memory-wait cycles per miss, uncached stores per million */
static float hw_drawing[4];
static int hw_drawing_valid = 0;
#define BENCH_CONFIGS ((int)(sizeof(bench_configs) / sizeof(bench_configs[0])))
static int bench_step = -1;
static unsigned int bench_window_seen = 0;

/* a configuration on (or everything back to the defaults with -1) */
static void bench_apply(int config, int view)
{
	/* the defaults: GE overlapped, float vertices, every draw sent, the list
	in RAM, no VRAM load */
	psp_ge_setting(9, 1, 0);
	if (psp_ge_setting(17, 0, 0) > 0)
		psp_ge_setting(17, 1, 0);
	psp_ge_setting(20, 1, 0);
	psp_ge_setting(21, 1, 0);
	psp_ge_setting(22, 1, 0);
	psp_ge_setting(23, 1, 0);
	psp_ge_setting(24, 1, 0);
	psp_ge_setting(25, 1, 0);
	if (config == BENCH_SERIAL)
		psp_ge_setting(9, 1, 1);
	else if (config == 2)
		psp_ge_setting(20, 1, 2);	/* models */
	else if (config == 3)
		psp_ge_setting(20, 1, 1);	/* the level */
	else if (config == 4)
		psp_ge_setting(23, 1, 1);	/* stand-in texture, every bind */
	else if (config == 5)
		psp_ge_setting(23, 1, 2);	/* stand-in texture, lightmaps */
	else if (config == 6)
		psp_ge_setting(24, 1, 1);	/* one level smaller */
	else if (config == BENCH_LEVEL_VRAM)
		psp_ge_setting(25, 1, 1);	/* the level's vertices in VRAM */
	else if (config == BENCH_VRAM_BUSY)
	{
		/* the load's size found in the settling window (the GE alone, timed),
		then held for the measured one; the target is this view's serial
		window's GE time (tenths of a ms; 15 ms if it is missing) */
		float target = view >= 0 && view < 8 && bench_ge_busy_ms[view] > 1.0f ? bench_ge_busy_ms[view] : 15.0f;

		psp_ge_setting(22, 1, (int)(target * 10.0f));
	}
}

static void bench_tick(void)
{
	extern void psp_player_teleport(float x, float y, float z, float fx, float fy, float fz);
	static int asked = -1;
	int view, config, half;

	if (asked < 0)
		asked = psp_keep("benchrun");
	if (!asked || bench_step >= BENCH_VIEWS * BENCH_CONFIGS * 2)
		return;
	if (psp_heartbeat_windows == bench_window_seen)
		return;
	/* a heartbeat window ended: the next step */
	bench_window_seen = psp_heartbeat_windows;
	/* the map settles first: build 89, three windows of the 5 s cadence
	(b87-b88 counted the first 20 s's 1 s beats as windows, so view 0's
	switches ran on 1 s windows right after the map loaded; review) */
	{
		static unsigned int windows_at_five = 0;

		if (bench_step < 0)
		{
			if (heartbeat_beats <= 20)
				return;
			if (!windows_at_five)
				windows_at_five = psp_heartbeat_windows;
			if (psp_heartbeat_windows < windows_at_five + 3)
				return;
		}
	}
	if (bench_step >= 0 && bench_step % 2)
	{
		/* the measured window that just ended, from the heartbeat's own
		figures for it (one self-contained line: the log's lines from
		different threads can come out of order) */
		extern float psp_me_busy_percent(void);

		int result_view = bench_step / (BENCH_CONFIGS * 2), result_config = (bench_step / 2) % BENCH_CONFIGS;
		char counters[240] = "";

		if (result_config == BENCH_SERIAL && result_view < 8)
			bench_ge_busy_ms[result_view] = psp_budget_last[5];
		/* build 93: the drawing phase's hardware counters for the window */
		if (hw_drawing_valid)
			snprintf(counters, sizeof(counters), " | drawing: memory wait %.1f%%, D-misses %.0f a M cycles, %.0f cycles a miss, uncached stores %.0f a M",
				hw_drawing[0], hw_drawing[1], hw_drawing[2], hw_drawing[3]);
		if (result_config == BENCH_LEVEL_VRAM)
		{
			/* build 103: did the copies fit? */
			size_t used = strlen(counters);

			snprintf(counters + used, sizeof(counters) - used, " | VRAM vertices %d KB, %d copies, %d refused",
				psp_ge_setting(26, 0, 0), psp_ge_setting(26, 0, 1), psp_ge_setting(26, 0, 2));
		}
		platform_log("bench: RESULT view %d (%s) | switch %d (%s) | %.1f ms a frame, %.1f fps | ticks %.1f ms | frame %.1f ms (renderer CPU %.1f, GE wait %.1f) | %.1f ticks/s | ME %.1f%% busy%s",
			result_view, bench_views[result_view].name, result_config,
			bench_configs[result_config], psp_budget_last[0], psp_budget_last[1], psp_budget_last[2],
			psp_budget_last[3], psp_budget_last[4], psp_budget_last[5], psp_budget_last[6], psp_me_busy_percent(), counters);
	}
	bench_step++;
	if (bench_step >= BENCH_VIEWS * BENCH_CONFIGS * 2)
	{
		bench_apply(-1, -1);
		platform_log("bench: done (%d views x %d switches); everything back to the defaults", BENCH_VIEWS, BENCH_CONFIGS);
		return;
	}
	view = bench_step / (BENCH_CONFIGS * 2);
	config = (bench_step / 2) % BENCH_CONFIGS;
	half = bench_step % 2;
	if (!half)
		bench_apply(config, view);
	else if (config == BENCH_VRAM_BUSY)
		psp_ge_setting(22, 1, -1);	/* the load's size held: measure */
	/* build 88: put at the spot when the view changes, and again only if the
	game has moved the player off it (b87: every window, and the tester saw the
	teleport repeat; each one can hitch the window it starts) */
	{
		extern int psp_player_origin(float *out);
		float at[3];
		int placed = 0;

		if (bench_step % (BENCH_CONFIGS * 2) == 0 || !psp_player_origin(at) ||
			(at[0] - bench_views[view].spot[0]) * (at[0] - bench_views[view].spot[0]) +
			(at[1] - bench_views[view].spot[1]) * (at[1] - bench_views[view].spot[1]) > 0.3f * 0.3f)
		{
			psp_player_teleport(bench_views[view].spot[0], bench_views[view].spot[1], bench_views[view].spot[2],
				bench_views[view].spot[3], bench_views[view].spot[4], bench_views[view].spot[5]);
			placed = 1;
		}
		platform_log("bench: window %d: view %d (%s), switch %d (%s), %s%s", bench_step, view, bench_views[view].name, config,
			bench_configs[config], half ? "MEASURED" : "settling", placed ? " (player placed)" : "");
	}
}

/* ---------- build 112: "ceilingbench" (keep word): what each big part costs

A structural lever is worth weeks only where the part it speeds up is big.
This bench measures each part's ceiling on the PSP itself, cache and bus
effects included: from 90 s of play, wherever the player stands, the big
parts are switched off one at a time, two heartbeat windows each (the first
settles, the second is measured), and the cycle runs three times to average
out a live scene. About three minutes. Measurement only: while a part is off
the game is wrong (units freeze, models vanish); each part comes back at the
end of its window, and everything at "bench: done". Stand still, touch
nothing. The frame throttle is off for the run, as for benchrun (padding to
vertical blanks would hide the savings).

The lines are benchrun's ("bench: RESULT view N (...) | switch N (...) |
..."), the views being the cycles, so tools/benchparse.py reads them;
--by-switch averages the cycles. The switches (psp_bench_skip, read in
game.c and render.c through psp_fixups.py's FIXES_112):
- AI off: ai_update;
- objects frozen: objects_update (every object's update: bipeds, vehicles,
  weapons, scenery...);
- effects off: effects_update, particles_update, render_particles;
- models not drawn: render_objects, render_object_shadows;
- level not drawn: structure_render_lightmaps,
  structure_render_transparent_geometry;
- GE layer skipped: every draw the engine hands psp_ge.c, dropped on arrival
  (the engine's own drawing work stays): the most the GE layer's move to the
  Media Engine could save. */
int psp_bench_skip = 0;
static const struct
{
	const char *name;
	int skip;
} ceiling_configs[] =
{
	{ "baseline", 0 },
	{ "AI off", PSP_SKIP_AI },
	{ "objects frozen", PSP_SKIP_OBJECTS },
	{ "effects off", PSP_SKIP_EFFECTS },
	{ "models not drawn", PSP_SKIP_MODELS },
	{ "level not drawn", PSP_SKIP_LEVEL },
	{ "GE layer skipped", PSP_SKIP_GE },
};
#define CEILING_CONFIGS ((int)(sizeof(ceiling_configs) / sizeof(ceiling_configs[0])))
#define CEILING_CYCLES 3
#define CEILING_START_TICKS (90 * 30)
static int ceiling_step = -1;
static unsigned int ceiling_window_seen = 0;

static void ceiling_tick(void)
{
	static int asked = -1;
	int config;

	if (asked < 0)
	{
		asked = psp_keep("ceilingbench");
		if (asked && psp_keep("benchrun"))
		{
			platform_log("bench: ceilingbench left out: benchrun is on (one bench a run)");
			asked = 0;
		}
	}
	if (!asked || ceiling_step >= CEILING_CONFIGS * CEILING_CYCLES * 2)
		return;
	if (psp_heartbeat_windows == ceiling_window_seen)
		return;
	ceiling_window_seen = psp_heartbeat_windows;
	if (ceiling_step < 0 && (heartbeat_beats <= 20 || psp_game_time_peek() < CEILING_START_TICKS))
		return;
	if (ceiling_step >= 0 && ceiling_step % 2)
	{
		/* the measured window that just ended (benchrun's line, so
		benchparse.py reads it) */
		extern float psp_me_busy_percent(void);
		int cycle = ceiling_step / (CEILING_CONFIGS * 2), done = (ceiling_step / 2) % CEILING_CONFIGS;
		char counters[120] = "";

		if (hw_drawing_valid)
			snprintf(counters, sizeof(counters), " | drawing: memory wait %.1f%%, D-misses %.0f a M cycles",
				hw_drawing[0], hw_drawing[1]);
		platform_log("bench: RESULT view %d (ceiling cycle %d) | switch %d (%s) | %.1f ms a frame, %.1f fps | ticks %.1f ms | frame %.1f ms (renderer CPU %.1f, GE wait %.1f) | %.1f ticks/s | ME %.1f%% busy%s",
			cycle, cycle + 1, done, ceiling_configs[done].name, psp_budget_last[0], psp_budget_last[1], psp_budget_last[2],
			psp_budget_last[3], psp_budget_last[4], psp_budget_last[5], psp_budget_last[6], psp_me_busy_percent(), counters);
	}
	ceiling_step++;
	if (ceiling_step >= CEILING_CONFIGS * CEILING_CYCLES * 2)
	{
		psp_bench_skip = 0;
		platform_log("bench: done (ceilingbench, %d cycles x %d switches); everything back on", CEILING_CYCLES, CEILING_CONFIGS);
		return;
	}
	config = (ceiling_step / 2) % CEILING_CONFIGS;
	psp_bench_skip = ceiling_configs[config].skip;
	platform_log("bench: window %d: ceiling cycle %d, switch %d (%s), %s", ceiling_step,
		ceiling_step / (CEILING_CONFIGS * 2) + 1, config, ceiling_configs[config].name, ceiling_step % 2 ? "MEASURED" : "settling");
}

/* build 95: every thread's stack, its size and how deep it has been (the
kernel fills a new stack with 0xFF; the first word from the bottom that
isn't is the deepest the thread has reached). For the memory plan: 1 MB of
user RAM above the tag window goes to stacks */
static void stack_report(void)
{
	SceUID ids[64];
	int count = 0, k, length = 0;
	char line[480];
	unsigned long total = 0, used_total = 0;

	if (sceKernelGetThreadmanIdList(SCE_KERNEL_TMID_Thread, ids, 64, &count) < 0)
		return;
	for (k = 0; k < count && k < 64; k++)
	{
		SceKernelThreadInfo info;
		const unsigned int *word;
		unsigned int words, w;

		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		if (sceKernelReferThreadStatus(ids[k], &info) < 0 || !info.stack || info.stackSize <= 0)
			continue;
		/* build 97: our own threads' stacks only. b95/b96 read every
		thread's, the system's idle threads too, whose stacks are kernel
		memory: on the PSP a user-mode read there is an exception (b96's
		runs 1 and 2 both died at tick 75, the first time this ran; the
		emulator doesn't protect kernel memory) */
		if (!(info.attr & PSP_THREAD_ATTR_USER) || (unsigned int)info.stack < 0x08800000u ||
			(unsigned int)info.stack + (unsigned int)info.stackSize > 0x0A000000u)
			continue;
		word = (const unsigned int *)info.stack;
		words = (unsigned int)info.stackSize / 4;
		/* the bottom 16 bytes hold the kernel's own guard words (the
		thread's id): skipped */
		for (w = 4; w < words && word[w] == 0xFFFFFFFFu; w++)
			;
		total += info.stackSize;
		used_total += (words - w) * 4;
		length += snprintf(line + length, sizeof(line) - length, " %.12s %d/%d;", info.name,
			(int)((words - w) * 4 / 1024), info.stackSize / 1024);
		if (length > (int)sizeof(line) - 40)
		{
			platform_log("stacks (KB used deepest/size):%s", line);
			length = 0;
			line[0] = 0;
		}
	}
	platform_log("stacks (KB used deepest/size):%s total %lu KB reserved, %lu KB ever used", line, total / 1024, used_total / 1024);
}

static void bus_probe(const char *when); /* build 105, below */


/* build 117: keep.txt's "ladderN": the campaign's openings one after
another, N seconds of game time each, with nobody at the controls
(2026-10-12: a test session that runs itself). N seconds into a map, the
next one in the campaign's order loads; one that isn't on the stick is
passed over, and after the last the game exits. Start it from the launch
menu on any campaign map. The map's name comes from the cache file's
header as it opens (psp_map_opened, psp_fixups.py) */
static char current_map[32];

/* the main menu (ui.map) is the hardware's showcase (CLAUDE.md): the switches
that raise its quality read this, so they never reach a level. "noshowcase"
draws the menu as the levels are drawn */
int psp_showcase_map = 0;

void psp_map_opened(const char *name)
{
	snprintf(current_map, sizeof(current_map), "%s", name ? name : "");
	psp_showcase_map = !strcmp(current_map, "ui") && !psp_keep("noshowcase");
}

/* every map load, from the geometry pack's open (psp_geo.c): a map whose
cache file is still open from an earlier visit (the menu again after a
level) never reaches psp_map_opened, and the flag kept the level's 0, so
the menu took a level's carves while it loaded its full-size pack (B29).
"showcaseonce" leaves the flag to psp_map_opened alone, as before */
/* counts map loads, so a cache keyed by a map's tag addresses knows when
they've changed (psp_fixups.py, the Chief parts' kinds) */
int psp_map_generation = 0;

void psp_showcase_refresh(const char *name)
{
#if defined(PSP_RECON) && defined(PSP_TICK_STEPS)
	objsteps_map_loaded();
#endif
	psp_map_generation++;
	if (!psp_keep("showcaseonce"))
		psp_showcase_map = name && !strcmp(name, "ui") && !psp_keep("noshowcase");
}

static int map_on_stick(const char *name)
{
	char path[96];
	SceIoStat stat;

	snprintf(path, sizeof(path), DATA_ROOT "/maps/%s.map", name);
	return sceIoGetstat(path, &stat) >= 0;
}

/* Loads the map after `from` in the campaign's order, passing over any not
on the stick; 0 when `from` was the last one there */
static int ladder_load_next(const char *from)
{
	static const char *const order[] = { "a10", "a30", "a50", "b30", "b40", "c10", "c20", "c40", "d20", "d40" };
	int k, at = -1;

	for (k = 0; k < (int)(sizeof(order) / sizeof(order[0])); k++)
		if (!strcmp(order[k], from))
			at = k;
	for (k = at + 1; k < (int)(sizeof(order) / sizeof(order[0])); k++)
	{
		char skip[24];

		/* "ladderskip<map>" (the time map's intro bench): passed over, as a map
		not on the stick is */
		snprintf(skip, sizeof(skip), "ladderskip%s", order[k]);
		if (psp_keep(skip))
			continue;
		if (map_on_stick(order[k]))
		{
			extern unsigned char hs_compile_and_evaluate(const char *source);
			char command[64];

			snprintf(command, sizeof(command), "map_name levels\\%s\\%s", order[k], order[k]);
			platform_log("ladder: %s's turn is over; %s next", from, order[k]);
			hs_compile_and_evaluate(command);
			return 1;
		}
	}
	return 0;
}

/* keep.txt's "sceneladder": the openings' cutscenes only, for a showcase
(2026-10-14). A map's turn ends once its cutscene has been over for
SCENE_SETTLE of game time: several openings run one scene into the next
with a moment's gap, and the wait keeps a chain whole. A map whose
cutscene hasn't started by SCENE_WAIT has none, and is passed over;
SCENE_LIMIT is a backstop for a scene that never ends. After the last map
the game plays on rather than quitting: a game that exits by itself can
leave the PSP hung, and a video wants to end on the picture */
#define SCENE_WAIT (30 * 30)
#define SCENE_SETTLE (3 * 30)
#define SCENE_LIMIT (15 * 60 * 30)

static void scene_ladder_tick(unsigned long ticks_seen)
{
	extern unsigned char cinematic_in_progress(void); /* the engine's boolean */
	static char map[32];
	static unsigned long started, last_scene;
	static int seen_scene, scene_over, asked, done;

	if (done || !current_map[0] || !strcmp(current_map, "ui"))
		return;
	if (strcmp(map, current_map) != 0)
	{
		snprintf(map, sizeof(map), "%s", current_map);
		started = ticks_seen;
		seen_scene = scene_over = asked = 0;
		platform_log("scene ladder: %s, waiting for its cutscene", map);
		return;
	}
	if (asked)
		return; /* the next map is loading */
	if (cinematic_in_progress())
	{
		if (!seen_scene)
			platform_log("scene ladder: %s's cutscene began at game time %lu", map, ticks_seen - started);
		seen_scene = 1;
		scene_over = 0;
		last_scene = ticks_seen;
		if (ticks_seen - started < SCENE_LIMIT)
			return;
		platform_log("scene ladder: %s's cutscene still running at the limit", map);
	}
	else if (seen_scene && !scene_over)
	{
		/* the phase edge the time map reads (its stretch spec) */
		scene_over = 1;
		platform_log("scene ladder: %s's cutscene ended at game time %lu", map, ticks_seen - started);
		return;
	}
	else if (seen_scene ? ticks_seen - last_scene < SCENE_SETTLE : ticks_seen - started < SCENE_WAIT)
		return;
	else if (!seen_scene)
		platform_log("scene ladder: %s has no opening cutscene", map);
	asked = 1;
	if (!ladder_load_next(map))
	{
		/* "sceneladderexit": the game quits after the last map, so a rig run
		ends in the game (the agent, ARK-4's launcher, takes back a game that
		quits by itself); by default it plays on, for a showcase video */
		if (psp_keep("sceneladderexit"))
		{
			platform_log("scene ladder: %s was the last map on the stick; quitting (sceneladderexit)", map);
			exit_game("scene ladder done");
		}
		platform_log("scene ladder: %s was the last map on the stick; playing on", map);
		done = 1;
	}
}

/* keep.txt's "phasemark" (a time map of each level's phases): a log line
at each of a level's phase edges (the map loaded, its cutscene begun and
ended, a BSP switch), with the heartbeat's window cut there, so each window's
averages (the heartbeat, the step profile and the lines that report with it)
hold one phase rather than straddling two. Reads only; play is unchanged */
static void phase_mark(const char *map, const char *edge)
{
	extern unsigned long psp_ge_frame_number(void);

	platform_log("phase: %s, %s at game time %ld, frame %lu", map, edge, psp_game_time_peek(), psp_ge_frame_number());
	psp_phase_cut = 1;
	PSP_LANE_ARM(); /* "lanetrace" (psp_lanetrace.c): a window of frames from here */
	if (log_wake >= 0)
		sceKernelSignalSema(log_wake, 1);
}

static void phase_tick(void)
{
	extern unsigned char cinematic_in_progress(void);
	extern short global_structure_bsp_index;
	static int on = -1, in_scene;
	static short bsp;
	static char map[32];
	int scene;

	if (on < 0)
		on = psp_keep("phasemark");
	if (!on || !current_map[0])
		return;
	if (strcmp(map, current_map) != 0)
	{
		snprintf(map, sizeof(map), "%s", current_map);
		in_scene = 0;
		bsp = global_structure_bsp_index;
		phase_mark(map, "map loaded");
		return;
	}
	scene = cinematic_in_progress() != 0;
	if (scene != in_scene)
	{
		in_scene = scene;
		phase_mark(map, scene ? "cutscene began" : "cutscene ended");
	}
	if (global_structure_bsp_index != bsp)
	{
		char edge[32];

		snprintf(edge, sizeof(edge), "BSP %d -> %d", bsp, global_structure_bsp_index);
		bsp = global_structure_bsp_index;
		phase_mark(map, edge);
	}
}

static void ladder_tick(unsigned long ticks_seen)
{
	static long ladder_ticks = -1;
	static unsigned long ladder_next = 0;
	static char ladder_map[32];
	static int scenes = -1;

	if (scenes < 0)
		scenes = psp_keep("sceneladder");
	if (scenes)
	{
		scene_ladder_tick(ticks_seen);
		return;
	}
	if (ladder_ticks < 0)
		ladder_ticks = (long)psp_keep_number("ladder", 0) * 30;
	if (ladder_ticks <= 0 || !current_map[0] || !strcmp(current_map, "ui"))
		return;
	if (strcmp(ladder_map, current_map) != 0)
	{
		/* a map has started: its turn runs from here */
		snprintf(ladder_map, sizeof(ladder_map), "%s", current_map);
		ladder_next = ticks_seen + (unsigned long)ladder_ticks;
		platform_log("ladder: %s for %ld s of game time", ladder_map, ladder_ticks / 30);
		return;
	}
	if (ticks_seen != ladder_next)
		return;
	if (ladder_load_next(ladder_map))
		return;
	platform_log("ladder: %s was the last map on the stick", ladder_map);
	exit_game("ladder done");
}

/* teleport.txt's and route.txt's spot: the player put there and turned, or,
with "flycam" (debug tools), the debug camera (debug_camera_load's flying
camera) put there looking along the direction and the player left alone.
The spot is then the eye, as a screenshot's "from" and "looking" give it,
and neither a cutscene's camera nor where the player is gets in the way:
a10's corridors can be seen while its opening still plays, their BSP put in
with commands.txt's switch_bsp */
static void spot_visit(const float *spot)
{
	extern void psp_player_teleport(float x, float y, float z, float fx, float fy, float fz);
#ifdef PSP_DEBUG_TOOLS
	static int fly = -1;

	if (fly < 0)
		fly = psp_keep("flycam") != 0;
	if (fly)
	{
		extern void psp_director_fly(float x, float y, float z, float fx, float fy, float fz); /* director.c (psp_fixups.py) */

		psp_director_fly(spot[0], spot[1], spot[2], spot[3], spot[4], spot[5]);
		return;
	}
#endif
	psp_player_teleport(spot[0], spot[1], spot[2], spot[3], spot[4], spot[5]);
}

/* build 126: "frametrace" (psp_frametrace.c) */
void psp_frametrace_tick(unsigned long microseconds);
void psp_frametrace_frame(unsigned long draw_us, unsigned long ticks, unsigned long end_us);

void psp_tick_account(unsigned long microseconds)
{
	static unsigned long ticks_seen = 0;

	ticks_since_frame++;
	bench_tick();
#ifdef PSP_MULTIPLAYER
	{
		extern void psp_net_tick(void);

		psp_net_tick();
	}
#endif
	ceiling_tick(); /* build 112 */
	/* build 95: the high-water marks, from the game thread every 5 s */
	if (ticks_seen % 150 == 75)
	{
		extern void psp_sound_cache_report(void);
		extern void psp_dynamic_geometry_report(void);

		psp_sound_cache_report();
		psp_dynamic_geometry_report();
		{
			/* build 105: the heap's state and its pieces (psp_libc.c) */
			extern void psp_heap_line(char *line, int size);
			char line[200];

			psp_heap_line(line, sizeof(line));
			platform_log("%s", line);
		}
		{
			/* build 106: the busiest printf formats ("printfcount") */
			extern void psp_printf_report(void);

			psp_printf_report();
		}
		if (ticks_seen % 900 == 75)
			stack_report();
		if (ticks_seen % 1800 == 75)
		{
			/* "poolpeaks": the peaks so far, every 60 s of game time */
			extern void psp_pool_peaks_report(const char *label, int level_end) __attribute__((weak));

			if (psp_pool_peaks_report)
				psp_pool_peaks_report("every 60 s", 0);
		}
	}

	/* build 105: the bus probe's second reading, 10 s into play */
	if (ticks_seen == 300)
		bus_probe("in play, the GE busy");

	/* build 99: "crashplay": a deliberate fault 20 s into play, to check
	the crash handler's report shows on the game's screen */
	if (ticks_seen == 600 && psp_keep("crashdraw"))
	{
		extern void psp_crash_draw_test(void);

		psp_crash_draw_test();
	}
	if (ticks_seen == 600 && psp_keep("crashplay"))
	{
		platform_log("crashplay: storing to address 4 (the crash handler's report should show on screen)");
		psp_log_flush();
		*(volatile int *)4 = 0;
	}

	/* build 103: "handletest": 15 s in, opens one small file again and again
	until the kernel refuses, then closes them all: how many descriptors were
	free at that moment, and a check of the handle table's out-of-handles
	path. Build 105: the file is created first and then opened read-only;
	b104 asked for read-only and create in one call, which the PSP's FAT
	driver refuses (0x80010016, invalid argument) though the emulator allows it */
	if (ticks_seen == 450 && psp_keep("handletest"))
	{
		static SceUID held[80];
		int count = 0, k;
		SceUID fd = sceIoOpen(LOG_FOLDER "/handletest.txt", PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);

		if (fd >= 0)
			sceIoClose(fd);
		while (count < 80)
		{
			fd = sceIoOpen(LOG_FOLDER "/handletest.txt", PSP_O_RDONLY, 0777);
			if (fd < 0)
				break;
			held[count++] = fd;
		}
		platform_log("handletest: %d more handles opened before the kernel refused (0x%08x)", count, (unsigned)fd);
		for (k = 0; k < count; k++)
			sceIoClose(held[k]);
		{
			extern void psp_handles_reserve(void);

			psp_handles_reserve();
		}
	}

	/* keep.txt's "savetest": a checkpoint 10 s in (once), timed, the way the
	campaign makes them (game_state_save writes the whole game state) */
	++ticks_seen;
	/* ms0:/halo/commands.txt: console commands run 10 s after the game starts (one
a line; e.g. cheat_active_camouflage_local_player 0), for testing what a
map doesn't show by itself (build 30). Build 106: a line starting "@N "
runs at game tick N instead (e.g. "@900 ai_kill enc2"), for tests that need
steps in order: the console has no way to wait */
	{
#define TIMED_COMMANDS 16
		static struct { unsigned long tick; char text[200]; } timed[TIMED_COMMANDS];
		static int timed_count = 0;
		extern unsigned char hs_compile_and_evaluate(const char *source);
		int k;

		if (ticks_seen == 300)
		{
			FILE *file = fopen(DATA_ROOT "/commands.txt", "r");

			if (file)
			{
				char line[256];

				while (fgets(line, sizeof(line), file))
				{
					size_t length = strlen(line);
					char *text = line;
					unsigned long at = 0;

					while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
						line[--length] = 0;
					if (line[0] == '@')
					{
						at = strtoul(line + 1, &text, 10);
						while (*text == ' ')
							text++;
					}
					if (!*text)
						continue;
					if (at > ticks_seen && timed_count < TIMED_COMMANDS)
					{
						timed[timed_count].tick = at;
						snprintf(timed[timed_count].text, sizeof(timed[timed_count].text), "%s", text);
						timed_count++;
						platform_log("commands.txt: at tick %lu: %s", at, text);
					}
					else
					{
						platform_log("commands.txt: %s", text);
						hs_compile_and_evaluate(text);
					}
				}
				fclose(file);
			}
			else
				/* build 103: said either way (b102 run 6: nothing logged, and no
				way to tell a missing file from lines that didn't run) */
				platform_log("commands.txt: none (no %s)", DATA_ROOT "/commands.txt");
		}
		for (k = 0; k < timed_count; k++)
			if (timed[k].tick == ticks_seen)
			{
				platform_log("commands.txt: tick %lu: %s", ticks_seen, timed[k].text);
				hs_compile_and_evaluate(timed[k].text);
			}
	}
	/* build 59: ms0:/halo/teleport.txt, "x y z forward_x forward_y forward_z"
	(the player's feet, as the heartbeat prints them, and a direction as a
	screenshot's "looking"): the player put there 11 s in, and again each
	5 s for a minute (the game may move a unit that lands in the air) */
	if (ticks_seen >= 330 && ticks_seen <= 2130 && (ticks_seen - 330) % 150 == 0)
	{
		FILE *file = fopen(DATA_ROOT "/teleport.txt", "r");

		if (file)
		{
			float p[6];

			if (fscanf(file, "%f %f %f %f %f %f", &p[0], &p[1], &p[2], &p[3], &p[4], &p[5]) == 6)
			{
				platform_log("teleport.txt: to %.2f %.2f %.2f looking %.2f %.2f %.2f", p[0], p[1], p[2], p[3], p[4], p[5]);
				spot_visit(p);
			}
			fclose(file);
		}
	}
	/* build 102: ms0:/halo/route.txt, lines of "seconds x y z forward_x
	forward_y forward_z": the player put at each spot in turn, at that many
	seconds of play (game ticks), each line once. A route replayed from a
	hardware log's "player: at" lines (tools/route_from_log.py), so the
	emulator can size the caches on the route actually played */
	{
		static FILE *route = NULL;
		static int route_done = 0;
		static float next[7];
		static int have_next = 0;
		static unsigned long route_count = 0;

		if (!route && !route_done)
		{
			route = fopen(DATA_ROOT "/route.txt", "r");
			if (!route)
				route_done = 1;
		}
		while (route && !route_done)
		{
			if (!have_next)
			{
				if (fscanf(route, "%f %f %f %f %f %f %f", &next[0], &next[1], &next[2], &next[3], &next[4], &next[5], &next[6]) != 7)
				{
					fclose(route);
					route = NULL;
					route_done = 1;
					platform_log("route.txt: done, %lu spots", route_count);
					break;
				}
				have_next = 1;
			}
			if ((float)ticks_seen < next[0] * 30.0f)
				break;
			{
				spot_visit(next + 1);
				route_count++;
				if (route_count % 10 == 1)
					platform_log("route.txt: spot %lu at %.1f s: %.2f %.2f %.2f", route_count, next[0], next[1], next[2], next[3]);
			}
			have_next = 0;
		}
	}
	/* build 70: keep.txt's "colltest": the collision regression rays, 14 s in
	(after the teleport, if any, has settled) and again at 24 s */
	if ((ticks_seen == 420 || ticks_seen == 720) && psp_keep("colltest"))
	{
		extern void psp_collision_test(const float *fixed_origin);
		FILE *file = fopen(DATA_ROOT "/teleport.txt", "r");
		float p[6];
		int fixed = file && fscanf(file, "%f %f %f %f %f %f", &p[0], &p[1], &p[2], &p[3], &p[4], &p[5]) == 6;

		if (file)
			fclose(file);
		/* from teleport.txt's spot when there is one (the same rays whatever
		the player did), else from the player */
		psp_collision_test(fixed ? p : NULL);
	}
	/* keep.txt's "menutest": from the main menu, Blood Gulch after 20 s, the
	way the menu starts a game (a new map's tags over the menu's) */
	if (ticks_seen == 600 && psp_keep("menutest"))
	{
		extern unsigned char hs_compile_and_evaluate(const char *source);

		platform_log("menutest: loading Blood Gulch from the menu");
		hs_compile_and_evaluate("map_name levels\\test\\bloodgulch\\bloodgulch");
	}
	/* keep.txt's "reopentest": the wake's reopening of every file, at 20 s
	of game time without a sleep, with the kernel's descriptor numbers
	moved, so the emulator can show play goes on exactly as without it */
	if (ticks_seen == 600 && psp_keep("reopentest"))
	{
		char line[160];
		void psp_geo_settle(void);

		psp_geo_settle();
		psp_io_reopen_all(1, line, sizeof(line));
		platform_log("reopentest: %s", line);
	}
	/* keep.txt's "mewaketest": the ME held in reset as a sleep holds it, at
	15 s of game time, and the wake's restart asked for 3 s later (it
	happens with "mewake"), so play runs on the main CPU alone between;
	with and without "mewake", play must stay exact (statecrc) */
	if ((ticks_seen == 450 || ticks_seen == 540) && psp_keep("mewaketest"))
	{
		void psp_me_wake_test_sleep(void);
		void psp_me_wake_test_wake(void);

		if (ticks_seen == 450)
			psp_me_wake_test_sleep();
		else
		{
			platform_log("mewaketest: the wake");
			psp_me_wake_test_wake();
		}
	}
	/* keep.txt's "exitatN": back to the menu at N seconds of game time, so
	two emulator runs (the cache models' A/Bs) cover the same ticks and
	their totals compare directly */
	{
		static long exit_tick = -1;

		if (exit_tick < 0)
			exit_tick = (long)psp_keep_number("exitat", 0) * 30;
		if (exit_tick > 0 && (long)ticks_seen == exit_tick)
			exit_game("exitat");
	}
#ifdef PSP_PGO_GENERATE
	{
		/* PSP_PGO=generate (psp_build.py): the hot units' counts written to
		ms0:/halo/gcda at "gcovdumpN" seconds of game time, once, for a
		PSP_PGO=use build. libgcov names each file by its object's path
		under "/pgo" (the build's -fprofile-generate=/pgo); GCOV_PREFIX_STRIP
		drops that and GCOV_PREFIX puts the stick's folder in front */
		static long dump_tick = -1;

		if (dump_tick < 0)
			dump_tick = (long)psp_keep_number("gcovdump", 0) * 30;
		if (dump_tick > 0 && (long)ticks_seen == dump_tick)
		{
			extern void __gcov_dump(void);
			unsigned long start = psp_microseconds();

			char saved[256];

			sceIoMkdir(DATA_ROOT "/gcda", 0777);
			setenv("GCOV_PREFIX", DATA_ROOT "/gcda", 1);
			setenv("GCOV_PREFIX_STRIP", "1", 1);
			/* and the folder made the current one: libgcov takes only an
			absolute prefix ("/..."), and "ms0:/..." may not be one to it: the
			first profile run's counts probably went to the folder the game started
			in. Without the prefix it writes "./<name>" */
			if (!getcwd(saved, sizeof(saved)))
				saved[0] = '\0';
			chdir(DATA_ROOT "/gcda");
			/* libgcov says what goes wrong on stderr: into a file, read back
			into the log below */
			freopen(DATA_ROOT "/gcda_errors.txt", "w", stderr);
			__gcov_dump();
			fflush(stderr);
			if (saved[0])
				chdir(saved);
			platform_log("gcov: the hot units' counts written to %s/gcda at tick %lu, in %lu ms", DATA_ROOT,
				ticks_seen, (psp_microseconds() - start) / 1000);
			{
				SceUID folder = sceIoDopen(DATA_ROOT "/gcda");
				SceIoDirent entry;
				int files = 0, shown = 0;

				if (folder >= 0)
				{
					memset(&entry, 0, sizeof(entry));
					while (sceIoDread(folder, &entry) > 0)
					{
						if (entry.d_name[0] != '.')
						{
							if (shown++ < 3)
								platform_log("gcov: %s/gcda holds %s (%d bytes)", DATA_ROOT, entry.d_name,
									(int)entry.d_stat.st_size);
							files++;
						}
						memset(&entry, 0, sizeof(entry));
					}
					sceIoDclose(folder);
				}
				platform_log("gcov: %d files in %s/gcda", files, DATA_ROOT);
			}
			{
				FILE *errors = fopen(DATA_ROOT "/gcda_errors.txt", "r");
				char line[200];
				int lines = 0;

				while (errors && fgets(line, sizeof(line), errors) && lines++ < 12)
					platform_log("gcov stderr: %s", line);
				if (errors)
					fclose(errors);
			}
		}
	}
#endif
	phase_tick();
	ladder_tick(ticks_seen); /* build 117 */
	{
		extern void psp_transwalk_tick(void); /* psp_fixups.py (FIXES_TRANSITIONS) */

		psp_transwalk_tick();
	}
	{
		/* "saveeveryN" (a checkpoint every N s of game time) and "revertatN"
		(the last one reverted to at game second N): the checkpoint writer's
		timing over many checkpoints on the PSP, and, with statecrc, whether a
		format restores the game state exactly (two runs that differ only in
		the format must play on identically after the revert) */
		static int every = -2, revert_at = -2;

		if (every == -2 && psp_keep_loaded())
		{
			every = psp_keep_number("saveevery", 0);
			revert_at = psp_keep_number("revertat", 0);
		}
		if ((every > 0 && ticks_seen % ((unsigned long)every * 30) == 0) ||
			(revert_at > 0 && ticks_seen == (unsigned long)revert_at * 30))
		{
			extern void main_save_map_nonsafe(void);
			extern void main_revert_map(void);
			int reverting = revert_at > 0 && ticks_seen == (unsigned long)revert_at * 30;

			psp_player_snapshot(player_text, sizeof(player_text));
			platform_log("saveevery: %s at tick %lu (%s)", reverting ? "revert" : "checkpoint", ticks_seen, player_text);
			if (reverting)
				main_revert_map();
			else
				main_save_map_nonsafe();
		}
	}
	if ((ticks_seen == 300 || ticks_seen == 450) && psp_keep("savetest"))
	{
		/* the main loop does both between ticks, as for the campaign's own
		checkpoints and deaths */
		extern void main_save_map_nonsafe(void);
		extern void main_revert_map(void);

		psp_player_snapshot(player_text, sizeof(player_text));
		platform_log("savetest: %s at tick %lu (%s)", ticks_seen == 300 ? "checkpoint" : "revert", ticks_seen,
			player_text);
		if (ticks_seen == 300)
			main_save_map_nonsafe();
		else
			main_revert_map();
	}
	if (++ticks_since_snapshot >= 30)
	{
		ticks_since_snapshot = 0;
		psp_player_snapshot(player_text, sizeof(player_text));
		{
			extern float psp_player_crouch(void);

			player_crouch_now = psp_player_crouch();
		}
	}
	tick_count++;
	tick_total += microseconds;
	if (microseconds > tick_maximum)
		tick_maximum = microseconds;
	PSP_METRICS_FRAMETRACE_TICK(microseconds); /* build 126 */
}

void psp_ge_frame_end(void);

void psp_phase(int phase);

/* build 87: game ticks between rendered frames, as a histogram (0, 1, 2, 3,
4+). With interpolation off (its default on the PSP), a frame shows the
state of the last tick: frames alternating 1 and 2 ticks move the world in
uneven steps, which reads as judder in fast turns (the warthog's donut
bounce, b80: a candidate cause). keep word "interpolation" smooths it */
static unsigned long ticks_per_frame_histogram[5];
/* the last frame's length, for the mixer's dry gaps (psp_dsound.c): a gap
follows a frame longer than the streams' queue */
unsigned long psp_last_frame_us = 0;

void psp_frame_account(unsigned long microseconds)
{
	extern void psp_crash_stop_if_crashed(void);
	unsigned long ticks = ticks_since_frame, end_start;

	/* build 102: another thread crashed: the report stays on the screen */
	psp_crash_stop_if_crashed();
	if (log_frame_flush)
		log_flush();
	ticks_per_frame_histogram[ticks > 4 ? 4 : ticks]++;
	ticks_since_frame = 0;
	/* the rendered frame goes to the screen (psp_ge.c) */
	psp_phase(3);
	end_start = psp_heartbeat_clock();
	psp_ge_frame_end();
	psp_phase(0);
	/* build 126: "frametrace" (psp_frametrace.c) */
	PSP_METRICS_FRAMETRACE_FRAME(microseconds, ticks, psp_heartbeat_clock() - end_start);
	{
		/* "lanetrace" (psp_lanetrace.c) */
		extern unsigned long psp_ge_frame_number(void);

		PSP_LANE_FRAME_END(psp_ge_frame_number(), (unsigned int)ticks);
	}
	psp_last_frame_us = microseconds;
	frame_count++;
	steps_frame_edge();
	frame_total += microseconds;
	if (microseconds > frame_maximum)
		frame_maximum = microseconds;
}

/* build 37: the hardware profiler (keep.txt "hwprofile"; psp/profile/main.c,
a kernel module, adds the CPU's counters into this table ten times a second) */
#define HW_COUNTERS 19
#define HW_PARTITIONS 8
#define HW_PHASES 4
static struct
{
	volatile unsigned int sequence, pad;
	volatile unsigned long long count[HW_COUNTERS];
	/* build 44: the memory partitions' free space, surveyed once by the
	module (psp/profile/main.c) */
	volatile int partition_total[HW_PARTITIONS];
	volatile int partition_largest[HW_PARTITIONS];
	volatile unsigned int partitions_surveyed;
	/* build 46: the counters by phase (0 other, 1 tick, 2 drawing, 3 frame
	end), and coprocessor 0's register 24 (bit 0: the 04g+ "L2" cache) */
	volatile unsigned long long phase_count[HW_PHASES][HW_COUNTERS];
	volatile unsigned int phase_changes;
	volatile unsigned int cop0_24;
	/* the split by thread ("hwthreads"; psp/profile/main.c): per phase,
	microseconds of the clock, of the other threads and of the idle thread,
	and the thread switches */
	volatile unsigned int threads_wanted;
	volatile unsigned int threads_tracked;
	volatile unsigned long long phase_threads[HW_PHASES][4];
} hwprofile __attribute__((aligned(64)));
static unsigned long long hwprofile_threads_last[HW_PHASES][4];
static unsigned long long hwprofile_phase_last[HW_PHASES][HW_COUNTERS];
static int partitions_reported = 0;
static unsigned long long hwprofile_last[HW_COUNTERS];
static int hwprofile_running = 0;

/* build 46: tell the profiler what the game is doing now (the system call
halo_profile.prx exports; psp/profile/exports.exp). Only with the module
running: before it loads, the import isn't linked */
int halo_profile_phase(int phase);

/* build 108: the phase as a plain word too (0 other, 1 tick, 2 drawing, 3
frame end), for the emulator's cache models to split their counts by
(PSP_DCACHE_PHASE; tools/ppsspp_local_patches.diff) */
volatile int psp_current_phase = 0;

void psp_phase(int phase)
{
	psp_current_phase = phase;
	/* "lanetrace" (psp_lanetrace.c) */
	PSP_LANE(PSP_LANE_PHASE, 0, sceKernelGetSystemTimeLow(), 0, (unsigned int)phase);
	if (hwprofile_running)
		halo_profile_phase(phase);
#ifdef PSP_RECON
	psp_me_bus_phase(phase); /* "mebusN" (psp_me.c) */
#endif
}

static void hwprofile_start(const char *eboot_path)
{
	char path[256];
	char *slash;
	SceUID module;
	int status = 0;
	unsigned int address = (unsigned int)&hwprofile;

	snprintf(path, sizeof(path), "%s", eboot_path ? eboot_path : "ms0:/PSP/GAME/halo/EBOOT.PBP");
	slash = strrchr(path, '/');
	snprintf(slash ? slash + 1 : path, sizeof(path) - (slash ? (size_t)(slash + 1 - path) : 0), "halo_profile.prx");
	hwprofile.threads_wanted = psp_keep("hwthreads") != 0;
	module = sceKernelLoadModule(path, 0, NULL);
	if (module >= 0)
		module = sceKernelStartModule(module, sizeof(address), &address, &status, NULL);
	hwprofile_running = module >= 0;
	platform_log("hardware profiler %s: %s (0x%08x)", path, module >= 0 ? "started" : "not loaded", (unsigned)module);
}

/* build 105: the bus-priority probe (halo_profile.prx; psp_hardware_reference.md
section 9.1): the undocumented DDR, GE, EDRAM and system-controller
registers, read once with the GE idle (start-up) and once busy (in play).
Read-only, and only the module's own fixed list. The emulator has no such
registers, so it is skipped there */
int halo_profile_bus_probe(unsigned int *pairs, int max_pairs);

static void bus_probe(const char *when)
{
	extern int psp_is_emulator(void);
	static unsigned int pairs[2 * 32] __attribute__((aligned(64)));
	char line[600];
	int count, length = 0, k;

	if (!hwprofile_running)
		return;
	if (psp_is_emulator())
	{
		platform_log("bus probe (%s): skipped in the emulator", when);
		return;
	}
	count = halo_profile_bus_probe(pairs, 32);
	for (k = 0; k < count && length < (int)sizeof(line) - 24; k++)
		length += snprintf(line + length, sizeof(line) - length, " %08x=%08x", pairs[2 * k], pairs[2 * k + 1]);
	platform_log("bus probe (%s): %d registers:%s", when, count, count > 0 ? line : " (the module refused)");
}

/* the counters since the last heartbeat, as shares of the CPU's cycles and
counts a millisecond (counters in PspDebugProfilerRegs order, from systemck) */
static void hwprofile_report(void)
{
	unsigned long long d[HW_COUNTERS];
	double cycles, ms;
	int k;

	if (!hwprofile_running || !hwprofile.sequence)
		return;
	if (hwprofile.partitions_surveyed && !partitions_reported)
	{
		char line[400];
		int length = 0;

		partitions_reported = 1;
		for (k = 0; k < HW_PARTITIONS && length < (int)sizeof(line) - 48; k++)
			if (hwprofile.partition_total[k] > 0 || hwprofile.partition_largest[k] > 0)
				length += snprintf(line + length, sizeof(line) - length, " %d: %d KB free (largest block %d KB);", k + 1,
					hwprofile.partition_total[k] / 1024, hwprofile.partition_largest[k] / 1024);
		platform_log("memory partitions (1 kernel, 2 the game, 5 the borrowed 4 MB):%s", length ? line : " none reported");
		platform_log("coprocessor 0 register 24: 0x%08x (bit 0, the firmware's test for the 04g+ boards' L2 cache: %s)",
			hwprofile.cop0_24, hwprofile.cop0_24 & 1 ? "set" : "clear");
	}
	for (k = 0; k < HW_COUNTERS; k++)
	{
		unsigned long long now = hwprofile.count[k];

		d[k] = now - hwprofile_last[k];
		hwprofile_last[k] = now;
	}
	cycles = d[1] ? (double)d[1] : 1.0;
	/* counts per million CPU cycles (3 ms at 333 MHz) */
	ms = cycles / 1e6;
	platform_log("hardware: %.0f M CPU cycles (%.0f M system); waiting on memory %.1f%%, internal %.1f%%, VFPU %.1f%%, coprocessors %.1f%%, asleep %.1f%% | per million cycles: D-cache misses %.0f, writebacks %.0f, I-cache misses %.0f, uncached loads %.0f stores %.0f, cached loads %.0f stores %.0f, bus %.0f, FPU ops %.0f, VFPU ops %.0f, local bus %.0f",
		d[1] / 1e6, d[0] / 1e6, 100.0 * d[3] / cycles, 100.0 * d[2] / cycles, 100.0 * d[5] / cycles,
		100.0 * d[4] / cycles, 100.0 * d[6] / cycles,
		d[13] / ms, d[14] / ms, d[12] / ms, d[8] / ms, d[9] / ms, d[10] / ms, d[11] / ms, d[7] / ms,
		d[16] / ms, d[17] / ms, d[18] / ms);
	/* (build 113: the 19th counter, the profiler's "local bus", was read but
	never printed; PSPTEK reads it as the GE's bus cycles [U]: a cross-check
	for gebusy) */
	{
		/* build 46: the same by phase */
		static const char *const names[HW_PHASES] = { "other", "tick", "drawing", "frame end" };
		char line[700];
		int length = 0, phase;

		for (phase = 0; phase < HW_PHASES && length < (int)sizeof(line) - 160; phase++)
		{
			unsigned long long p[HW_COUNTERS];
			double pc;

			for (k = 0; k < HW_COUNTERS; k++)
			{
				unsigned long long now = hwprofile.phase_count[phase][k];

				p[k] = now - hwprofile_phase_last[phase][k];
				hwprofile_phase_last[phase][k] = now;
			}
			pc = p[1] ? (double)p[1] : 1.0;
			if (phase == 2)
			{
				/* build 93: for benchrun's RESULT line */
				hw_drawing[0] = (float)(100.0 * p[3] / pc);
				hw_drawing[1] = (float)(p[13] / (pc / 1e6));
				hw_drawing[2] = p[13] ? (float)((double)p[3] / (double)p[13]) : 0.0f;
				hw_drawing[3] = (float)(p[9] / (pc / 1e6));
				hw_drawing_valid = p[1] > 0;
			}
			length += snprintf(line + length, sizeof(line) - length,
				" %s %.1f%% (memory %.1f%%, asleep %.1f%%, I %.0f, D %.0f, wb %.0f, uncached loads %.0f stores %.0f);",
				names[phase], 100.0 * p[1] / cycles, 100.0 * p[3] / pc, 100.0 * p[6] / pc,
				p[12] / (pc / 1e6), p[13] / (pc / 1e6), p[14] / (pc / 1e6), p[8] / (pc / 1e6), p[9] / (pc / 1e6));
			/* two phases a line: a log line holds 512 characters (b46's were cut) */
			if (phase == 1)
			{
				platform_log("hardware by phase (share of cycles; per million cycles: I/D-cache misses, writebacks, uncached; %u phase changes):%s",
					hwprofile.phase_changes, line);
				length = 0;
				line[0] = 0;
			}
		}
		platform_log("hardware by phase, continued:%s", line);
	}
	{
		/* Each phase's share of the clock: the system counter (0) runs at
		333 MHz whatever the CPU does, while the CPU's (1), the shares
		above, stops while the idle thread has the CPU halted. A window that
		waits (a frame-rate cap, a load) shows its active phases larger by
		the cycles' count than by the clock: the "missing third" of the
		measurement round */
		static const char *const names[HW_PHASES] = { "other", "tick", "drawing", "frame end" };
		static unsigned long long system_last[HW_PHASES], active_last[HW_PHASES];
		unsigned long long system[HW_PHASES], active[HW_PHASES], system_all = 0;
		char line[400];
		int length = 0, phase;

		for (phase = 0; phase < HW_PHASES; phase++)
		{
			system[phase] = hwprofile.phase_count[phase][0] - system_last[phase];
			active[phase] = hwprofile.phase_count[phase][1] - active_last[phase];
			system_last[phase] = hwprofile.phase_count[phase][0];
			active_last[phase] = hwprofile.phase_count[phase][1];
			system_all += system[phase];
		}
		for (phase = 0; phase < HW_PHASES && length < (int)sizeof(line) - 64; phase++)
			length += snprintf(line + length, sizeof(line) - length, " %s %.1f%% (halted %.1f%%);", names[phase],
				system_all ? 100.0 * system[phase] / system_all : 0.0,
				system[phase] && system[phase] > active[phase] ? 100.0 * (system[phase] - active[phase]) / system[phase] : 0.0);
		platform_log("hardware by phase, of the clock (share of the system cycles; in brackets, the share of the phase the CPU was halted):%s", line);
	}
	if (hwprofile.threads_wanted)
	{
		/* "hwthreads": who ran in each phase, by the threads' run clocks:
		the game's thread is the phase's clock less the other threads and
		the idle thread */
		static const char *const names[HW_PHASES] = { "other", "tick", "drawing", "frame end" };
		unsigned long long d[HW_PHASES][4], clock_all = 0;
		char line[500];
		int length = 0, phase, k;

		for (phase = 0; phase < HW_PHASES; phase++)
			for (k = 0; k < 4; k++)
			{
				d[phase][k] = hwprofile.phase_threads[phase][k] - hwprofile_threads_last[phase][k];
				hwprofile_threads_last[phase][k] = hwprofile.phase_threads[phase][k];
			}
		for (phase = 0; phase < HW_PHASES; phase++)
			clock_all += d[phase][0];
		for (phase = 0; phase < HW_PHASES && length < (int)sizeof(line) - 120; phase++)
		{
			double clock = d[phase][0] ? (double)d[phase][0] : 1.0;
			unsigned long long busy = d[phase][1] + d[phase][2];
			double game = d[phase][0] > busy ? (double)(d[phase][0] - busy) : 0.0;

			length += snprintf(line + length, sizeof(line) - length, " %s %.1f%% (game %.1f%%, others %.1f%%, idle %.1f%%; %lu switches);",
				names[phase], clock_all ? 100.0 * d[phase][0] / clock_all : 0.0,
				100.0 * game / clock, 100.0 * d[phase][1] / clock, 100.0 * d[phase][2] / clock, (unsigned long)d[phase][3]);
		}
		platform_log("hardware by thread (each phase's share of the clock; in brackets, who had the CPU in it; %u threads):%s", hwprofile.threads_tracked, line);
	}
}

/* the heartbeat's work between its sleeps, for frametrace's late frames:
busy while a beat reports, spans counting the beats begun, work_us their
time (by the clock, the stick's waits included) */
volatile int psp_heartbeat_busy = 0;
volatile unsigned long psp_heartbeat_spans = 0, psp_heartbeat_work_us = 0;
static unsigned long heartbeat_work_start = 0;

static int heartbeat_thread(SceSize args, void *argp)
{
	long last_ticks = -1;
	SceInt64 last_time = sceKernelGetSystemTimeWide();
	int beat = 0;

	(void)args; (void)argp;
	psp_fpu_mask_traps();
	/* "hblow": the heartbeat below the game thread (0x20), so its reports
	and their formatting run only while the game thread waits (the vertical
	blank, the GE). Its default priority (0x18) is above the game's, so a
	beat's work can hold the game thread past a vertical blank. Opt-in: below
	the game thread the heartbeat, which also writes the log, can't keep up
	on a busy map (lines dropped), and it is the lead suspect for a freeze */
	if (psp_keep("hblow"))
		sceKernelChangeThreadPriority(0, 0x28);
	if (!psp_keep("logflushgame"))
	{
		log_wake_thread = sceKernelGetThreadId();
		log_wake = sceKernelCreateSema("halo log wake", 0, 0, 1, NULL);
	}
	for (;;)
	{
		SceInt64 now;
		long ticks;
		unsigned long tc, tt, tm, fc, ft, fm;
		int window_done = 0;
		double seconds;

		/* the beat's work ends here: frametrace asks whether a late frame
		overlapped it */
		if (psp_heartbeat_busy)
		{
			psp_heartbeat_work_us += psp_heartbeat_clock() - heartbeat_work_start;
			psp_heartbeat_busy = 0;
		}
		/* every second for the first 20, then every 5 */
		/* build 98: with "blackbox", the log saved every quarter second
		meanwhile (psp_trace.c) */
		{
			static int blackbox = -1;
			int slice, slices;

			if (blackbox < 0)
				blackbox = psp_keep("blackbox");
			slices = blackbox ? (beat < 20 ? 4 : 20) : 1;
			for (slice = 0; slice < slices; slice++)
			{
				SceInt64 until = sceKernelGetSystemTimeWide() + (beat < 20 ? 1000000 : 5000000) / slices;

				/* asleep for the slice, but up at once to write a filling batch */
				for (;;)
				{
					SceInt64 left = until - sceKernelGetSystemTimeWide();
					SceUInt timeout;

					if (left <= 0)
						break;
					timeout = (SceUInt)left;
					if (log_wake < 0)
					{
						sceKernelDelayThread(timeout);
						break;
					}
					if (sceKernelWaitSema(log_wake, 1, &timeout) < 0)
						break;
					log_flush();
					if (psp_phase_cut)
						break;
				}
				if (psp_phase_cut)
					break;
				if (blackbox && slice + 1 < slices)
					log_flush();
			}
		}
		beat++;
		phase_cut_beat = psp_phase_cut;
		psp_phase_cut = 0;
		heartbeat_work_start = psp_heartbeat_clock();
		psp_heartbeat_spans++;
		psp_heartbeat_busy = 1;
		heartbeat_beats = beat;
		now = sceKernelGetSystemTimeWide();
		seconds = (now - last_time) / 1e6;
		ticks = psp_game_time_peek();
		tc = tick_count; tt = tick_total; tm = tick_maximum;
		fc = frame_count; ft = frame_total; fm = frame_maximum;
		tick_count = tick_total = tick_maximum = 0;
		frame_count = frame_total = frame_maximum = 0;
		/* the profile, every other 5 s beat once the game runs; the first
		window starts at the first such beat */
		if (ticks >= 0 && last_ticks >= 0 && beat > 20)
		{
			static SceInt64 profile_window_start = 0;
			static unsigned long profile_frames = 0, profile_ticks = 0;

			if (!profile_window_start)
			{
				profile_window_start = now;
				profile_forget();
				memset(object_microseconds, 0, sizeof(object_microseconds));
				memset(object_updates, 0, sizeof(object_updates));
				{
					extern long psp_objects_asleep, psp_objects_sleep_checked, psp_vehicles_asleep, psp_items_asleep;
					extern long psp_scenery_unchecked, psp_scenery_woken;
					extern long psp_items_lit_updated, psp_items_lit_offered, psp_items_lit_asleep, psp_items_lit_reason[6];
					extern long psp_weapons_ground, psp_weapons_asleep;

					psp_objects_asleep = psp_objects_sleep_checked = psp_vehicles_asleep = psp_items_asleep = 0;
					psp_items_lit_updated = psp_items_lit_offered = psp_items_lit_asleep = 0;
					memset(psp_items_lit_reason, 0, sizeof(psp_items_lit_reason));
					psp_weapons_ground = psp_weapons_asleep = 0;
					psp_scenery_unchecked = psp_scenery_woken = 0;
				}
			}
			else
			{
				profile_frames += fc;
				profile_ticks += tc;
				if ((beat & 1) == 0 || phase_cut_beat)
				{
					profile_report((now - profile_window_start) / 1e6, profile_frames, profile_ticks);
					profile_window_start = now;
					profile_frames = profile_ticks = 0;
				}
			}
		}
		if (ticks >= 0 && last_ticks >= 0)
			platform_log("heartbeat: game time %ld (%.1f ticks/s) | tick %.2f ms avg, %.2f max | frame %.2f ms avg, %.2f max, %.1f fps | cpu %d MHz | free %d KB | heap top %lu of %d KB",
				ticks, (ticks - last_ticks) / seconds,
				tc ? tt / 1000.0 / tc : 0.0, tm / 1000.0,
				fc ? ft / 1000.0 / fc : 0.0, fm / 1000.0, fc / seconds,
				scePowerGetCpuClockFrequencyInt(), sceKernelTotalFreeMemSize() / 1024,
				heap_base ? (unsigned long)((char *)sbrk(0) - heap_base) / 1024 : 0, PSP_HEAP_KB);
#ifdef PSP_MULTIPLAYER
		/* mp3: the network stack's pool and the access point (psp_net.c) */
		if (ticks >= 0 && last_ticks >= 0)
		{
			extern void psp_net_heartbeat(void);

			psp_net_heartbeat();
		}
#endif
		/* build 73: the Memory Stick reads of this window: how many, how
		long in all, the slowest one (b71/b72: 1.7-5 s a frame at the
		frame's end, the reads the suspects) */
		if (ticks >= 0 && last_ticks >= 0)
		{
			extern void psp_tex_io_window(unsigned long *reads, unsigned long *us, unsigned long *worst_us);
			extern void psp_geo_io_window(unsigned long *reads, unsigned long *us, unsigned long *worst_us);
			extern void psp_anim_io_window(unsigned long *reads, unsigned long *us, unsigned long *worst_us);
			unsigned long tr, tu, tw, gr, gu, gw, ar, au, aw;

			psp_tex_io_window(&tr, &tu, &tw);
			psp_geo_io_window(&gr, &gu, &gw);
			psp_anim_io_window(&ar, &au, &aw);
			platform_log("io: textures %lu reads, %.1f ms (slowest %.1f); geometry %lu reads, %.1f ms (slowest %.1f); animations %lu reads, %.1f ms (slowest %.1f) over %.1f s",
				tr, tu / 1000.0, tw / 1000.0, gr, gu / 1000.0, gw / 1000.0, ar, au / 1000.0, aw / 1000.0, seconds);
			{
				extern void psp_ge_frame_window(char *text, int size);
				char line[512]; /* (build 113: gebusy's part too; the log's lines stop at 512) */

				psp_ge_frame_window(line, sizeof(line));
				platform_log("%s", line);
			}
		}
		/* build 64: the frame against the 33.3 ms of 30 frames a second */
		if (ticks >= 0 && last_ticks >= 0 && fc)
		{
			extern void psp_ge_budget(unsigned long *renderer_us, unsigned long *wait_us);
			unsigned long renderer_us = 0, wait_us = 0;
			double wall = seconds * 1000.0 / fc, ticks_ms = tt / 1000.0 / fc, frame_ms = ft / 1000.0 / fc;

			psp_ge_budget(&renderer_us, &wait_us);
			platform_log("budget: tag lookups by name %lu a frame (tag_loaded; verify_tag_reference answers from the reference since build 66)",
				fc ? psp_tag_loaded_calls / fc : 0);
			psp_tag_loaded_calls = 0;
			{
				/* build 109: model_find_marker's table (psp_fixups.py, FIXES_109) */
				extern unsigned long psp_marker_memo_hits, psp_marker_memo_searches;

				platform_log("budget: marker lookups %lu a frame, %lu of them searched",
					(psp_marker_memo_hits + psp_marker_memo_searches) / fc, psp_marker_memo_searches / fc);
				psp_marker_memo_hits = psp_marker_memo_searches = 0;
			}
			psp_budget_last[0] = (float)wall;
			psp_budget_last[1] = (float)(fc / seconds);
			psp_budget_last[2] = (float)ticks_ms;
			psp_budget_last[3] = (float)frame_ms;
			psp_budget_last[4] = renderer_us / 1000.0f;
			psp_budget_last[5] = wait_us / 1000.0f;
			psp_budget_last[6] = (float)((ticks - last_ticks) / seconds);
			/* build 87: benchrun steps on window boundaries (counted at the
			end of this heartbeat, once the ME's report is in too) */
			window_done = 1;
			platform_log("budget: %.1f ms a frame by the clock (%.1f fps; 30 fps is 33.3): ticks %.1f ms a frame (%.2f ticks), frame %.1f (the GE renderer's CPU %.1f of it, waiting for the GE %.1f), the rest %.1f; %s by %.1f ms",
				wall, fc / seconds, ticks_ms, (double)tc / fc, frame_ms, renderer_us / 1000.0, wait_us / 1000.0,
				wall - ticks_ms - frame_ms, wall > 33.33 ? "over" : "under", wall > 33.33 ? wall - 33.33 : 33.33 - wall);
		}
		if (ticks >= 0 && last_ticks >= 0 && fc)
		{
			platform_log("frames by game ticks since the last frame: 0: %lu, 1: %lu, 2: %lu, 3: %lu, 4+: %lu (interpolation %s)",
				ticks_per_frame_histogram[0], ticks_per_frame_histogram[1], ticks_per_frame_histogram[2],
				ticks_per_frame_histogram[3], ticks_per_frame_histogram[4], psp_keep("interpolation") ? "on" : "off");
			memset(ticks_per_frame_histogram, 0, sizeof(ticks_per_frame_histogram));
		}
		/* build 96: the load trace ends after two seconds of play */
		if (ticks >= 60)
		{
			extern void psp_trace_stop(const char *why);

			psp_trace_stop("two seconds of play");
		}
		if (!(ticks >= 0 && last_ticks >= 0))
			platform_log("heartbeat %d: game time %ld; step %s; free %d KB; log fd %d", beat, ticks, last_step,
				sceKernelTotalFreeMemSize() / 1024, log_descriptor);
		hwprofile_report();
		psp_lock_report(); /* build 104 */
		{
			/* build 105: the reserve descriptor, given up when the kernel ran
			out, is taken back as soon as one is free again (b104 run 1 lost
			two log batches after the first refusal used it up) */
			extern void psp_handles_reserve(void);

			psp_handles_reserve();
		}
		/* build 103: the game counts as activity, so the firmware doesn't dim
		the backlight (b102: the screen went dark after a while without
		buttons; the analog stick alone doesn't reset the timer) or put the PSP
		to sleep during play, a cutscene or a bench left alone */
		if (ticks >= 0)
			scePowerTick(PSP_POWER_TICK_ALL);
		{
			/* build 100: the Memory Stick lock's waits (psp_trace.c) */
			extern void psp_stick_report(void);
			extern void psp_threads_report(void); /* build 106 */

			if (ticks >= 0 && beat > 20 && (beat % 6) == 0)
			{
				/* the collision log by user (collision_usage.c; recon builds
				with PSP_DEBUG_TOOLS only, a no-op otherwise) */
				extern void psp_collision_report(void);

				psp_stick_report();
				psp_threads_report();
				psp_collision_report();
				if (psp_fraction_word == 2)
					platform_log("fractioncheck: %lu fractions compared, %lu differed", fraction_checked, fraction_differed);
			}
		}
		if (ticks >= 0 && (beat % 6) == 3)
		{
			/* build 44: the game state's arrays and pools, with their peaks */
			extern void psp_game_state_report(void);

			psp_game_state_report();
		}
		/* once the game runs, its lines wait in the batch for the beat's
		write: a line written at once costs the stick an open, a write and a
		close, and a map's first seconds log the most while it streams the
		most (b30 in the emulator: the log held the stick 7.1 s up to tick
		400, and geometry waited 0.58 s behind it). A crash still writes the
		batch (psp_crash.c); a hang loses at most the beat's lines */
		if (ticks >= 0 && (beat > 20 || !log_each_line()))
			log_batching = 1;
		if (ticks >= 0)
		{
			extern volatile unsigned int psp_pad_buttons, psp_pad_reads;
			extern volatile int psp_pad_lx, psp_pad_ly;

			extern volatile int psp_nub_seen[4];

			/* the nub's furthest each way since launch: what a nub.txt
			calibration is chosen from (psp_sdl.c) */
			platform_log("%s | pad %04x stick %d,%d (%u reads; the nub has reached x %d-%d, y %d-%d)", player_text,
				psp_pad_buttons & 0xFFFF, psp_pad_lx, psp_pad_ly, psp_pad_reads, psp_nub_seen[0], psp_nub_seen[1],
				psp_nub_seen[2], psp_nub_seen[3]);
			{
				extern volatile unsigned int controls_events[5];

				if (controls_events[0] | controls_events[1] | controls_events[2])
					platform_log("controls (since launch): crouch toggled %u times; d-pad left tapped %u times (the game reloaded %u), held %u times (the game acted %u); crouched %d%% now",
						controls_events[0], controls_events[1], controls_events[3], controls_events[2], controls_events[4],
						(int)(player_crouch_copy() * 100.0f));
			}
			if ((beat & 1) == 0)
			{
				/* the sound mixer's cost (psp_dsound.c; build 25) */
				extern void psp_audio_report(void);

				psp_audio_report();
			}
			{
				/* build 87: the Media Engine's jobs (psp_me.c), every window */
				extern void psp_me_report(void);
				extern void psp_me_edram_probe(void);

				psp_me_report();
				psp_me_edram_probe(); /* build 115: edramcheck */
			}
			{
				/* build 88: the learned textures, saved here during play (psp_tex.c) */
				extern void psp_tex_save_pending(void);

				psp_tex_save_pending();
			}
		}
		last_ticks = ticks;
		last_time = now;
		if (window_done)
			psp_heartbeat_windows++;
		if (log_holds != log_holds_reported)
		{
			/* build 116 (R11) */
			platform_log("log: writes held %lu times around checkpoints (the sound reader first)",
				log_holds - log_holds_reported);
			log_holds_reported = log_holds;
		}
		log_flush();
	}
	return 0;
}

/* loads halo_crash.prx from the EBOOT's folder: a kernel-mode module that
writes ms0:/halo/crash.txt when the game crashes (psp/crash/main.c) */
static void load_crash_handler(const char *eboot_path)
{
	char path[256];
	char *slash;
	SceUID module;
	int status = 0;

	snprintf(path, sizeof(path), "%s", eboot_path ? eboot_path : "ms0:/PSP/GAME/halo/EBOOT.PBP");
	slash = strrchr(path, '/');
	snprintf(slash ? slash + 1 : path, sizeof(path) - (slash ? (size_t)(slash + 1 - path) : 0), "halo_crash.prx");
	module = sceKernelLoadModule(path, 0, NULL);
	if (module >= 0)
	{
		/* build 102: the module returns from a crash into the game's own
		handler (psp_crash.c); these say where it is */
		extern int psp_crash_arguments(void *out, int size);
		unsigned int arguments[4];

		if (psp_crash_arguments(arguments, sizeof(arguments)))
			module = sceKernelStartModule(module, sizeof(arguments), arguments, &status, NULL);
		else
			module = sceKernelStartModule(module, 0, NULL, &status, NULL);
	}
	platform_log("crash handler %s: %s (0x%08x)", path, module >= 0 ? "installed" : "not loaded", (unsigned)module);
}

/* The last launches' files kept, numbered, in the log folder: log.txt,
profile.txt and crash.txt become log/NNN_log.txt and so on (NNN counts
launches), and only the last OLD_LAUNCHES sets stay, so a test whose log
wasn't pulled before the next launch isn't lost, and profile.txt doesn't
gather every launch's windows. Renamed within the one folder: whether the
Memory Stick's driver moves a file to another folder isn't known, so
crash.txt (in the data folder) is tried and left where it was if the rename
fails. No keep word: keep.txt is read after this, and the cost is a few
renames at start-up */
#define OLD_LAUNCHES 20
static void keep_last_launch(void)
{
	static const char *const names[3] = { LOG_FOLDER "/log.txt", LOG_FOLDER "/profile.txt", DATA_ROOT "/crash.txt" };
	static const char *const suffixes[3] = { "log.txt", "profile.txt", "crash.txt" };
	SceIoStat status;
	SceIoDirent entry;
	SceUID folder;
	int newest = 0, oldest = 0x7FFFFFFF, sets = 0, number, k, n;
	char path[96];

	if (sceIoGetstat(LOG_PATH, &status) < 0)
		return;
	folder = sceIoDopen(LOG_FOLDER);
	if (folder >= 0)
	{
		memset(&entry, 0, sizeof(entry));
		while (sceIoDread(folder, &entry) > 0)
		{
			if (entry.d_name[0] >= '0' && entry.d_name[0] <= '9' && strcmp(entry.d_name + 3, "_log.txt") == 0)
			{
				number = atoi(entry.d_name);
				sets++;
				if (number > newest)
					newest = number;
				if (number < oldest)
					oldest = number;
			}
			memset(&entry, 0, sizeof(entry));
		}
		sceIoDclose(folder);
	}
	number = newest >= 999 ? 1 : newest + 1;
	for (k = 0; k < 3; k++)
	{
		snprintf(path, sizeof(path), LOG_FOLDER "/%03d_%s", number, suffixes[k]);
		sceIoRemove(path);
		if (k < 2)
			sceIoRename(names[k], path);
		else if (sceIoGetstat(names[k], &status) >= 0)
			sceIoRename(names[k], path); /* on failure, left in ms0:/halo as before */
	}
	/* the oldest sets past the last OLD_LAUNCHES */
	for (n = oldest; sets + 1 > OLD_LAUNCHES && n < number; n++)
	{
		snprintf(path, sizeof(path), LOG_FOLDER "/%03d_log.txt", n);
		if (sceIoRemove(path) < 0)
			continue;
		for (k = 1; k < 3; k++)
		{
			snprintf(path, sizeof(path), LOG_FOLDER "/%03d_%s", n, suffixes[k]);
			sceIoRemove(path);
		}
		sets--;
	}
}

int main(int argc, char **argv)
{
	SceUID thread;
	SceIoStat status;

	psp_fpu_mask_traps();
	heap_base = (char *)sbrk(0);
	log_lock = sceKernelCreateSema("halo log", 0, 1, 1, NULL);
	flush_lock = sceKernelCreateSema("halo log flush", 0, 1, 1, NULL);
	/* build 88: 16 KB (was 4 KB): the exit callback formats log lines (a 512-byte
	line plus vsnprintf's own frame) and writes files on this stack */
	thread = sceKernelCreateThread("halo callbacks", callback_thread, 0x11, 0x4000, 0, NULL);
	if (thread >= 0)
		sceKernelStartThread(thread, 0, NULL);

	/* everything the platform layer writes to stdout and stderr goes to
	the log, unbuffered, so it survives a crash */
	sceIoMkdir(DATA_ROOT, 0777);
	sceIoMkdir(DATA_ROOT "/save", 0777);
	sceIoMkdir(LOG_FOLDER, 0777);
	keep_last_launch(); /* build 114 */
	sceIoRemove(LOG_PATH);
	/* build 105: no exit.txt any more (see the exit callback) */
	sceIoRemove(LOG_FOLDER "/exit.txt");
	{
		/* build 103: one file handle held back for when the kernel runs out
		(psp_trace.c's handle table). b104 run 1: it runs out at about ten of
		the game's own, not the 64 uofw's table suggested */
		extern void psp_handles_reserve(void);

		psp_handles_reserve();
	}
	{
		/* build 105: newlib's number formatting, one thread at a time (psp_libc.c) */
		extern void psp_format_lock_init(void);

		psp_format_lock_init();
	}
	freopen(DATA_ROOT "/stderr.txt", "w", stderr);
	setvbuf(stderr, NULL, _IONBF, 0);
	/* build 105: stdout shares stderr's stream (and so its one descriptor);
	builds up to 104 opened the same file a second time for it */
	stdout = stderr;

	/* homebrew starts at 222 MHz; the measurements assume the PSP-1000's
333 MHz (bus 166) */
	scePowerSetClockFrequency(333, 333, 166);

	load_crash_handler(argc > 0 ? argv[0] : NULL);
	{
		/* halo_sleep.prx is beside the EBOOT (psp_sleepguard.c; loaded once
		keep.txt is read) */
		extern void psp_sleepguard_eboot(const char *path);

		psp_sleepguard_eboot(argc > 0 ? argv[0] : NULL);
	}

	setenv("HALO_DATA_ROOT", DATA_ROOT, 1);
	setenv("HALO_SAVE_ROOT", DATA_ROOT "/save", 1);
	setenv("HALO_NULL_RENDERER", "1", 1);
	/* the game's frustum in the screen's own shape (480:272, as 848 x 480),
	so it culls what the GE's projection shows (build 19) */
	setenv("HALO_DISPLAY_WIDTH", "848", 1);
	/* sound: config.toml's [audio] enabled (on unless it says false; build 25) */
	/* build 53: the build's number first, so a log says which EBOOT ran */
	platform_log("Halo PSP bring-up build %d (%s): free user memory %d KB, largest block %d KB", PSP_BUILD_NUMBER,
		PSP_ENGINE_BASE, sceKernelTotalFreeMemSize() / 1024, sceKernelMaxFreeMemSize() / 1024);
#ifdef PSP_PGO_PROFILE
	/* PSP_PGO=use (psp_build.py): the profile it was built with */
	platform_log("pgo: the hot code laid out and optimised by the profile pgo/%s.tgz", PSP_PGO_PROFILE);
#endif
	/* build 89: whether HOME and sleep are armed (negative: an error) */
	platform_log("callbacks: power registration %08x, exit callback %08x, exit registration %08x",
		(unsigned int)exit_callbacks_registered[0], (unsigned int)exit_callbacks_registered[1],
		(unsigned int)exit_callbacks_registered[2]);
	/* build 66: the launch menu (psp_menu.c): the map and the keep words,
	written to init.txt and keep.txt before they are read */
	{
		extern void psp_menu(int build);

		psp_menu(PSP_BUILD_NUMBER);
	}
	keep_load();
	{
		/* "heapmap" and "poolpeaks": the heap's map and the game state's
		peaks at each level change (psp_libc.c; psp_fixups.py FIXES_RECON
		#poolpeaks, linked weakly) */
		extern void psp_heap_map_start(void);
		extern void psp_pool_peaks_start(void) __attribute__((weak));

		psp_heap_map_start();
		if (psp_pool_peaks_start)
			psp_pool_peaks_start();
		else if (psp_keep("poolpeaks"))
			platform_log("poolpeaks: not in this build (PSP_RECON=1 builds it)");
	}
	{
		/* before anything reads the maps: a bake of another format is
		explained on the screen rather than drawn wrong (psp_bakecheck.c) */
		extern void psp_bake_check(void);

		psp_bake_check();
	}
	{
		/* build 37: the VFPU's matrix product, checked against the FPU */
		extern void psp_vfpu_initialize(void);

		psp_vfpu_initialize();
	}
	/* build 93: benchrun starts the profiler too (its counters go on each
	RESULT line) */
	if (psp_keep("hwprofile") || psp_keep("benchrun"))
	{
		hwprofile_start(argc > 0 ? argv[0] : NULL);
		bus_probe("at start-up, the GE idle");
	}
	{
		/* the loading screen from here to the main menu, in place of the log
		on the screen (psp_loading.c; the probe above wants the GE idle) */
		extern void psp_loading_boot_begin(void);

		psp_loading_boot_begin();
	}

	thread = sceKernelCreateThread("halo heartbeat", heartbeat_thread, 0x18, 0x4000, 0, NULL);
	if (thread >= 0)
		sceKernelStartThread(thread, 0, NULL);
	/* from here the start-up's lines wait in the batch for the heartbeat's
	write each second, as the game's do once it runs: each line written at
	once cost the stick an open, a write and a close, and the heartbeat's
	(at a priority above the game's and the saved-game check's) held the
	stick 3 s at the first frame on the PSP-1000.
	A crash still writes the batch (psp_crash.c); a hang loses at most a
	second's lines. "logeachline": each line at once, as before */
	if (thread >= 0 && !log_each_line())
		log_batching = 1;

	/* the crash handler's own test: with ms0:/halo/crashtest.txt present,
	the game faults on purpose here, and the handler's screen should appear */
	if (sceIoGetstat(DATA_ROOT "/crashtest.txt", &status) >= 0)
	{
		platform_log("crash test: storing to address 4 (delete crashtest.txt to run the game)");
		sceKernelDelayThread(1000000);
		*(volatile int *)4 = 1;
	}
	SDL_SetMainReady();
	return halo_main();
}

/* ---------- no network: the campaign build (the multiplayer build's
sockets are psp_net.c's) */

#ifndef PSP_MULTIPLAYER
int posix_socket_last_error(void) { return 10050; /* WSAENETDOWN */ }
int posix_socket(int family, int type, int protocol) { (void)family; (void)type; (void)protocol; return -1; }
int posix_socket_close(int socket) { (void)socket; return -1; }
int posix_socket_bind(int socket, const void *address, int length) { (void)socket; (void)address; (void)length; return -1; }
int posix_socket_connect(int socket, const void *address, int length) { (void)socket; (void)address; (void)length; return -1; }
int posix_socket_listen(int socket, int backlog) { (void)socket; (void)backlog; return -1; }
int posix_socket_accept(int socket, void *address, int *length) { (void)socket; (void)address; (void)length; return -1; }
int posix_socket_send(int socket, const void *buffer, int length, int flags) { (void)socket; (void)buffer; (void)length; (void)flags; return -1; }
int posix_socket_sendto(int socket, const void *buffer, int length, int flags, const void *address, int address_length)
{ (void)socket; (void)buffer; (void)length; (void)flags; (void)address; (void)address_length; return -1; }
int posix_socket_recv(int socket, void *buffer, int length, int flags) { (void)socket; (void)buffer; (void)length; (void)flags; return -1; }
int posix_socket_recvfrom(int socket, void *buffer, int length, int flags, void *address, int *address_length)
{ (void)socket; (void)buffer; (void)length; (void)flags; (void)address; (void)address_length; return -1; }
int posix_socket_shutdown(int socket, int how) { (void)socket; (void)how; return -1; }
int posix_socket_set_nonblocking(int socket, int nonblocking) { (void)socket; (void)nonblocking; return -1; }
int posix_socket_bytes_available(int socket, posix_ulong *count) { (void)socket; if (count) *count = 0; return -1; }
int posix_socket_set_nodelay(int socket) { (void)socket; return -1; }
int posix_socket_setsockopt(int socket, int level, int name, const void *value, int length)
{ (void)socket; (void)level; (void)name; (void)value; (void)length; return -1; }
int posix_socket_getsockopt(int socket, int level, int name, void *value, int *length)
{ (void)socket; (void)level; (void)name; (void)value; (void)length; return -1; }
int posix_socket_getsockname(int socket, void *address, int *length) { (void)socket; (void)address; (void)length; return -1; }
int posix_socket_getpeername(int socket, void *address, int *length) { (void)socket; (void)address; (void)length; return -1; }
int posix_socket_select(int *read, int *read_count, int *write, int *write_count,
	int *error, int *error_count, posix_long timeout_seconds, posix_long timeout_microseconds, int infinite)
{
	(void)read; (void)write; (void)error; (void)timeout_seconds; (void)timeout_microseconds; (void)infinite;
	if (read_count) *read_count = 0;
	if (write_count) *write_count = 0;
	if (error_count) *error_count = 0;
	return 0;
}
posix_ulong posix_local_ipv4_address(void) { return 0; }
posix_ulong posix_resolve_ipv4(const char *host) { (void)host; return 0; }
#endif

/* (the campaign build's; the multiplayer build makes keys from its bytes, and
has its own: psp_internet.c) */
#ifndef PSP_MULTIPLAYER
void posix_random_bytes(void *buffer, posix_ulong size)
{
	static SceKernelUtilsMt19937Context context;
	static int seeded = 0;
	unsigned char *bytes = buffer;
	posix_ulong index;

	if (!seeded)
	{
		sceKernelUtilsMt19937Init(&context, (u32)sceKernelGetSystemTimeLow());
		seeded = 1;
	}
	for (index = 0; index < size; index++)
		bytes[index] = (unsigned char)sceKernelUtilsMt19937UInt(&context);
}
#endif

int posix_upnp_forward_udp(unsigned short port, unsigned short preferred_port, posix_ulong *external_address,
	unsigned short *external_port, char *error, int error_size)
{
	(void)port; (void)preferred_port; (void)external_address; (void)external_port;
	if (error && error_size > 0)
		snprintf(error, error_size, "no network on the PSP yet");
	return 0;
}
void posix_upnp_stop_forwarding_udp(unsigned short external_port) { (void)external_port; }
int posix_command_line_argument(int index, char *buffer, posix_ulong size) { (void)index; (void)buffer; (void)size; return 0; }
posix_ulong posix_process_id(void) { return 1; }
int posix_register_url_scheme(const char *scheme, const char *description) { (void)scheme; (void)description; return 0; }
int posix_user_secret(unsigned char *secret, int size) { (void)secret; (void)size; return 0; }
int posix_discord_connect(void) { return -1; }
int posix_discord_write(int handle, const void *buffer, int length) { (void)handle; (void)buffer; (void)length; return -1; }
int posix_discord_read(int handle, void *buffer, int length) { (void)handle; (void)buffer; (void)length; return -1; }
void posix_discord_close(int handle) { (void)handle; }

/* build 92: for the screenshot stamp (psp_ge.c) */
int psp_build_number(void)
{
	return PSP_BUILD_NUMBER;
}
