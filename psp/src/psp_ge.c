/*
PSP_GE.C

The first slice of the PSP's own renderer: the level (the BSP) drawn by the
GE, the PSP's graphics chip, from the player's camera. Flat colors, one per
material, faded by distance fog, so the shapes read. No textures, objects,
sky or HUD yet; the game's own (null) Direct3D path still runs beside it.

How the geometry reaches the GE without copying:
- The game keeps the level's vertices in the map, 32 bytes each: position
  (3 floats), normal, binormal and tangent (packed, 4 bytes each), texture
  coordinate (2 floats). The GE reads vertices in a fixed component order,
  ending with the position, so it can't start a vertex at a position.
- Asked for a texture coordinate (8 bytes), a normal (12) and a position (12),
  the GE reads 32-byte vertices with the position at the end. Started 20
  bytes before the game's buffer, each GE vertex ends on the game vertex's
  position; the texture coordinate and normal it reads are the previous
  vertex's packed vectors, and with texturing and lighting off the GE never
  uses them.
- The triangles are the game's own list of the visible surfaces, built each
  frame (16-bit indices into the material's vertices).

Near the camera the GE needs help: it drops a whole triangle when a corner
falls behind the camera or lands too far off the screen (its coordinates
only reach 2048 pixels either side), and the ground under the player is
large triangles that do both. So every triangle is first checked on the CPU
against the camera: wholly outside the view, it is skipped; safely in front,
it goes to the GE as it is; otherwise the CPU cuts it to the part inside a
widened view (the near plane and four "guard band" planes) and the pieces
are drawn at the end of the frame, already in camera space.

Memory: the frame buffers and the depth buffer take the first 816 KB of
VRAM (16-bit color, 512 pixels a row); the on-screen log lives above them
(psp_platform.c) and the null renderer's targets never draw.
*/

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <pspgum.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdio.h>
#include <malloc.h>
#include <pspge.h>
#include "psp_paths.h"   /* ms0:/halo or ms0:/halomp, and the maps */
#include <stdarg.h>
#include "psp_me.h" /* build 118: psp_on_me, PSP_ME_WORK, the submit/finish jobs */
#include "psp_lanetrace.h"

void platform_log(const char *format, ...);
/* build 112: the GE layer logs through ge_log (defined with the deferral's
machinery): platform_log on the main CPU; while the work is marked as the
Media Engine's (gemesim), only a count per message, written out by the main
CPU at the frame's end, since formatting and the log's file writes take
locks and system calls. (platform_log) in parentheses is the real one */
static void ge_log(const char *format, ...);
#define platform_log(...) ge_log(__VA_ARGS__)
#define PSP_CLAMP_255(x) ((x) < 0 ? 0 : (x) > 255 ? 255 : (x))

/* build 106: the renderer's own timers (a part's or an entry's time, for
the GE lines) read the system clock, a system call, hundreds of times a
frame: "notimers" in keep.txt skips them, and their reports then read 0.
The frame's own timing, which the budget lines need, stays */
extern int psp_timers_off;
/* build 112: 0 too while the work is marked as the Media Engine's
(gemesim): the main CPU's clock is a system call the ME can't make; the ME
version will read the ME's own cycle counter */
extern volatile unsigned int psp_me_context;
static inline unsigned int ge_clock(void)
{
	return psp_timers_off || PSP_ME_WORK() ? 0 : sceKernelGetSystemTimeLow();
}

/* build 119: "probes": the finer timers that split a hot loop from its
call (the near corner loop and bone slots, the level's passes by piece, the
light draw before, in and after its triangle loop, the decal calls by
piece). Each reading costs ~0.6 us on the PSP, so they are off unless the
word is set: off, they read no clock and log nothing. Read at launch
(gebusy_start) */
static int probes_on = 0;
static inline unsigned int probe_clock(void)
{
	return probes_on ? ge_clock() : 0;
}

/* build 112: a range written back from the data cache for the GE to read:
the firmware's call on the main CPU, as before; while the work is marked as
the Media Engine's (gemesim), the cache instruction on each line instead (no
system call; on the ME it writes back the ME's own cache, the one holding
the data), each line written back and dropped as the list's lines are */
static void ge_writeback(const void *start, unsigned int bytes)
{
	unsigned int line, end;

	if (!PSP_ME_WORK())
	{
		sceKernelDcacheWritebackRange(start, bytes);
		return;
	}
	end = (unsigned int)start + bytes;
	for (line = (unsigned int)start & ~63u; line < end; line += 64)
		__builtin_allegrex_cache(0x1B, line);
}

#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 272
#define BUFFER_WIDTH 512
#define FRAME_BYTES (BUFFER_WIDTH * SCREEN_HEIGHT * 2)
/* 256 KB (build 33): on the PSP, firing at walls filled the 128 KB list
(decals pile up, shadows copy their receivers in), and the HUD, drawn last,
was what got cut */
/* 384 KB (build 40): sustained fire at a wall still filled 256 KB (decals
copy the wall's triangles under them, every spark is a sprite) */
/* 512 KB (build 107): b106 on b30's beach filled 384 KB in the fight (models
175 KB a frame, up to 235 model and 850 decal draws a frame skipped; the
flicker the tester saw). Before the level's first draw the models may use only
the list less the HUD's, the see-through draws' and the level's reserves
(about 200 KB of 384). The 128 KB come from the heap; the start-up buffers
psp_memory.c now places outside it gave back more than that */
#define DISPLAY_LIST_BYTES 0x80000
/* build 113: 768 KB with "list768" (b113_candidates.md #19). b112 run 1's
approach to b30's beach skipped 46 KB of model draws a frame with the list
full; b112 run 5's cliffs 4,844 draws in 30 s. The list comes from the heap,
which b111's long run left about 900 KB of untaken. Set when the GE starts */
#define DISPLAY_LIST_BYTES_LARGE 0xC0000
/* build 114: 768 KB by default, "list512" for b113's. b113's Test 2 on the
PSP: no draw skipped with the list full (Test 1: 13,518 by game time 1,200,
the trees blinking), and the heap took 5,363 of 5,888 KB by game time 4,000
with sound1792 too */
static int display_list_size = DISPLAY_LIST_BYTES_LARGE;
/* build 93: the list's size this frame (benchrun's "display list in VRAM"
puts it in VRAM's last 256 KB, which psp_tex.c keeps free during benchrun) */
#define VRAM_BENCH_START 0x001C0000
#define VRAM_BENCH_BYTES 0x40000
/* build 103: benchrun's "level vertices in VRAM": the level's vertex copies
(the arrays the GE reads) built in the 256 KB below the bench room
(psp_tex.c keeps it out of the texture cache under benchrun). b102 run 5:
the GE's main-RAM reads cost the CPU 8-18 ms a frame, texels none of it,
and b87/b93's 16-bit level vertices (half the bytes) changed nothing: if the
cost is the number of scattered fetches rather than their bytes, only
taking the fetches off the main bus shows it */
#define VRAM_VERTEX_START 0x00180000
#define VRAM_VERTEX_BYTES 0x40000
static int level_vram = 0;
static unsigned int vram_vertex_next = 0;
static unsigned long vram_vertex_refused = 0, vram_vertex_streams = 0;

/* every piece of a level copy comes from copy_alloc and goes back through
copy_free: the copies' arena first, then the level carve, the general heap
when neither has room (both are with the copies, below) */
static void *copy_alloc(unsigned int bytes, void **owner);
static void copy_free(void *pointer);

/* the arrays the GE reads (the float copies): benchrun's VRAM room first.
owner: the field that will hold the address (the arena moves blocks) */
static void *stream_alloc(unsigned int bytes, void **owner)
{
	if (level_vram)
	{
		unsigned int at = (vram_vertex_next + 63) & ~63u;

		if (at + bytes <= VRAM_VERTEX_BYTES)
		{
			vram_vertex_next = at + bytes;
			vram_vertex_streams++;
			return (void *)(0x04000000 + VRAM_VERTEX_START + at);
		}
		vram_vertex_refused++;
	}
	return copy_alloc(bytes, owner);
}

static void vm_level_forget(void *vertices); /* the copies' room, below */
static void stream_free(void *pointer)
{
	/* VRAM is given back all at once, when the level's copies are let go */
	if (((unsigned int)pointer & 0x0F000000u) == 0x04000000u)
		return;
	vm_level_forget(pointer);
	copy_free(pointer);
}
static int list_bytes = DISPLAY_LIST_BYTES;

/* build 93: benchrun's bus tests. The CPU builds every frame the same way;
these only stop the GE from drawing a kind of draw (and so from reading its
vertices): GE_SKIP_LEVEL the level's draws, GE_SKIP_MODELS the models'
parts, GE_SKIP_ALL every draw (the VRAM-only load). ge_category says which
hook is drawing */
#define GE_SKIP_LEVEL 1
#define GE_SKIP_MODELS 2
#define GE_SKIP_ALL 4
static int ge_skip = 0, ge_category = 0;
static unsigned long ge_skipped_draws = 0;

/* build 112: the ceiling bench's "GE layer skipped" (psp_bench.h). Where the
bus tests above keep the CPU's work and stop the GE, this drops every draw the
engine hands this file, on arrival: the engine still does all of its own
drawing work, and this file's CPU work and the GE's reading are gone. What it
saves is the most that moving this file's work to the Media Engine could save
(an upper bound: the ME's version keeps the GE's reading) */
#include "../include/psp_bench.h" /* the plain-C units have no -I for psp/include (psp_build.py) */
#include "../include/psp_seethrough.h" /* build 116: a see-through part's fade, packed with its blend */
#define GE_BENCH_SKIP(...) do { if (psp_bench_skip & PSP_SKIP_GE) return __VA_ARGS__; } while (0)

/* ---------- build 112: the GE layer's calls deferred (keep word "gedefer")

The first steps toward the Media Engine building the GE's lists
(docs/big_levers.md, lever L1). Every call the engine makes that writes the
list is written down as it arrives, with each array it points to copied
(node matrices, lights, colours, centroids, a sprite group's corners, a text
character's, a screen quad's), and carried out later in the same order:
when the buffer is full, when a call that must run at once arrives (it
carries out what is waiting first: GE_DEFER_DRAIN), or at the frame's end.

Kept as pointers, since they stay put until the frame ends: the geometry
cache's blocks (protected for RECENT_FRAMES, psp_geo.c), the level's vertices
and the frame's dynamic triangle lists, the decals' vertices (written in the
tick, read while drawing).

Run at once, after what is waiting: the frame's start and end
(psp_ge_window_begin decides the near plane from the level and the player;
psp_ge_frame_end hands the list over), and the calls whose answer the engine
uses (the level vertex queries) or
that change data a waiting call reads (psp_ge_text_changed), and the map
loads. A model with more nodes than DEFER_MAX_NODES runs as it comes
(defer_paused), since the engine reuses its node matrices' buffer for the
next model; so does an array too large for the buffer.

On the main CPU the list comes out the same (the same calls, in the same
order); only the CPU's work is reordered: the engine's half for a run of
calls, then this file's half for them, so each half's code stays in the
instruction cache longer. The ME step moves the carrying out to the ME. Off
(the default): one test a call. */
#define DEFER_WORDS (64 * 1024 / 4)
#define DEFER_LARGEST (DEFER_WORDS / 4) /* a record past this runs as it comes */
#define DEFER_MAX_NODES 64
#define NODE_FLOATS 13 /* a node matrix as the game keeps it (halo_matrix): scale, three axes, position */
#define SCREEN_VERTEX_WORDS 5 /* struct game_screen_vertex: x, y, u, v, argb */
#define SPRITE_VERTEX_WORDS 6 /* struct game_sprite_vertex: x, y, z, u, v, argb */
#define METER_WORDS 7 /* struct psp_seethrough_meter (the game's rasterizer_meter_parameters) */
#define LIGHT_GEL_FLOATS 28 /* psp_ge_light_gel: six face colours, the gel's forward, left and up, then its cube's key */
enum
{
	DEFER_MODEL_BEGIN = 1, DEFER_MODEL_SHAPE, DEFER_MODEL_EFFECT, DEFER_MODEL_MULTIPURPOSE, DEFER_DRAW_MODEL,
	DEFER_QUEUE_TRANSPARENT, DEFER_FOG, DEFER_WINDOW_TARGET, DEFER_LEVEL_DETAIL, DEFER_DRAW_LEVEL, DEFER_SKY,
	DEFER_SPRITES, DEFER_WIDGET_BEGIN, DEFER_WIDGET_TEXTURE, DEFER_WIDGET_OCCLUSION, DEFER_WIDGET_SPRITE,
	DEFER_QUEUE_LEVEL_TRANSPARENT, DEFER_HUD_ZOOM, DEFER_UI_CENTERED, DEFER_UI_SPACE, DEFER_SCREEN_QUAD,
	DEFER_SHADOW_BEGIN, DEFER_SHADOW_DRAW, DEFER_SHADOW_END, DEFER_LIGHT_BEGIN, DEFER_LIGHT_DRAW, DEFER_SCREEN_FLASH,
	DEFER_DECAL, DEFER_BLIPS_BEGIN, DEFER_BLIP, DEFER_BLIPS_END, DEFER_TEXT_CHARACTER, DEFER_TEXT_END,
	DEFER_TEXT_BEGIN, DEFER_CONTRAIL, DEFER_ZOOM_MASK /* build 113 */,
	DEFER_FOG_PLANAR, DEFER_SEETHROUGH_TINT, DEFER_SEETHROUGH_METER /* build 116 */,
	DEFER_MODEL_POINT_LIGHT, DEFER_MODEL_DETAIL, DEFER_FOG_SUBMERGED, DEFER_MODEL_REFLECTION, DEFER_LIGHT_GEL, DEFER_LEVEL_GLOW,
	DEFER_FRAME_END, DEFER_WINDOW_BEGIN /* last: REPLAY_SERVED follows it */
};
typedef union
{
	unsigned int u;
	int i;
	float f;
	const void *p;
} defer_word;
static defer_word *defer_buffer = NULL;
static float *defer_nodes = NULL; /* the carried-out model's node matrices: model_nodes points here */
/* "gemesim" (with gedefer): the carrying out marked as the Media Engine's
work while it still runs on the main CPU. This word holds the game thread's
id meanwhile; the emulator's ME-strict check (PSP_MESTRICT, a patched
emu.sh) prints every system call that thread makes then: each is a gate the
ME version must not reach (big_levers.md, L1 step 3) */
volatile unsigned int psp_me_context = 0;
static unsigned int defer_me_thread = 0;
/* and the GE's stall address: while the work is the Media Engine's, the
newest one is kept here (the call is a system call; the stall wrapper, below)
and the main CPU passes it on when the run of waiting calls ends */
static int stall_me_qid = 0;
static void *stall_me_address = NULL;
static unsigned long stall_forwarded; /* (defined with the stall wrapper) */
int __real_sceGeListUpdateStallAddr(int qid, void *stall);
/* gebusy2 (with the stall wrapper): the Media Engine's chunks given to the
GE, and the main CPU's waits, for the GE's own busy time */
static void gebusy_me_handed(unsigned int stall);
static void gebusy_wait(int begin, int which);
static PSP_MAIN_SIDE unsigned int defer_used = 0;
/* each time the buffer starts over (rso: a span of records is one object's
only if none came between its first and last) */
static PSP_MAIN_SIDE unsigned long defer_resets = 0;
/* rso (with its code, below): the windows written down, and its words */
static PSP_MAIN_SIDE unsigned long rso_stamp = 16;
static PSP_MAIN_SIDE int rso_asked = -1;
static void rso_window(void);
static PSP_MAIN_SIDE int defer_on = 0, defer_replaying = 0, defer_paused = 0;
/* gemesim or gemeon: the carrying out is (or stands for) the Media Engine's,
so what it can't do is done when the call is written down, on the main CPU
(the see-through level triangles' wall answers) */
static int defer_me_semantics = 0;
/* build 118: "gemeoverlap" (with gedefer and gemeon): L1 step 4, the Media
Engine carrying out one half of the buffer while the main CPU writes the
next calls into the other (docs/big_levers.md, section 4). A half goes to
the ME when it is full; the main CPU waits for it only when it needs the
other half back, or at a call that runs at once (the frame's start and end,
the level vertex queries, text updates), which then runs with the ME idle,
as without the overlap. The list comes out the same: the same calls, in the
same order, carried out by one CPU at a time.
What each CPU writes while a half is out, on cache lines of its own (each
CPU's cache writes back whole 64-byte lines, so a line both write loses one
CPU's bytes): the ME, the GE layer's state, pspgu's, the list, the wanted
lists; the main CPU, the records' other half and the recorder's words
(defer_used and the rest). The emulator runs no ME code: there the half is
carried out at once on the main CPU, marked as the ME's work (gemesim), and
psp_me_window says a half is out, for the emulator's write trace (keep word
mewrites, the patched emulator's PSP_MEWRITES), which lists every line both write */
#define DEFER_HALF (DEFER_WORDS / 2)
static PSP_MAIN_SIDE int me_overlap = 0;      /* this session overlaps (decided once) */
static PSP_MAIN_SIDE int defer_half = 0;      /* the half being written */
static PSP_MAIN_SIDE defer_word *defer_base = NULL; /* its first word */
static PSP_MAIN_SIDE unsigned int defer_limit = DEFER_WORDS; /* its words */
static PSP_MAIN_SIDE int me_out = 0;          /* the other half is out with the ME */
static PSP_MAIN_SIDE int defer_me_on = -1; /* gemeon (asked once; mebench sets it) */
static PSP_MAIN_SIDE int me_wrote = 0;        /* the ME has carried a half out since the main CPU last dropped its cache */
static PSP_MAIN_SIDE unsigned long me_overlap_runs = 0, me_overlap_waits = 0, me_overlap_wait_us = 0, me_overlap_alone = 0;
/* a line of its own: only the main CPU writes it */
volatile unsigned int psp_me_window[16] __attribute__((aligned(64)));
/* build 118: "mebench": the three ways of running the GE layer's work, in
turns of ME_BENCH_FRAMES frames in one run, each frame's time (swap to swap,
the frame pacing off) added to its way: on the main CPU as it comes (gedefer
off), on the ME with the main CPU waiting (gemeon), and overlapped
(gemeoverlap). One build and one run answer what the overlap is worth on the
PSP, in the same stretch of play (the hardware tests' golden rule) */
#define ME_BENCH_FRAMES 60
static PSP_MAIN_SIDE int me_bench = -1, me_bench_way = 0, me_bench_settled = 0;
static PSP_MAIN_SIDE unsigned long me_bench_frame = 0, me_bench_frames[3], me_bench_us[3];
/* the wall answers a record carried, while it is carried out (the replay of
DEFER_QUEUE_LEVEL_TRANSPARENT sets it; overlay_answer reads it) */
static const unsigned char *overlay_answers = NULL;
static int overlay_answers_for(const void *vertices, int vertex_count, const unsigned short *indices,
	long triangle_count, unsigned char *out);
static PSP_MAIN_SIDE unsigned long defer_records = 0, defer_drains = 0, defer_drains_full = 0, defer_big_models = 0, defer_too_large = 0;
/* "geinflight" (with gedefer; docs/frames_in_flight.md): the frame's end and
the next frame's first window are written down too, so a frame is carried out
only at the next frame's first call, after that frame's ticks: the order the
Media Engine version will run in (the ME carrying out frame N while the main
CPU ticks N+1). Carried out here on the main CPU, it moves no work; what it
gives is that order, so the emulator's drawhash and statecrc show anything the
next ticks change under a frame still waiting. A frame is written down whole,
so the buffer is sized for b30's busiest (~100 KB a frame, recon_134.md).
A frame larger than the buffer is carried out in part when it fills, in the
engine walk, and the main CPU waits for that part: on the Media Engine
(gemeon) it sleeps through the ME's replay of it. b30's flight and landing
fill it in 24-100% of frames, and the PSP-1000 waits 15-53 ms a frame there
(docs/design_inflightkb.md). "inflightkbN": the buffer N KB (64 to
1024) instead, so those frames go out whole after the next ticks. Exact: the
same calls carried out in the same order; only when the part runs moves */
#define DEFER_INFLIGHT_KB 128
#define DEFER_INFLIGHT_KB_MIN 64
#define DEFER_INFLIGHT_KB_MAX 1024
static PSP_MAIN_SIDE int inflight_on = 0;
static PSP_MAIN_SIDE int inflight_pending = 0; /* a frame's end is written down and waits */
/* the frame being written down has written down its window's start: the
GE layer's own frame_open stays the last frame's (shut at its show) until
that start is carried out, so the answers the walk needs now (whether text
is drawn) take this instead. "inflighttextold": frame_open, as before, which
left out every string of a frame that hadn't filled the buffer by then */
static PSP_MAIN_SIDE int inflight_window_written = 0, inflight_text_old = 0;
static PSP_MAIN_SIDE unsigned long inflight_text_early = 0;
/* the buffer in the texture RAM block's front room rather than the heap
(psp_tex_inflight_room), and the block's generation when the records now
in it were begun: a sleep gives the block back, so records begun before a
new generation are not carried out (inflight_room_lost) */
static PSP_MAIN_SIDE int inflight_room = 0;
static PSP_MAIN_SIDE unsigned int inflight_room_generation = 0;
static PSP_MAIN_SIDE unsigned long inflight_room_drops = 0;
void *psp_tex_inflight_room(unsigned int *generation);
static int frame_open; /* (defined with the GE's state, below) */
static int inflight_room_lost(void);
static void inflight_room_check(int mid_frame);
/* the firmware saves the GE's and the ME's eDRAM into the block as the PSP
goes to sleep, so the game thread writes no record there from the power
callback (away) until the wake has the block back: it waits at its next
record (parked), and the callback waits for that before giving the block
back (psp_ge_inflight_room_suspend) */
static volatile int inflight_room_away = 0, inflight_room_parked = 0;
/* gedefer asked (-1 not yet; 0 off, also for good when the buffer has no
room: defer_frame_begin, inflight_room_check) */
static PSP_MAIN_SIDE int defer_asked = -1;
/* the waiting frame's list has ended (its window was open): the main CPU's
part of its end runs once the run is over (defer_drain). Written in the
run, read after it */
static int inflight_list_ended = 0;
static void inflight_show(void);
/* geinflight with gemeon (F1): the frame is handed to the Media Engine as its
end is written down, and carried out there while the main CPU ticks the next
frame. While it is out, the ME publishes each stall address it reaches in an
uncached pair (the address, the list's id) and a thread of the main CPU's
passes it to the GE, so the GE draws behind the ME rather than after it */
static PSP_MAIN_SIDE int inflight_me_out = 0;
static PSP_MAIN_SIDE unsigned long inflight_handed = 0, inflight_me_failed = 0;
static volatile unsigned int inflight_stall_word[16] __attribute__((aligned(64))); /* [0] address, [1] id: written uncached */
#define INFLIGHT_STALL ((volatile unsigned int *)(0x40000000u | (unsigned int)inflight_stall_word))
static void inflight_hand_over(void);
/* "mechunk" (lever MECHUNK, docs/design_mechunk.md): with geinflight, gemeon
and geshow3, the frame is handed to the Media Engine in pieces as it is
written down, not whole at its end, so the ME draws it beside the record
instead of after it while the main CPU sleeps (a10's armoury: the record ~34
ms, then 27 ms asleep for the ME's 30). One job a frame, with a moving end:
the main CPU writes its cache back and publishes how far the records are
whole (a record is whole once the next one is taken); the ME replays up to
there, drops its cache, looks again, and leaves at the final mark (the frame's
end). Only while every texture the published records bind was marked this
frame (psp_tex.c keeps a marked texture for the frame, so the main CPU's
loads meanwhile never move one the ME is binding); the first that wasn't,
or a call that runs at once, stops the publishing for the rest of the frame,
which then goes over whole at its end as before. The gate: a frame chunks
only when the last one kept the main CPU waiting for the ME. Off by default */
static PSP_MAIN_SIDE int chunk_on = -1, chunk_all = 0, chunk_frame = 0, chunk_stopped = 0;
static PSP_MAIN_SIDE unsigned int chunk_words = 2048, chunk_published = 0, chunk_gate_us = 2000;
static PSP_MAIN_SIDE unsigned int chunk_wait_last_us = 0;
static PSP_MAIN_SIDE unsigned long chunk_frames = 0, chunk_gated = 0, chunk_publishes = 0, chunk_publish_us = 0,
	chunk_stops_texture = 0, chunk_stops_drain = 0, chunk_simulated = 0;
/* [0] the records' end published (words), [1] the final mark, [2] how far the
ME has replayed, [3] the ME's looks: written uncached */
static volatile unsigned int chunk_word[16] __attribute__((aligned(64)));
#define CHUNK_LINE ((volatile unsigned int *)(0x40000000u | (unsigned int)chunk_word))
static void chunk_publish(int final);
static void chunk_piece(void);
static PSP_MAIN_SIDE int chunk_out = 0; /* the frame out with the ME went in pieces */
extern int psp_tex_chunking, psp_tex_chunk_missed; /* psp_tex.c */
extern int psp_me_simulated(void); /* psp_me.c */
extern void psp_me_touch(int which);
/* "geshow2" (with geinflight and gemeon): the frame shown off the game
thread. Without it the game thread, at the next frame's first call, waits for
the ME to end frame N and then for the GE's tail, the swap and the list's
start before it records N+1. With it the game thread waits for the ME only:
the forwarding thread waits out the GE's tail, shows N (the swap, the
picture's hash, the overlay, a shot) and starts the one display list again,
while the game thread records N+1 into the one record buffer. N+1 goes to the
ME only once the list is started (it draws into the buffer the swap frees),
so a record shorter than the GE's tail waits the rest at its frame's end.
What the GE may still read of N while N+1 is recorded is kept for it until
the show: textures drawn in N (psp_tex.c's room), the level copies drawn in N
(not let go; a move or a compaction waits for the show), and the engine's own
index lists, which the walk of N+1 rewrites (copied into the list where a
level draw would hand the GE the engine's). Calls that run at once wait for
the show as they wait for the ME (docs/frames_in_flight.md, S10a) */
static PSP_MAIN_SIDE int show2_on = -1;
static PSP_MAIN_SIDE int show2_now = 0; /* this show goes to the forwarding thread */
/* 1 from the show's asking until the GE has ended the frame and the list is
started again: the GE may still read the shown frame's data. psp_tex.c
reads it to spare last frame's textures */
volatile int psp_ge_show_busy = 0;
static volatile int show2_asked = 0;
/* the replay copies the engine's index lists into the list (the ME reads it:
set once, before the first hand-over) */
static int show2_copy_indices = 0;
/* the engine's index buffer taken from its two ends in turn, waiting for
the show, instead (psp_ring_place, psp_fixups.py): the copy took list room,
and a10's full list then skipped draws plain draws. Keep word "geshow2copy":
the copy */
static int show2_ring = 0;
static PSP_MAIN_SIDE unsigned long show2_ring_waits = 0;
static PSP_MAIN_SIDE int show2_owed = 0; /* a show asked whose end the game thread hasn't taken yet */
static PSP_MAIN_SIDE int show2_list_started = 0; /* the forwarding thread starts the next frame's list */
static PSP_MAIN_SIDE SceUID show2_sema = -1;
enum { SHOW2_HAND_OVER, SHOW2_DRAIN, SHOW2_COMPACT, SHOW2_FOLLOW, SHOW2_OTHER, SHOW3_ME_END, SHOW3_ROOM, SHOW2_WHYS };
static PSP_MAIN_SIDE unsigned long show2_shows = 0, show2_waited[SHOW2_WHYS], show2_wait_us[SHOW2_WHYS];
static PSP_MAIN_SIDE unsigned long show2_spared = 0;
/* written by the replay, as the frame's other draw counts */
static unsigned long show2_index_copies = 0, show2_index_bytes = 0;
static void show_picture(int show3);
static void show2_list_start(void);
static void show2_settle(int why);
/* "showlat": each frame's time from its window's start (its ticks done, its
camera set) to the swap that shows it, for the latency geshow2 adds */
static PSP_MAIN_SIDE int show_lat_on = -1;
static PSP_MAIN_SIDE unsigned int frame_begin_us = 0, show_begin_us = 0;
static PSP_MAIN_SIDE unsigned long show_lat_frames = 0, show_lat_sum = 0, show_lat_most = 0;
static PSP_MAIN_SIDE unsigned long win_lat_frames = 0, win_lat_sum = 0, win_lat_most = 0;
static PSP_MAIN_SIDE unsigned long show_frame_number = 0; /* the frame's number, for framecrc */
static PSP_MAIN_SIDE int show_shot = 0; /* the frame being shown takes a screenshot */
/* "geshow3" (with geinflight and gemeon; it uses geshow2's forwarding
thread): the ME replays the next frame into a second display list while the
GE draws the last one's tail. Under geshow2 the frame is the ME's job plus
the GE's tail, because the ME's replay of N+1 waits until the GE has ended N
and the one list is started again. Here the game thread starts N+1's list
itself, in a part of the display list N's doesn't use, and hands N+1 to the
ME without waiting for N's show:
- N+1 draws into the buffer N's swap frees, so the swap's pointers move on the
  game thread as N is asked to be shown (the replay reads them), and the
  display is set by the forwarding thread only once the GE has reached N's
  finish (its id: show3_finish_callback). Until then every stall address of
  N+1's list is held back (show3_stall_held), so the GE can't start N+1 on the
  buffer still on the screen.
- At most two frames are out with the GE: the game thread takes N's show at
  the ME's end of N+1 (inflight_show), before N+1's frame end lets anything
  go, so at every place that reuses what the GE reads only N+1 is still
  unshown: the protections of geshow2 hold (textures, level copies, the index
  lists copied into the list, the geometry cache's and the bind blocks' last
  two frames, the VRAM copies' quarantine), and N+1's list never overlaps N's.
- The forwarding thread now runs beside the ME, so it writes no pspgu state
  and no variable the ME writes near (only PSP_MAIN_SIDE ones).
"geshow3check": each of those places counts the reuses whose last use was by
a frame handed over and not yet shown (logically: from its hand-over to the
game thread's taking of its show). The count, 0 for a sound run, is on the
GE: geshow3: line */
static PSP_MAIN_SIDE int show3_on = 0, show3_check = 0;
static PSP_MAIN_SIDE int show3_now = 0; /* this frame's list finished with its id, its show to the forwarding thread */
enum { SHOW3_TEX, SHOW3_BIND, SHOW3_GEO, SHOW3_LEVEL, SHOW3_VM, SHOW3_LIST, SHOW3_KINDS };
static const char *const show3_kind_name[SHOW3_KINDS] = { "textures", "bind blocks", "geometry", "level copies", "VRAM copies", "list room" };
/* each kind's frame counter as it was while a frame out with the GE was
recorded ([0] the oldest), and how many frames are out */
static PSP_MAIN_SIDE unsigned int show3_out_stamp[2][SHOW3_KINDS];
static PSP_MAIN_SIDE int show3_out = 0;
static PSP_MAIN_SIDE unsigned long show3_reused[SHOW3_KINDS], show3_checked[SHOW3_KINDS];
/* the display list's part (byte offsets) the frame out uses, this frame's
start, and the largest list lately (decaying), for the next room's size */
static PSP_MAIN_SIDE unsigned int show3_list_from = 0, show3_list_to = 0, show3_list_start = 0, show3_list_peak = 0;
static PSP_MAIN_SIDE void *show3_buffer = NULL; /* the buffer the frame being shown drew into */
static PSP_MAIN_SIDE unsigned long show3_rooms_shared = 0, show3_rooms_whole = 0, show3_held = 0;
/* written by the replay (a draw skipped for want of room in a shared part,
which must never happen; a part grown into the rest of the list above it, or
by a jump to the list's start) */
static unsigned long show3_room_skips = 0, show3_grown = 0, show3_jumped = 0;
/* the part this frame's list is in, set as the list starts (byte addresses,
the uncached view's bit off): its end (0 for the whole list: the limits alone
keep it inside), and what it can grow into once the GE has ended the frame
out (SHOW3_GROW_UP: the list's end, for a part below that frame; SHOW3_GROW_JUMP:
the list's start up to this part's, for a part above it). The replay reads and
moves them */
enum { SHOW3_GROW_NONE, SHOW3_GROW_UP, SHOW3_GROW_JUMP };
static unsigned int show3_part_end = 0, show3_part_grow = SHOW3_GROW_NONE, show3_part_base = 0, show3_part_next = 0;
static int show3_part_jumped = 0;
/* [0] the last finish id the GE reached (its interrupt), [1] the one the
frame out ends with (the game thread, as the part is taken): uncached, as the
ME waits on them */
static volatile unsigned int show3_ge_word[16] __attribute__((aligned(64)));
#define SHOW3_GE_WORD ((volatile unsigned int *)(0x40000000u | (unsigned int)show3_ge_word))
static int show3_room(unsigned int needed);
/* the finish id each shown frame's list ends with, and the last one the GE
reached (written by the GE's interrupt) */
static PSP_MAIN_SIDE unsigned int show3_asked = 0;
static PSP_MAIN_SIDE volatile unsigned int show3_finished = 0, show3_shown = 0;
static PSP_MAIN_SIDE SceUID show3_flag = -1;
/* the list started while the last frame wasn't shown: its stall address
waits for that show (show3_gate_show), the newest kept */
static PSP_MAIN_SIDE volatile int show3_gate_qid = -1;
static PSP_MAIN_SIDE volatile unsigned int show3_gate_show = 0;
static PSP_MAIN_SIDE void *volatile show3_gate_stall = NULL;
#define SHOW3_ROOM_LEAST (192 * 1024)
static void show3_finish_callback(int id); /* (with gebusy's) */
static void show3_stamp_out(void); /* (with the VRAM copies') */
static int ge_stall_pass(int qid, void *stall);
void psp_ge_show3_reuse(int kind, unsigned int stamp);
extern int psp_display_hold; /* psp_trace.c */
int psp_display_release(void);
extern volatile int psp_ge_enqueued;
int sceGuFinishId(unsigned int id); /* pspgu: sceGuFinish with the FINISH's id */
static PSP_MAIN_SIDE void *shot_buffer = NULL; /* the buffer screenshot() reads (the shown frame's) */
static PSP_MAIN_SIDE int stall_force; /* (defined with the stall wrapper) */
static int inflight_finish_left = 0; /* geinflight: the list's finish waits for the main CPU */
/* set by the frame's job itself (its args), so the ME never reads the main
CPU's words for it: publish each stall address, and serve the ring's jobs
between records (geinflight) */
static int replay_inflight = 0;
static PSP_MAIN_SIDE int replay_kinds_on = 0; /* "mekinds" (see defer_replay) */
static PSP_MAIN_SIDE unsigned long inflight_frames = 0, inflight_words = 0, inflight_words_most = 0;
/* the whole frame's words, the parts carried out when the buffer filled
included (inflight_words counts only what is left at the sync) */
static PSP_MAIN_SIDE unsigned long inflight_part_words = 0, inflight_whole_words = 0, inflight_whole_most = 0;
static PSP_MAIN_SIDE unsigned int inflight_buffer_words = 0;
extern int psp_game_running(void); /* psp_fixups.py: players.c */
static void defer_drain(void);
static void defer_hand_over(void); /* build 118: gemeoverlap */
static void list_write_back(const void *stall); /* build 114: listcachedall */
static void lit_forget(int what); /* build 114: statememo */
/* which calls made the waiting calls run, for the log: each is a point where
the ME's version has to catch up with the main CPU */
#define DEFER_SITES 16
static PSP_MAIN_SIDE const char *defer_site[DEFER_SITES];
static PSP_MAIN_SIDE unsigned long defer_site_drains[DEFER_SITES];
static void defer_drain_for(const char *site)
{
	int k;

	for (k = 0; k < DEFER_SITES && defer_site[k] && defer_site[k] != site; k++)
		;
	if (k < DEFER_SITES)
	{
		defer_site[k] = site;
		defer_site_drains[k]++;
	}
	defer_drain();
}
/* a call that runs at once carries out what is waiting first, so the list's
order never changes. Only the game's thread writes the list, so only it
records and carries out; a call another thread makes (the heartbeat's
psp_ge_frame_window, psp_ge_budget, psp_ge_setting) never drains */
/* build 118: the calls are written down only on the main CPU, never while
carrying out. On the Media Engine (gemeoverlap) the main CPU's own words
(defer_used, defer_replaying) say what the main CPU is doing meanwhile, not
the ME: there the test is psp_on_me(), its own stack */
#define DEFER_RECORDING() (defer_on && !defer_replaying && !psp_on_me())
#define GE_DEFER_DRAIN() do { if (!psp_on_me() && defer_used && !defer_replaying) defer_drain_for(__func__); } while (0)
/* a deferred call's own test: deferring this frame, not already carrying
out, not inside a model too big to defer, and written down */
#define GE_DEFER(record, ...) do { if (DEFER_RECORDING() && !defer_paused && (record)) return __VA_ARGS__; } while (0)
/* "texpretouch": a draw's texture read in as its record is written, for a
replay that can't read it itself (the Media Engine's, or gemesim's standing
for it): psp_tex_pretouch says why. Only while recording, so a draw carried
out at once binds (and loads) as it always did. Not with gemeoverlap: there
the ME binds one half while this CPU writes the other, and both would write
the texture cache's residency words through caches of their own */
void psp_tex_pretouch(unsigned int key, int kind, int resident_only);
int psp_tex_has(unsigned int key);
static PSP_MAIN_SIDE int pretouch_on = 0, pretouch_all = 0;
static void pretouch(unsigned int key, int kind, int resident_only);
/* the record kinds, for psp_tex_pretouch's memo and census (psp_tex.c's
PRETOUCH_KINDS, txc_names) */
enum { TXPT_LEVEL_BASE, TXPT_LIGHTMAP, TXPT_LEVEL_DETAIL, TXPT_MODEL_BASE, TXPT_CHANGE, TXPT_GLOW, TXPT_MODEL_DETAIL,
	TXPT_SPRITE, TXPT_CONTRAIL, TXPT_WIDGET, TXPT_SEETHROUGH, TXPT_LEVEL_SEETHROUGH, TXPT_SCREEN, TXPT_ZOOM, TXPT_LIGHT,
	TXPT_DECAL, TXPT_BLIP, TXPT_BLIPS_END };
#define PRETOUCHING() (pretouch_on && !me_overlap && DEFER_RECORDING() && !defer_paused)
#define GE_PRETOUCH(key, kind) do { if (PRETOUCHING()) pretouch(key, kind, 0); } while (0)
/* the HUD's and the motion sensor's: pinned, past the frame's budget and the
ceiling (psp_tex_pretouch, "hudpinme") */
#define GE_PRETOUCH_HUD(key, kind) do { if (PRETOUCHING()) pretouch(key, kind, 3); } while (0)
/* a texture the replay binds only for some draws (a level's detail maps,
only near the eye): marked only if it's resident already, so it costs no
load and no room ("txptall": loaded too) */
#define GE_PRETOUCH_IF_RESIDENT(key, kind) do { if (PRETOUCHING()) pretouch(key, kind, !pretouch_all); } while (0)
/* a texture the replay won't bind for this record (the census, recon only,
counts the ones it binds anyway) */
#ifdef PSP_RECON
#define GE_PRETOUCH_PASSED(key, kind) do { if (PRETOUCHING()) psp_tex_pretouch(key, kind, 2); } while (0)
#else
#define GE_PRETOUCH_PASSED(key, kind) do { } while (0)
#endif
/* a frame's boundaries are written down only in a game: the menus and loads
run as with gedefer alone; and a frame after the power switch asked to sleep
isn't in flight, so it ends with its list finished and the GE done, where the
sleep is carried out (psp_sleepguard.c, B41) */
extern int psp_sleep_frame;
#define INFLIGHT_RECORDING() (inflight_on && DEFER_RECORDING() && psp_game_running() && !psp_sleep_frame)

int psp_keep(const char *name);
void psp_ge_model_begin(const float *node_matrices, int node_matrix_count, float u_scale, float v_scale,
	const float *ambient, int light_count, const float *light0, const float *light1);
void psp_ge_model_shape(int first_person, const float *centroid, float radius, float near_fp, float far_fp);
void psp_ge_model_effect(int type, float intensity);
void psp_ge_model_point_light(const float *position, unsigned int color);
void psp_ge_model_multipurpose(unsigned int multipurpose_key, unsigned int glow_key, float const *change, float const *glow);
void psp_ge_model_reflection(unsigned int cube_key, float const *rgb);
void psp_ge_model_detail(unsigned int detail_key, float u_scale, float v_scale);
void psp_ge_draw_model(void *vertices, int vertex_count, int uncompressed, const unsigned short *indices,
	long index_count, int strip, unsigned int color, unsigned int base_key, float u_scale, float v_scale,
	int alpha_tested);
void psp_ge_queue_transparent(void *vertices, int vertex_count, int uncompressed, const unsigned short *indices,
	long index_count, int strip, unsigned int key, float u_scale, float v_scale, float u_offset, float v_offset,
	short blend, const float *centroid);
void psp_ge_fog(const float *color, float minimum_distance, float maximum_distance, float density);
void psp_ge_fog_planar(float red, float green, float blue, float maximum_distance, float density);
void psp_ge_fog_submerged(float i, float j, float k, float d, unsigned int color, float density, float depth,
	float distance);
void psp_ge_seethrough_tint(unsigned int tint);
void psp_ge_seethrough_shader(unsigned int shader);
void psp_ge_seethrough_meter(const void *meter);
static void seethrough_tint_set(unsigned int tint, int flags, unsigned int shader, unsigned int face,
	unsigned int edge);
int psp_tex_shader_angle_tints(unsigned int shader, unsigned int *face, unsigned int *edge);
unsigned int psp_tex_shader_tint(unsigned int shader, int *flags);
int psp_tex_resident(unsigned int key);
void psp_ge_window_target(int target);
void psp_ge_level_detail(unsigned int primary_key, float primary_u, float primary_v,
	unsigned int micro_key, float micro_u, float micro_v);
void psp_ge_level_glow(unsigned int primary_key, float const *primary, unsigned int secondary_key,
	float const *secondary);
static void level_glow_set(unsigned int primary_key, unsigned int primary_color, unsigned int secondary_key,
	unsigned int secondary_color);
static unsigned int abgr_from_rgb(float const *rgb);
void psp_ge_draw_level(const void *vertices, const void *lightmap_vertices, int vertex_count,
	const unsigned short *indices, long triangle_count, unsigned int color,
	unsigned int base_key, unsigned int lightmap_key, int alpha_tested);
int psp_ge_contrail(void const *vertices, int vertex_count, unsigned int bitmap_key, short blend);
void psp_ge_zoom_mask(unsigned int key, unsigned int strength);
void psp_ge_sky(int on);
int psp_ge_sprites(void const *vertices, int sprite_count, unsigned int bitmap_key, short blend, float const *view_to_world);
void psp_ge_widget_begin(int depth_test);
void psp_ge_widget_texture(unsigned int key);
void psp_ge_widget_occlusion(int visible);
void psp_ge_widget_sprite(float const *point, float radius, float scale_x, float scale_y, float rotation,
	unsigned long color);
void psp_ge_queue_level_transparent(const void *vertices, int vertex_count, const unsigned short *indices,
	long triangle_count, unsigned int key, float u_scale, float v_scale, float u_offset, float v_offset, short blend,
	const float *centroid);
void psp_ge_hud_zoom(float zoom);
void psp_ge_ui_centered(int centered);
void psp_ge_ui_space(int width, int height);
void psp_ge_screen_quad(void const *vertices, unsigned int key, short blend, float red, float green, float blue,
	float fade, float offset_x, float offset_y, void const *meter_parameters, int wrapped, float u_scale, float v_scale);
void psp_ge_shadow_begin(const float *matrix, const float *color, float radius);
void psp_ge_shadow_draw(const void *vertices, int vertex_count, const unsigned short *indices, long triangle_count);
void psp_ge_shadow_end(void);
void psp_ge_light_begin(const float *position, const float *forward, const float *color, float radius,
	float cosine_falloff, float cosine_cutoff);
void psp_ge_light_gel(const float *gel);
void psp_ge_light_draw(const void *vertices, int vertex_count, const unsigned short *indices, long triangle_count,
	unsigned int base_key);
void psp_ge_screen_flash(short type, float intensity, float const *argb);
void psp_ge_decal(void const *vertices, int quad_count, unsigned int key, short blend, unsigned long color,
	int intensity, int alpha_tested, int permanent);
void psp_ge_blips_begin(void);
void psp_ge_blip(float x, float y, float radius, float red, float green, float blue, unsigned int key);
void psp_ge_blips_end(float center_x, float center_y, float scale, unsigned int sweep_key, unsigned int mask_key);
void psp_ge_text_character(void const *vertices);
void psp_ge_text_end(void);
int psp_ge_text_begin(void const *bitmap, short blend, float offset_x, float offset_y);
static void window_begin_list(const float *position, const float *forward, const float *up,
	float vertical_field_of_view, float z_near, float z_far);
static int frame_end_list(void);
static void frame_end_show(void);
void psp_ge_frame_end(void);
void psp_ge_window_begin(const float *position, const float *forward, const float *up,
	float vertical_field_of_view, float z_near, float z_far);

/* geinflight's buffer size in bytes from the keep words, or 0 when the
buffer isn't geinflight's */
static unsigned int inflight_buffer_bytes(void)
{
	extern int psp_keep_number(const char *prefix, int fallback);
	int kb;

	if (!psp_keep("geinflight") || !psp_keep("gedefer") || psp_keep("mebench") || psp_keep("gemeoverlap"))
		return 0;
	kb = psp_keep_number("inflightkb", DEFER_INFLIGHT_KB);
	if (kb < DEFER_INFLIGHT_KB_MIN)
		kb = DEFER_INFLIGHT_KB_MIN;
	if (kb > DEFER_INFLIGHT_KB_MAX)
		kb = DEFER_INFLIGHT_KB_MAX;
	return (unsigned int)kb * 1024;
}

/* the bytes psp_tex.c keeps at the texture RAM block's front for the
buffer, asked as the block is first taken (before any texture is in it).
"inflightroom": the buffer lives there, off the general heap, and the
texture RAM cache gives up that much instead. Not the default: at 224 KB
it doubled b30's and a30's texture evictions and frames drawn with a
texture missing (inflightkb2, docs/design_inflightkb.md) */
unsigned int psp_ge_inflight_room_bytes(void)
{
	return psp_keep("inflightroom") ? inflight_buffer_bytes() : 0;
}

/* at each frame's start: whether this frame defers (the keep word, asked
once; the buffer taken the first time) */
static void defer_frame_begin(void)
{
	if (defer_asked < 0)
	{
		/* build 118: mebench takes gedefer and gemeon with it */
		me_bench = psp_keep("mebench");
		defer_asked = psp_keep("gedefer") || me_bench;
		if (defer_asked)
		{
			/* geinflight: a whole frame's buffer; not with the overlap's
			halves or mebench's turns, which hand work over within a frame */
			int inflight = psp_keep("geinflight") && !me_bench && !psp_keep("gemeoverlap");
			unsigned int words = DEFER_WORDS;

			if (inflight)
			{
				words = inflight_buffer_bytes() / sizeof(defer_word);
				if (psp_ge_inflight_room_bytes())
				{
					/* the front room, if the block is the volatile
					partition (else NULL: the heap, as below) */
					defer_buffer = psp_tex_inflight_room(&inflight_room_generation);
					inflight_room = defer_buffer != NULL;
				}
			}
			if (!defer_buffer)
				defer_buffer = memalign(64, words * sizeof(defer_word));
			defer_nodes = memalign(64, DEFER_MAX_NODES * NODE_FLOATS * sizeof(float));
			if (!defer_buffer || !defer_nodes)
			{
				if (!inflight_room)
					free(defer_buffer);
				free(defer_nodes);
				defer_buffer = NULL;
				defer_nodes = NULL;
				inflight_room = 0;
				defer_asked = 0;
				platform_log("GE: gedefer: no room for its buffer, off");
			}
			else
			{
				platform_log("GE: gedefer: the GE layer's calls deferred, %u KB of buffer (%s)",
					(unsigned int)(words * sizeof(defer_word) / 1024),
					inflight_room ? "the texture RAM block's front room, off the heap" : "from the heap");
				if (inflight)
				{
					inflight_on = 1;
					defer_limit = words;
					inflight_buffer_words = words;
					inflight_text_old = psp_keep("inflighttextold") != 0;
					platform_log("GE: geinflight: each frame carried out at the next frame's first call, after its ticks%s",
						inflight_text_old ? "; inflighttextold: text drawn only once its frame's start is carried out" : "");
				}
				if (psp_keep("gemesim"))
				{
					defer_me_thread = (unsigned int)sceKernelGetThreadId();
					platform_log("GE: gemesim: carrying out marked as the Media Engine's (thread %08x)", defer_me_thread);
					/* the check's own test: one known system call while
					marked, which the emulator's mestrict.txt must list
					(sceKernelGetSystemTimeLow from here), so a run with no
					other line proves the gates rather than a dead check */
					psp_me_context = defer_me_thread;
					(void)sceKernelGetSystemTimeLow();
					psp_me_context = 0;
				}
				/* geinflight too: its frame is carried out after the next
				ticks, so a query in them reads what is built, as on the ME */
				defer_me_semantics = psp_keep("gemesim") || psp_keep("gemeon") || me_bench || inflight_on;
				pretouch_on = defer_me_semantics && psp_keep("texpretouch");
				pretouch_all = psp_keep("txptall");
				if (pretouch_on)
					platform_log("GE: texpretouch: each recorded draw's textures read in as it is written down, for the replay's binds%s",
						pretouch_all ? " (txptall: every texture a record names)" : "");
				replay_kinds_on = psp_keep("mekinds") != 0;
				if (replay_kinds_on)
					platform_log("GE: mekinds: each record kind timed as it's carried out, the main CPU's and the ME's apart (a line each 300 frames)");
				if (me_bench)
					platform_log("GE: mebench: the GE layer's work on the main CPU, on the ME serially, and overlapped, in turns of %d frames; the frame pacing off",
						ME_BENCH_FRAMES);
				defer_base = defer_buffer;
				/* build 118: the overlap needs the ME's carrying out (gemeon) */
				if (psp_keep("gemeoverlap") && psp_keep("gemeon"))
				{
					me_overlap = 1;
					defer_limit = DEFER_HALF;
					platform_log("GE: gemeoverlap: the Media Engine carries out one half of the buffer (%u KB) while the main CPU writes the other",
						(unsigned int)(DEFER_HALF * sizeof(defer_word) / 1024));
				}
				else if (psp_keep("gemeoverlap"))
					platform_log("GE: gemeoverlap needs gemeon (and gedefer): off");
			}
		}
	}
	defer_on = defer_asked;
	defer_paused = 0;
	/* build 118: mebench's way for this frame (the ME idle here: the frame's
	end carried everything out) */
	if (me_bench > 0 && defer_buffer && !defer_used)
	{
		int way = (int)((me_bench_frame++ / ME_BENCH_FRAMES) % 3);

		me_bench_settled = way == me_bench_way;
		me_bench_way = way;
		defer_on = way > 0;
		defer_me_on = 1;
		me_overlap = way == 2;
		defer_limit = me_overlap ? DEFER_HALF : DEFER_WORDS;
		defer_base = defer_buffer;
		defer_half = 0;
	}
}

/* room for a call of words words of arguments (what is waiting is carried
out first if the buffer is full); NULL if the call is too large to defer,
after carrying out what is waiting, so the call runs as it comes */
/* geinflight: the frame waiting is carried out before the next frame's first
call is written down (the Media Engine version's one wait a frame) */
static void inflight_wait_me(void);

static void inflight_sync(void)
{
	inflight_frames++;
	inflight_words += defer_used;
	if (defer_used > inflight_words_most)
		inflight_words_most = defer_used;
	inflight_part_words += defer_used;
	inflight_whole_words += inflight_part_words;
	if (inflight_part_words > inflight_whole_most)
		inflight_whole_most = inflight_part_words;
	inflight_part_words = 0;
	if (inflight_me_out)
	{
		inflight_wait_me();
		return;
	}
	defer_drain_for("the next frame's first call (geinflight)");
}

static void __attribute__((noinline, cold)) inflight_room_park(void)
{
	inflight_room_parked = 1;
	while (inflight_room_away)
		sceKernelDelayThread(1000);
	inflight_room_parked = 0;
	inflight_room_check(1);
}

static defer_word *defer_take(int kind, unsigned int words)
{
	defer_word *at;

	if (inflight_room_away)
		inflight_room_park();
	if (inflight_pending)
		inflight_sync();
	if (words > DEFER_LARGEST)
	{
		defer_too_large++;
		GE_DEFER_DRAIN();
		return NULL;
	}
	if (defer_used + 1 + words > defer_limit)
	{
		defer_drains_full++;
		if (inflight_on)
			inflight_part_words += defer_used;
		if (me_overlap)
			defer_hand_over(); /* build 118: the half to the ME, the other one next */
		else
			defer_drain();
	}
	if (!defer_used && inflight_room)
		(void)psp_tex_inflight_room(&inflight_room_generation);
	if (chunk_frame && !chunk_stopped && defer_used - chunk_published >= chunk_words)
		chunk_piece();
	at = defer_base + defer_used;
	at->u = (unsigned int)kind | (1 + words) << 8;
	defer_used += 1 + words;
	defer_records++;
	return at + 1;
}

/* the frame still out (geinflight) carried out first, as the record's own
defer_take would: the ME then isn't running while the texture cache is
changed here, and the load counts in the frame the replay binds in */
static void pretouch(unsigned int key, int kind, int resident_only)
{
	if (inflight_room_away)
		inflight_room_park();
	if (inflight_pending)
		inflight_sync();
	psp_tex_pretouch(key, kind, resident_only);
}

/* n words copied, after one saying whether the pointer was NULL: 1 + n words */
static defer_word *defer_copy(defer_word *at, const void *p, int n)
{
	at->u = p != NULL;
	/* zeroed when there is nothing to copy: no stale word from an earlier
	frame reaches the replay, and a record is the same word for word each
	time its inputs are (rso compares and sends records whole) */
	if (p)
		memcpy(at + 1, p, n * sizeof(defer_word));
	else
		memset(at + 1, 0, n * sizeof(defer_word));
	return at + 1 + n;
}

/* and back: the copy's address, or NULL if the pointer was */
static const void *defer_copy_back(const defer_word *at, const defer_word **next, int n)
{
	*next = at + 1 + n;
	return at->u ? (const void *)(at + 1) : NULL;
}

/* ----- the calls written down: 1 if it was, 0 if it must run now */

static int defer_model_begin(const float *node_matrices, int node_matrix_count, float u_scale, float v_scale,
	const float *ambient, int light_count, const float *light0, const float *light1)
{
	int lights = light_count < 0 ? 0 : light_count > 2 ? 2 : light_count;
	int nodes = node_matrices && node_matrix_count > 0 ? node_matrix_count : 0;
	defer_word *at;

	if (nodes > DEFER_MAX_NODES)
	{
		defer_big_models++;
		GE_DEFER_DRAIN();
		defer_paused = 1;
		return 0;
	}
	defer_paused = 0;
	at = defer_take(DEFER_MODEL_BEGIN, 5 + 4 + 7 + 7 + nodes * NODE_FLOATS);
	at[0].p = node_matrices;
	at[1].i = node_matrix_count;
	at[2].f = u_scale;
	at[3].f = v_scale;
	at[4].i = light_count;
	at = defer_copy(at + 5, ambient, 3);
	at = defer_copy(at, lights > 0 ? light0 : NULL, 6);
	at = defer_copy(at, lights > 1 ? light1 : NULL, 6);
	if (nodes)
		memcpy(at, node_matrices, nodes * NODE_FLOATS * sizeof(float));
	return 1;
}

static int defer_model_shape(int first_person, const float *centroid, float radius, float near_fp, float far_fp)
{
	defer_word *at = defer_take(DEFER_MODEL_SHAPE, 4 + 4);

	at[0].i = first_person;
	at[1].f = radius;
	at[2].f = near_fp;
	at[3].f = far_fp;
	defer_copy(at + 4, centroid, 3);
	return 1;
}

static int defer_model_effect(int type, float intensity)
{
	defer_word *at = defer_take(DEFER_MODEL_EFFECT, 2);

	at[0].i = type;
	/* the replay reads the intensity for camouflage only: a still object's
	effect is the engine's "none", whose intensity is left unset, so it is
	written as 0 and the record is the same each time its inputs are (rso) */
	at[1].f = (type & 0xFF) == 1 ? intensity : 0.0f;
	return 1;
}

static int defer_model_point_light(const float *position, unsigned int color)
{
	defer_word *at = defer_take(DEFER_MODEL_POINT_LIGHT, 1 + 4);

	at[0].u = color;
	defer_copy(at + 1, position, 3);
	return 1;
}

static int defer_model_multipurpose(unsigned int multipurpose_key, unsigned int glow_key, float const *change,
	float const *glow)
{
	defer_word *at = defer_take(DEFER_MODEL_MULTIPURPOSE, 2 + 4 + 4);

	at[0].u = multipurpose_key;
	at[1].u = glow_key;
	defer_copy(defer_copy(at + 2, change, 3), glow, 3);
	return 1;
}

static int defer_model_reflection(unsigned int cube_key, float const *rgb)
{
	/* the key, then the copy's flag and its 4 floats: the tint and which of
	Chief's parts it is */
	defer_word *at = defer_take(DEFER_MODEL_REFLECTION, 1 + 1 + 4);

	at[0].u = cube_key;
	defer_copy(at + 1, rgb, 4);
	return 1;
}

static int defer_model_detail(unsigned int detail_key, float u_scale, float v_scale)
{
	defer_word *at = defer_take(DEFER_MODEL_DETAIL, 3);

	at[0].u = detail_key;
	at[1].f = u_scale;
	at[2].f = v_scale;
	return 1;
}

static int defer_draw_model(void *vertices, int vertex_count, int uncompressed, const unsigned short *indices,
	long index_count, int strip, unsigned int color, unsigned int base_key, float u_scale, float v_scale,
	int alpha_tested)
{
	defer_word *at = defer_take(DEFER_DRAW_MODEL, 11);

	at[0].p = vertices;
	at[1].i = vertex_count;
	at[2].i = uncompressed;
	at[3].p = indices;
	at[4].i = (int)index_count;
	at[5].i = strip;
	at[6].u = color;
	at[7].u = base_key;
	at[8].f = u_scale;
	at[9].f = v_scale;
	at[10].i = alpha_tested;
	return 1;
}

static int defer_queue_transparent(void *vertices, int vertex_count, int uncompressed, const unsigned short *indices,
	long index_count, int strip, unsigned int key, float u_scale, float v_scale, float u_offset, float v_offset,
	short blend, const float *centroid)
{
	defer_word *at = defer_take(DEFER_QUEUE_TRANSPARENT, 12 + 4);

	at[0].p = vertices;
	at[1].i = vertex_count;
	at[2].i = uncompressed;
	at[3].p = indices;
	at[4].i = (int)index_count;
	at[5].i = strip;
	at[6].u = key;
	at[7].f = u_scale;
	at[8].f = v_scale;
	at[9].f = u_offset;
	at[10].f = v_offset;
	at[11].i = blend;
	defer_copy(at + 12, centroid, 3);
	return 1;
}

static int defer_fog(const float *color, float minimum_distance, float maximum_distance, float density)
{
	defer_word *at = defer_take(DEFER_FOG, 3 + 4);

	at[0].f = minimum_distance;
	at[1].f = maximum_distance;
	at[2].f = density;
	defer_copy(at + 3, color, 3);
	return 1;
}

/* a call of up to eight plain words (defer_bits for a float, DEFER_P
for a pointer: both are a word on the PSP) */
static int defer_words(int kind, int count, unsigned int a, unsigned int b, unsigned int c, unsigned int d,
	unsigned int e, unsigned int f, unsigned int g, unsigned int h)
{
	defer_word *at = defer_take(kind, count);
	unsigned int all[8] = { a, b, c, d, e, f, g, h };
	int k;

	for (k = 0; k < count; k++)
		at[k].u = all[k];
	return 1;
}
#define DEFER_P(pointer) ((unsigned int)(unsigned long)(pointer))

static unsigned int defer_bits(float value)
{
	defer_word w;

	w.f = value;
	return w.u;
}

static int defer_draw_level(const void *vertices, const void *lightmap_vertices, int vertex_count,
	const unsigned short *indices, long triangle_count, unsigned int color,
	unsigned int base_key, unsigned int lightmap_key, int alpha_tested)
{
	defer_word *at = defer_take(DEFER_DRAW_LEVEL, 9);

	at[0].p = vertices;
	at[1].p = lightmap_vertices;
	at[2].i = vertex_count;
	at[3].p = indices;
	at[4].i = (int)triangle_count;
	at[5].u = color;
	at[6].u = base_key;
	at[7].u = lightmap_key;
	at[8].i = alpha_tested;
	return 1;
}

static int defer_sprites(void const *vertices, int sprite_count, unsigned int bitmap_key, short blend,
	float const *view_to_world)
{
	int corners = vertices && sprite_count > 0 ? 4 * sprite_count : 0;
	defer_word *at = defer_take(DEFER_SPRITES, 3 + 1 + NODE_FLOATS + 1 + corners * SPRITE_VERTEX_WORDS);

	if (!at)
		return 0;
	at[0].i = sprite_count;
	at[1].u = bitmap_key;
	at[2].i = blend;
	at = defer_copy(at + 3, view_to_world, NODE_FLOATS);
	defer_copy(at, corners ? vertices : NULL, corners * SPRITE_VERTEX_WORDS);
	return 1;
}

/* build 113: a contrail's corners (render_contrails.c's contrail_vertex has
the sprite vertex's layout: x, y, z, u, v, argb) */
static int defer_contrail(void const *vertices, int vertex_count, unsigned int bitmap_key, short blend)
{
	int words = vertices && vertex_count > 0 ? vertex_count * SPRITE_VERTEX_WORDS : 0;
	defer_word *at = defer_take(DEFER_CONTRAIL, 3 + 1 + words);

	if (!at)
		return 0;
	at[0].i = vertex_count;
	at[1].u = bitmap_key;
	at[2].i = blend;
	defer_copy(at + 3, words ? vertices : NULL, words);
	return 1;
}

static int defer_widget_sprite(float const *point, float radius, float scale_x, float scale_y, float rotation,
	unsigned long color)
{
	defer_word *at = defer_take(DEFER_WIDGET_SPRITE, 5 + 4);

	at[0].f = radius;
	at[1].f = scale_x;
	at[2].f = scale_y;
	at[3].f = rotation;
	at[4].u = (unsigned int)color;
	defer_copy(at + 5, point, 3);
	return 1;
}

static int defer_queue_level_transparent(const void *vertices, int vertex_count, const unsigned short *indices,
	long triangle_count, unsigned int key, float u_scale, float v_scale, float u_offset, float v_offset, short blend,
	const float *centroid)
{
	/* under ME semantics, each triangle's wall answer goes with the call
	(worked out here: heap for the answers, the engine's collision rays);
	a word saying how many answers, then the answers, a byte each */
	long answers = defer_me_semantics && triangle_count > 0 ? triangle_count : 0;
	defer_word *at = defer_take(DEFER_QUEUE_LEVEL_TRANSPARENT, 10 + 4 + 1 + (answers + 3) / 4);

	if (!at)
		return 0;
	at[14].i = answers && overlay_answers_for(vertices, vertex_count, indices, triangle_count,
		(unsigned char *)(at + 15)) ? (int)answers : 0;

	at[0].p = vertices;
	at[1].i = vertex_count;
	at[2].p = indices;
	at[3].i = (int)triangle_count;
	at[4].u = key;
	at[5].f = u_scale;
	at[6].f = v_scale;
	at[7].f = u_offset;
	at[8].f = v_offset;
	at[9].i = blend;
	defer_copy(at + 10, centroid, 3);
	return 1;
}

static int defer_screen_quad(void const *vertices, unsigned int key, short blend, float red, float green, float blue,
	float fade, float offset_x, float offset_y, void const *meter_parameters, int wrapped, float u_scale, float v_scale)
{
	defer_word *at = defer_take(DEFER_SCREEN_QUAD, 11 + 1 + 4 * SCREEN_VERTEX_WORDS + 1 + METER_WORDS);

	at[0].u = key;
	at[1].i = blend;
	at[2].f = red;
	at[3].f = green;
	at[4].f = blue;
	at[5].f = fade;
	at[6].f = offset_x;
	at[7].f = offset_y;
	at[8].i = wrapped;
	at[9].f = u_scale;
	at[10].f = v_scale;
	at = defer_copy(at + 11, vertices, 4 * SCREEN_VERTEX_WORDS);
	defer_copy(at, meter_parameters, METER_WORDS);
	return 1;
}

static int defer_shadow_begin(const float *matrix, const float *color, float radius)
{
	defer_word *at = defer_take(DEFER_SHADOW_BEGIN, 1 + 1 + NODE_FLOATS + 4);

	at[0].f = radius;
	defer_copy(defer_copy(at + 1, matrix, NODE_FLOATS), color, 3);
	return 1;
}

static int defer_light_begin(const float *position, const float *forward, const float *color, float radius,
	float cosine_falloff, float cosine_cutoff)
{
	defer_word *at = defer_take(DEFER_LIGHT_BEGIN, 3 + 4 + 4 + 4);

	at[0].f = radius;
	at[1].f = cosine_falloff;
	at[2].f = cosine_cutoff;
	defer_copy(defer_copy(defer_copy(at + 3, position, 3), forward, 3), color, 3);
	return 1;
}

static int defer_light_gel(const float *gel)
{
	defer_copy(defer_take(DEFER_LIGHT_GEL, 1 + LIGHT_GEL_FLOATS), gel, LIGHT_GEL_FLOATS);
	return 1;
}

static int defer_screen_flash(short type, float intensity, float const *argb)
{
	defer_word *at = defer_take(DEFER_SCREEN_FLASH, 2 + 5);

	at[0].i = type;
	at[1].f = intensity;
	defer_copy(at + 2, argb, 4);
	return 1;
}

static int defer_decal(void const *vertices, int quad_count, unsigned int key, short blend, unsigned long color,
	int intensity, int alpha_tested, int permanent)
{
	/* geinflight: the decals' vertices are rewritten by the next frame's
	ticks, before the frame is carried out, so they're copied (16 bytes a
	corner, psp_ge_decal_body; at most 256 quads drawn) */
	int copied = inflight_on && vertices && quad_count > 0 && quad_count <= 256 ? quad_count * 4 * 4 : 0;
	defer_word *at = defer_take(DEFER_DECAL, 8 + copied);

	if (!at)
		return 0; /* too large: carried out as it comes, after what waits */
	if (copied)
		memcpy(at + 8, vertices, copied * sizeof(defer_word));

	at[0].p = vertices;
	at[1].i = quad_count;
	at[2].u = key;
	at[3].i = blend;
	at[4].u = (unsigned int)color;
	at[5].i = intensity;
	at[6].i = alpha_tested;
	at[7].i = permanent;
	return 1;
}

static int defer_window_begin(const float *position, const float *forward, const float *up,
	float vertical_field_of_view, float z_near, float z_far)
{
	defer_word *at = defer_take(DEFER_WINDOW_BEGIN, 3 + 3 * (1 + 3));

	at[0].f = vertical_field_of_view;
	at[1].f = z_near;
	at[2].f = z_far;
	defer_copy(defer_copy(defer_copy(at + 3, position, 3), forward, 3), up, 3);
	return 1;
}

static int defer_text_character(void const *vertices)
{
	defer_word *at = defer_take(DEFER_TEXT_CHARACTER, 1 + 4 * SCREEN_VERTEX_WORDS);

	defer_copy(at, vertices, 4 * SCREEN_VERTEX_WORDS);
	return 1;
}

/* ----- a run of records carried out in order: on the main CPU, or as the
Media Engine's job (psp_ge_me_replay) */
/* the record being carried out, for the heap's report of a call in the Media
Engine's work (psp_libc.c): which kind of call led there */
int psp_me_record_kind = 0;

/* geinflight's frame on the ME: audio and the ring served now (both look at
one uncached word each when there's nothing to do). psp_me_yield serves both,
and the ring takes pushes during any GE job unless "meyieldoff" */
extern void psp_me_serve_audio(void); /* psp_me.c (build 113) */

static void inflight_serve(void)
{
	if (!psp_on_me())
		return;
	psp_me_yield();
}

/* "mekinds" (recon): each record kind's time as it's carried out, kept apart
for each CPU that carried it out. On the ME the frame's job is otherwise one
number (ge_clock reads nothing there), and the ME takes 2-3x the main CPU's
time for the same work: an A/B of the two sides by kind says where. The ME
reads its cycle counter (one instruction); the main CPU the system clock,
once a record. Simulated ME work (gemesim) isn't timed: the clock is a system
call the ME can't make. Set at launch with the replay's other words; off, one
test a run */
#define REPLAY_SERVED (DEFER_WINDOW_BEGIN + 1) /* audio and the ring served between records */
#define REPLAY_KINDS (REPLAY_SERVED + 1)
struct replay_kind_times
{
	unsigned long long ticks[2][REPLAY_KINDS]; /* [0] main CPU, us; [1] ME, cycles */
	unsigned int records[2][REPLAY_KINDS];
	unsigned int runs[2];
} __attribute__((aligned(64))); /* a line of its own: the ME writes it while the main CPU runs on */
static struct replay_kind_times replay_kinds;
static const char *const replay_kind_names[REPLAY_KINDS] =
{
	[0] = "unknown", [DEFER_MODEL_BEGIN] = "model begin", [DEFER_MODEL_SHAPE] = "model shape",
	[DEFER_MODEL_EFFECT] = "model effect", [DEFER_MODEL_MULTIPURPOSE] = "model multipurpose",
	[DEFER_DRAW_MODEL] = "model part draws", [DEFER_QUEUE_TRANSPARENT] = "see-through queued",
	[DEFER_FOG] = "fog", [DEFER_WINDOW_TARGET] = "window target", [DEFER_LEVEL_DETAIL] = "level detail",
	[DEFER_DRAW_LEVEL] = "level draws", [DEFER_SKY] = "sky", [DEFER_SPRITES] = "sprites",
	[DEFER_WIDGET_BEGIN] = "widget begin", [DEFER_WIDGET_TEXTURE] = "widget texture",
	[DEFER_WIDGET_OCCLUSION] = "widget occlusion", [DEFER_WIDGET_SPRITE] = "widget sprites",
	[DEFER_QUEUE_LEVEL_TRANSPARENT] = "see-through level queued", [DEFER_HUD_ZOOM] = "hud zoom",
	[DEFER_UI_CENTERED] = "ui centred", [DEFER_UI_SPACE] = "ui space", [DEFER_SCREEN_QUAD] = "screen quads",
	[DEFER_SHADOW_BEGIN] = "shadow begin", [DEFER_SHADOW_DRAW] = "shadow draws", [DEFER_SHADOW_END] = "shadow end",
	[DEFER_LIGHT_BEGIN] = "light begin", [DEFER_LIGHT_DRAW] = "light draws", [DEFER_SCREEN_FLASH] = "screen flash",
	[DEFER_DECAL] = "decals", [DEFER_BLIPS_BEGIN] = "blips begin", [DEFER_BLIP] = "blips", [DEFER_BLIPS_END] = "blips end",
	[DEFER_TEXT_CHARACTER] = "text characters", [DEFER_TEXT_END] = "text end", [DEFER_TEXT_BEGIN] = "text begin",
	[DEFER_CONTRAIL] = "contrails", [DEFER_ZOOM_MASK] = "zoom mask", [DEFER_FOG_PLANAR] = "planar fog",
	[DEFER_SEETHROUGH_TINT] = "see-through tint", [DEFER_SEETHROUGH_METER] = "see-through meters",
	[DEFER_MODEL_POINT_LIGHT] = "model point lights", [DEFER_MODEL_REFLECTION] = "model reflection",
	[DEFER_LIGHT_GEL] = "light gel faces",
	[DEFER_LEVEL_GLOW] = "level glow",
	[DEFER_FRAME_END] = "frame end (list's part)",
	[DEFER_WINDOW_BEGIN] = "window begin (list's part)", [REPLAY_SERVED] = "audio and ring served",
};

static inline unsigned int replay_clock(int me)
{
	unsigned int count;

	if (!me)
		return sceKernelGetSystemTimeLow();
	__asm__ __volatile__("mfc0 %0, $9" : "=r"(count));
	return count;
}

/* the time since *mark to kind, on side's clock */
static inline void replay_kinds_add(int side, int kind, unsigned int *mark)
{
	unsigned int now = replay_clock(side);

	replay_kinds.ticks[side][kind] += now - *mark;
	*mark = now;
}

static void defer_replay(const defer_word *buffer, unsigned int used, int on_me)
{
	unsigned int at = 0, records = 0;
	/* mekinds: timed only where the clock can be read (not gemesim's
	simulated ME work) */
	int side = psp_on_me(), timed = replay_kinds_on && (side || !psp_me_context), kind_before = -1;
	unsigned int mark = timed ? replay_clock(side) : 0;

	if (timed)
		replay_kinds.runs[side]++;

	while (at < used)
	{
		const defer_word *w = buffer + at + 1, *rest;
		int kind = buffer[at].u & 0xFF;
		const void *a, *b, *c;

		/* build 113: on the ME, an audio block asked for meanwhile is mixed
		between records rather than after the whole run (psp_me.c).
		geinflight: the frame's job lasts a frame while the main CPU ticks,
		which waits on both audio and the ring's jobs (perception's rays, the
		poses), so they're served every record */
		if (timed && kind_before >= 0)
			replay_kinds_add(side, kind_before, &mark);
		if (replay_inflight)
			inflight_serve();
		else if (on_me && (++records & 31) == 0)
			psp_me_serve_audio();
		if (timed && side)
			replay_kinds_add(side, REPLAY_SERVED, &mark);

		at += buffer[at].u >> 8;
		psp_me_record_kind = kind;
		if (timed)
		{
			kind_before = kind < REPLAY_KINDS - 1 ? kind : 0;
			replay_kinds.records[side][kind_before]++;
		}
		switch (kind)
		{
		case DEFER_MODEL_BEGIN:
		{
			int count = w[1].i, nodes = w[0].p && count > 0 ? count : 0;

			a = defer_copy_back(w + 5, &rest, 3);
			b = defer_copy_back(rest, &rest, 6);
			c = defer_copy_back(rest, &rest, 6);
			if (nodes)
				memcpy(defer_nodes, rest, nodes * NODE_FLOATS * sizeof(float));
			psp_ge_model_begin(w[0].p ? defer_nodes : NULL, count, w[2].f, w[3].f, a, w[4].i, b, c);
			break;
		}
		case DEFER_MODEL_SHAPE:
			psp_ge_model_shape(w[0].i, defer_copy_back(w + 4, &rest, 3), w[1].f, w[2].f, w[3].f);
			break;
		case DEFER_MODEL_EFFECT:
			psp_ge_model_effect(w[0].i, w[1].f);
			break;
		case DEFER_MODEL_POINT_LIGHT:
			psp_ge_model_point_light(defer_copy_back(w + 1, &rest, 3), w[0].u);
			break;
		case DEFER_MODEL_MULTIPURPOSE:
			a = defer_copy_back(w + 2, &rest, 3);
			psp_ge_model_multipurpose(w[0].u, w[1].u, a, defer_copy_back(rest, &rest, 3));
			break;
		case DEFER_MODEL_DETAIL:
			psp_ge_model_detail(w[0].u, w[1].f, w[2].f);
			break;
		case DEFER_MODEL_REFLECTION:
			psp_ge_model_reflection(w[0].u, defer_copy_back(w + 1, &rest, 4));
			break;
		case DEFER_DRAW_MODEL:
			psp_ge_draw_model((void *)w[0].p, w[1].i, w[2].i, (const unsigned short *)w[3].p, w[4].i, w[5].i,
				w[6].u, w[7].u, w[8].f, w[9].f, w[10].i);
			break;
		case DEFER_QUEUE_TRANSPARENT:
			psp_ge_queue_transparent((void *)w[0].p, w[1].i, w[2].i, (const unsigned short *)w[3].p, w[4].i, w[5].i,
				w[6].u, w[7].f, w[8].f, w[9].f, w[10].f, (short)w[11].i, defer_copy_back(w + 12, &rest, 3));
			break;
		case DEFER_FOG:
			psp_ge_fog(defer_copy_back(w + 3, &rest, 3), w[0].f, w[1].f, w[2].f);
			break;
		case DEFER_FOG_PLANAR: /* build 116 */
			psp_ge_fog_planar(w[0].f, w[1].f, w[2].f, w[3].f, w[4].f);
			break;
		case DEFER_FOG_SUBMERGED:
			psp_ge_fog_submerged(w[0].f, w[1].f, w[2].f, w[3].f, w[4].u, w[5].f, w[6].f, w[7].f);
			break;
		case DEFER_SEETHROUGH_TINT: /* build 116 */
			seethrough_tint_set(w[0].u, (int)w[1].u, w[2].u, w[3].u, w[4].u);
			break;
		case DEFER_SEETHROUGH_METER: /* build 116 */
			psp_ge_seethrough_meter(w[0].u ? (const void *)(w + 1) : NULL);
			break;
		case DEFER_WINDOW_TARGET:
			psp_ge_window_target(w[0].i);
			break;
		case DEFER_LEVEL_DETAIL:
			psp_ge_level_detail(w[0].u, w[1].f, w[2].f, w[3].u, w[4].f, w[5].f);
			break;
		case DEFER_LEVEL_GLOW:
			level_glow_set(w[0].u, w[1].u, w[2].u, w[3].u);
			break;
		case DEFER_DRAW_LEVEL:
			psp_ge_draw_level(w[0].p, w[1].p, w[2].i, (const unsigned short *)w[3].p, w[4].i, w[5].u, w[6].u, w[7].u,
				w[8].i);
			break;
		case DEFER_SKY:
			psp_ge_sky(w[0].i);
			break;
		case DEFER_SPRITES:
			a = defer_copy_back(w + 3, &rest, NODE_FLOATS);
			psp_ge_sprites(defer_copy_back(rest, &rest, 0), w[0].i, w[1].u, (short)w[2].i, a);
			break;
		case DEFER_CONTRAIL: /* build 113 */
			psp_ge_contrail(defer_copy_back(w + 3, &rest, 0), w[0].i, w[1].u, (short)w[2].i);
			break;
		case DEFER_ZOOM_MASK: /* build 113 */
			psp_ge_zoom_mask(w[0].u, w[1].u);
			break;
		case DEFER_WIDGET_BEGIN:
			psp_ge_widget_begin(w[0].i);
			break;
		case DEFER_WIDGET_TEXTURE:
			psp_ge_widget_texture(w[0].u);
			break;
		case DEFER_WIDGET_OCCLUSION:
			psp_ge_widget_occlusion(w[0].i);
			break;
		case DEFER_WIDGET_SPRITE:
			psp_ge_widget_sprite(defer_copy_back(w + 5, &rest, 3), w[0].f, w[1].f, w[2].f, w[3].f, w[4].u);
			break;
		case DEFER_QUEUE_LEVEL_TRANSPARENT:
			overlay_answers = w[14].i ? (const unsigned char *)(w + 15) : NULL;
			psp_ge_queue_level_transparent(w[0].p, w[1].i, (const unsigned short *)w[2].p, w[3].i, w[4].u, w[5].f,
				w[6].f, w[7].f, w[8].f, (short)w[9].i, defer_copy_back(w + 10, &rest, 3));
			overlay_answers = NULL;
			break;
		case DEFER_HUD_ZOOM:
			psp_ge_hud_zoom(w[0].f);
			break;
		case DEFER_UI_CENTERED:
			psp_ge_ui_centered(w[0].i);
			break;
		case DEFER_UI_SPACE:
			psp_ge_ui_space(w[0].i, w[1].i);
			break;
		case DEFER_SCREEN_QUAD:
			a = defer_copy_back(w + 11, &rest, 4 * SCREEN_VERTEX_WORDS);
			psp_ge_screen_quad(a, w[0].u, (short)w[1].i, w[2].f, w[3].f, w[4].f, w[5].f, w[6].f, w[7].f,
				defer_copy_back(rest, &rest, METER_WORDS), w[8].i, w[9].f, w[10].f);
			break;
		case DEFER_SHADOW_BEGIN:
			a = defer_copy_back(w + 1, &rest, NODE_FLOATS);
			psp_ge_shadow_begin(a, defer_copy_back(rest, &rest, 3), w[0].f);
			break;
		case DEFER_SHADOW_DRAW:
			psp_ge_shadow_draw(w[0].p, w[1].i, (const unsigned short *)w[2].p, w[3].i);
			break;
		case DEFER_SHADOW_END:
			psp_ge_shadow_end();
			break;
		case DEFER_LIGHT_BEGIN:
			a = defer_copy_back(w + 3, &rest, 3);
			b = defer_copy_back(rest, &rest, 3);
			psp_ge_light_begin(a, b, defer_copy_back(rest, &rest, 3), w[0].f, w[1].f, w[2].f);
			break;
		case DEFER_LIGHT_GEL:
			psp_ge_light_gel(defer_copy_back(w, &rest, LIGHT_GEL_FLOATS));
			break;
		case DEFER_LIGHT_DRAW:
			psp_ge_light_draw(w[0].p, w[1].i, (const unsigned short *)w[2].p, w[3].i, w[4].u);
			break;
		case DEFER_SCREEN_FLASH:
			psp_ge_screen_flash((short)w[0].i, w[1].f, defer_copy_back(w + 2, &rest, 4));
			break;
		case DEFER_FRAME_END:
			inflight_list_ended = frame_end_list();
			break;
		case DEFER_WINDOW_BEGIN:
			a = defer_copy_back(w + 3, &rest, 3);
			b = defer_copy_back(rest, &rest, 3);
			c = defer_copy_back(rest, &rest, 3);
			window_begin_list(a, b, c, w[0].f, w[1].f, w[2].f);
			break;
		case DEFER_DECAL:
			/* the record's own copy of the vertices when it has one (geinflight) */
			psp_ge_decal((w[-1].u >> 8) - 1 > 8 ? (const void *)(w + 8) : w[0].p, w[1].i, w[2].u, (short)w[3].i, w[4].u, w[5].i,
				w[6].i, w[7].i);
			break;
		case DEFER_BLIPS_BEGIN:
			psp_ge_blips_begin();
			break;
		case DEFER_BLIP:
			psp_ge_blip(w[0].f, w[1].f, w[2].f, w[3].f, w[4].f, w[5].f, w[6].u);
			break;
		case DEFER_BLIPS_END:
			psp_ge_blips_end(w[0].f, w[1].f, w[2].f, w[3].u, w[4].u);
			break;
		case DEFER_TEXT_CHARACTER:
			psp_ge_text_character(defer_copy_back(w, &rest, 4 * SCREEN_VERTEX_WORDS));
			break;
		case DEFER_TEXT_END:
			psp_ge_text_end();
			break;
		case DEFER_TEXT_BEGIN:
			psp_ge_text_begin(w[0].p, (short)w[1].i, w[2].f, w[3].f);
			break;
		}
	}
	if (timed && kind_before >= 0)
		replay_kinds_add(side, kind_before, &mark);
}

/* mekinds: the frames since the last report by kind, the largest first, each
CPU apart, on as many lines as the log's limit needs. Only while no frame is
out with the ME, which writes the counts */
static void replay_kinds_report(unsigned int frames)
{
	extern float psp_me_cycles_per_us(void);
	int side;

	if (!replay_kinds_on || inflight_me_out || !frames)
		return;
	for (side = 0; side < 2; side++)
	{
		const char *who = side ? "the ME" : "the main CPU";
		float rate = side ? psp_me_cycles_per_us() : 1.0f;
		unsigned long long all = 0;
		unsigned char shown[REPLAY_KINDS];
		char line[480], entry[120]; /* under the log's 512 */
		int length, k;

		if (!replay_kinds.runs[side])
			continue;
		if (rate <= 0.0f)
		{
			platform_log("GE: mekinds: the ME's times can't be given (its clock wasn't measured at start)");
			continue;
		}
		for (k = 0; k < REPLAY_KINDS; k++)
			all += replay_kinds.ticks[side][k];
		length = snprintf(line, sizeof(line), "GE: mekinds, %s, us a frame over %u frames (%.2f runs a frame): all %.0f;",
			who, frames, (float)replay_kinds.runs[side] / frames, (float)all / rate / frames);
		memset(shown, 0, sizeof(shown));
		for (;;)
		{
			int best = -1, size;
			float us;

			for (k = 0; k < REPLAY_KINDS; k++)
				if (!shown[k] && replay_kinds.ticks[side][k] &&
					(best < 0 || replay_kinds.ticks[side][k] > replay_kinds.ticks[side][best]))
					best = k;
			if (best < 0)
				break;
			shown[best] = 1;
			us = (float)replay_kinds.ticks[side][best] / rate;
			if (us / frames < 1.0f)
				break; /* the rest are under a microsecond a frame */
			size = replay_kinds.records[side][best] ?
				snprintf(entry, sizeof(entry), " %s %.0f (%.1f a frame, %.1f us each);", replay_kind_names[best] ? replay_kind_names[best] : "?",
					us / frames, (float)replay_kinds.records[side][best] / frames, us / replay_kinds.records[side][best]) :
				snprintf(entry, sizeof(entry), " %s %.0f;", replay_kind_names[best], us / frames);
			if (size >= (int)sizeof(entry))
				size = sizeof(entry) - 1;
			if (length + size >= (int)sizeof(line))
			{
				platform_log("%s", line);
				length = snprintf(line, sizeof(line), "GE: mekinds, %s (more):", who);
			}
			memcpy(line + length, entry, size + 1);
			length += size;
		}
		platform_log("%s", line);
	}
	memset(&replay_kinds, 0, sizeof(replay_kinds));
}

/* build 112: "gemeon" (with gedefer): the run carried out as the Media
Engine's job (psp_me.c, ME_JOB_GE), L1 step 4's first form: the main CPU
waits for it, so it measures the ME on this work and proves it runs there;
overlapping comes next. args: the records, their words, the value
psp_me_context holds meanwhile (the game thread's id under gemesim, so the
emulator's ME-strict check also covers this path when it simulates the job) */
unsigned int psp_ge_me_replay(const unsigned int *args, unsigned int *results)
{
	/* build 118: on the ME itself nothing marks the work (PSP_ME_WORK sees
	its stack): psp_me_context is the main CPU's word, which a write here
	would share a cache line with while the main CPU runs on (gemeoverlap).
	Simulated (args[3] 0), the main CPU runs it: marked as before */
	if (!args[3])
		psp_me_context = args[2] ? args[2] : 1;
	replay_inflight = args[4] >= 2;
	if (args[4] == 3 && args[3])
	{
		/* mechunk: what the main CPU has published, as it publishes it,
		until the final mark; the cache dropped before each piece (what it
		read of the line the last piece ended in may be older than the
		records now there). Audio and the ring served while it waits */
		extern void psp_me_cache_refresh(void);
		extern int psp_me_stop_asked(void);
		unsigned int at = 0, looks = 0;

		for (;;)
		{
			/* the final mark first: the main CPU writes the end before it,
			so an end read after a final mark is the last */
			unsigned int final = CHUNK_LINE[1], end = CHUNK_LINE[0];

			if (end > at)
			{
				psp_me_cache_refresh();
				defer_replay((const defer_word *)args[0] + at, end - at, 1);
				at = end;
				CHUNK_LINE[2] = at;
				continue;
			}
			if (final || psp_me_stop_asked())
				break;
			looks++;
			inflight_serve();
		}
		CHUNK_LINE[3] = looks;
		results[0] = at;
	}
	else
	{
		/* mechunk simulated (the emulator): the published piece, run here */
		defer_replay((const defer_word *)args[0] + (args[4] == 3 ? args[5] : 0), args[1], (int)args[3]);
		results[0] = args[1];
	}
	replay_inflight = 0;
	/* build 118: gemeoverlap (args[4]): the newest stall address the run
	kept goes back in the slot (uncached), for the main CPU to pass on, and
	is forgotten here: only the ME writes stall_me_address meanwhile */
	if (args[4])
	{
		results[1] = (unsigned int)stall_me_address;
		results[2] = (unsigned int)stall_me_qid;
		stall_me_address = NULL;
	}
	/* (last: everything above is the ME's work, also when simulated) */
	if (!args[3])
		psp_me_context = 0;
	return 0;
}

static PSP_MAIN_SIDE unsigned long defer_me_jobs = 0, defer_me_failed = 0, defer_me_wait_us = 0, defer_me_own_us = 0;

/* 1 if the ME carried the run out */
static int defer_on_me(void)
{
	extern int psp_me_running(void);
	extern int psp_me_run(int which, unsigned int type, const unsigned int *args, int arg_count, unsigned int *results,
		unsigned int first_look_us);
	extern unsigned int psp_me_last_job_us(int which);
	/* all six words: the slot keeps a word not given from the job before,
	and a mode word (args[4]) left from a frame in flight or in pieces would
	be read as this job's */
	unsigned int args[6] = { 0 }, results[8], start;

	if (defer_me_on < 0)
		defer_me_on = psp_keep("gemeon");
	if (!defer_me_on || !psp_me_running())
		return 0;
	/* everything the job reads to RAM: the records, what they point to that
	the engine wrote, and this file's state as the main CPU left it */
	sceKernelDcacheWritebackAll();
	args[0] = (unsigned int)defer_buffer;
	args[1] = defer_used;
	args[2] = defer_me_thread;
	start = sceKernelGetSystemTimeLow();
	if (!psp_me_run(1 /* the render slot */, 4 /* ME_JOB_GE */, args, 6, results, 20))
	{
		defer_me_failed++;
		return 0;
	}
	defer_me_wait_us += sceKernelGetSystemTimeLow() - start;
	defer_me_own_us += psp_me_last_job_us(1);
	defer_me_jobs++;
	/* and back: what the ME wrote (this file's state, pspgu's, the list)
	read fresh from RAM */
	sceKernelDcacheWritebackInvalidateAll();
	return 1;
}

/* ----- build 118: gemeoverlap's two halves */

/* the half out with the ME, waited for, and the GE given what it wrote */
static void defer_wait_me(void)
{
	extern unsigned int psp_me_last_job_us(int which);
	unsigned int results[ME_JOB_RESULTS] = { 0 }, start;

	if (!me_out)
		return;
	me_out = 0;
	start = sceKernelGetSystemTimeLow();
	gebusy_wait(1, 0);
	if (!psp_me_finish(ME_SLOT_RENDER, results))
	{
		/* the ME stopped (HOME, sleep, a job past 500 ms) with the half
		perhaps half carried out: the main CPU goes on alone from here,
		and this frame may draw wrong */
		gebusy_wait(0, 0);
		psp_me_window[0] = 0;
		defer_me_failed++;
		me_overlap = 0;
		/* the half being written moves to the buffer's start, where the
		calls are written from now on */
		if (defer_base != defer_buffer)
			memmove(defer_buffer, defer_base, defer_used * sizeof(defer_word));
		defer_base = defer_buffer;
		defer_half = 0;
		defer_limit = DEFER_WORDS;
		platform_log("GE: gemeoverlap: the Media Engine failed with half the buffer out; the main CPU carries the calls out from here");
		return;
	}
	psp_me_window[0] = 0;
	me_overlap_waits++;
	gebusy_wait(0, 0);
	me_overlap_wait_us += sceKernelGetSystemTimeLow() - start;
	defer_me_own_us += psp_me_last_job_us(ME_SLOT_RENDER);
	defer_me_jobs++;
	if (results[1])
	{
		stall_forwarded++;
		gebusy_me_handed(results[1]);
		ge_stall_pass((int)results[2], (void *)results[1]);
	}
}

/* the half being written handed to the ME, and the other one written next;
carried out here if the ME can't take it */
static void defer_hand_over(void)
{
	unsigned int args[5];

	defer_wait_me(); /* one half out at a time: the other is free after this */
	if (!defer_used)
		return;
	if (me_overlap && psp_me_running())
	{
		/* the records, what they point to that the engine wrote, and this
		file's state as the main CPU left it, to RAM for the ME (which drops
		its own cache as the job starts) */
		sceKernelDcacheWritebackAll();
		args[0] = (unsigned int)defer_base;
		args[1] = defer_used;
		args[2] = defer_me_thread;
		args[3] = 0; /* (psp_me.c sets it: on the ME or not) */
		args[4] = 1;
		psp_me_window[0] = 1;
		/* simulated, the job runs inside psp_me_submit, on this CPU */
		defer_replaying = 1;
		if (psp_me_submit(ME_SLOT_RENDER, ME_JOB_GE, args, 5))
		{
			defer_replaying = 0;
			me_out = 1;
			me_wrote = 1;
			me_overlap_runs++;
			defer_half ^= 1;
			defer_base = defer_buffer + defer_half * DEFER_HALF;
			defer_used = 0;
			defer_resets++;
			return;
		}
		defer_replaying = 0;
		psp_me_window[0] = 0;
	}
	/* the ME can't take it: carried out here, as gedefer alone, after
	dropping the main CPU's stale copies of what the ME wrote */
	me_overlap_alone++;
	if (me_wrote)
	{
		sceKernelDcacheWritebackInvalidateAll();
		me_wrote = 0;
	}
	defer_replaying = 1;
	psp_me_context = defer_me_thread;
	defer_replay(defer_base, defer_used, 0);
	psp_me_context = 0;
	defer_replaying = 0;
	if (stall_me_address)
	{
		stall_forwarded++;
		gebusy_me_handed((unsigned int)stall_me_address);
		__real_sceGeListUpdateStallAddr(stall_me_qid, stall_me_address);
		stall_me_address = NULL;
	}
	defer_used = 0;
	defer_resets++;
}

/* ----- geinflight with gemeon: the frame on the Media Engine (F1,
docs/frames_in_flight.md) */

static PSP_MAIN_SIDE SceUID inflight_thread = -1;
static PSP_MAIN_SIDE unsigned int inflight_forwarded_stall = 0;
static PSP_MAIN_SIDE unsigned long inflight_forwards = 0;
/* how long the forwarder sleeps between looks. Each wake preempts the game
thread (25-32 us on the PSP-1000, 600-720 wakes a second at 1 ms:
design_mepoll.md), and the GE's lane has room to take the list in larger
steps: the frame's last stall address is passed on at the frame's wait
(defer_wait_me) whatever the period, so a longer one never leaves the list
short, it only lets the GE start each stretch up to a period later.
"mepoll" makes it gefwdN ms (4 if not given); otherwise 1 ms, as before */
static PSP_MAIN_SIDE unsigned int inflight_forward_us = 1000;

/* above the game's thread: once it runs it finishes its look before the game
thread can go on, so the game thread's last forwarding at the frame's wait
(defer_wait_me) is never followed by an older address from here */
static int inflight_forwarder(SceSize args, void *argp)
{
	(void)args;
	(void)argp;
	for (;;)
	{
		/* geshow2: a show asked comes only while no frame is out with the ME
		(the game thread asks it once the ME has ended the frame) */
		if (show2_asked)
		{
			show2_asked = 0;
			if (show3_on)
			{
				/* geshow3: the next frame's list is the game thread's;
				the GE is let into it once this frame is on the screen */
				unsigned int shown = show3_asked;
				void *held;

				show_picture(1); /* waits for the GE's finish of the frame */
				show3_shown = shown;
				__asm__ volatile("" ::: "memory");
				held = show3_gate_stall;
				if (held && show3_gate_show == shown)
					__real_sceGeListUpdateStallAddr(show3_gate_qid, held);
			}
			else
			{
				show_picture(0); /* waits for the GE's end of the frame */
				show2_list_start();
				psp_ge_show_busy = 0;
			}
			sceKernelSignalSema(show2_sema, 1);
			continue;
		}
		if (!inflight_me_out)
		{
			sceKernelSleepThread();
			continue;
		}
		{
			unsigned int stall = INFLIGHT_STALL[0];

			if (stall && stall != inflight_forwarded_stall)
			{
				inflight_forwarded_stall = stall;
				inflight_forwards++;
				gebusy_me_handed(stall);
				ge_stall_pass((int)INFLIGHT_STALL[1], (void *)stall);
			}
		}
		sceKernelDelayThread(inflight_forward_us);
	}
	return 0;
}

/* mode 2: the whole frame; 3 (mechunk): the frame's records from word
"from" on, and, on the ME, whatever more is published until the final mark.
1 if the ME took it */
static int inflight_hand_over_from(unsigned int from, unsigned int mode)
{
	unsigned int args[6];

	if (defer_me_on < 0)
		defer_me_on = psp_keep("gemeon");
	if (!defer_me_on || !psp_me_running() || !defer_used)
		return 0; /* carried out on the main CPU at the next frame's first call */
	if (inflight_room_lost())
		return 0;
	if (inflight_thread < 0)
	{
		if (psp_keep("mepoll"))
		{
			extern int psp_keep_number(const char *prefix, int fallback);
			int ms = psp_keep_number("gefwd", 4);

			inflight_forward_us = (unsigned int)(ms < 1 ? 1 : ms > 16 ? 16 : ms) * 1000;
			platform_log("GE: geinflight: mepoll: the forwarding thread looks every %u ms", inflight_forward_us / 1000);
		}
		if (show2_on < 0)
			show2_on = psp_keep("geshow2") != 0;
		show3_on = psp_keep("geshow3") != 0;
		if (show3_on)
		{
			show2_on = 1;
			show3_flag = sceKernelCreateEventFlag("ge finish", 0, 0, NULL);
			if (show3_flag < 0)
				show3_on = 0;
		}
		if (show2_on)
		{
			show2_sema = sceKernelCreateSema("ge show", 0, 0, 1, NULL);
			if (show2_sema < 0)
				show2_on = show3_on = 0;
		}
		/* geshow2: the show's log lines and a shot's file writes run there too */
		inflight_thread = sceKernelCreateThread("ge forward", inflight_forwarder, 0x10, show2_on ? 0x4000 : 0x1000, 0, NULL);
		if (inflight_thread < 0 || sceKernelStartThread(inflight_thread, 0, NULL) < 0)
		{
			platform_log("GE: geinflight: no forwarding thread (%08x); the frame is carried out on the main CPU",
				(unsigned int)inflight_thread);
			defer_me_on = 0;
			show2_on = show3_on = 0;
			return 0;
		}
		if (show3_on)
		{
			show3_check = psp_keep("geshow3check") != 0;
			sceGuSetCallback(GU_CALLBACK_FINISH, show3_finish_callback);
			platform_log("GE: geshow3: the next frame replayed by the ME into a second list while the GE ends the last; shown by the forwarding thread%s",
				show3_check ? "; geshow3check: reuses of what the GE reads counted against the frames not yet shown" : "");
		}
		if (show2_on)
		{
			show2_ring = !psp_keep("geshow2copy") && !psp_keep("geshow2nocopy");
			show2_copy_indices = !show2_ring && !psp_keep("geshow2nocopy");
			platform_log("GE: geshow2: the frame shown, and the list started again, by the forwarding thread while the game thread records the next%s",
				show2_ring ? "; the engine's index buffer from its two ends in turn, waiting for the show" :
				show2_copy_indices ? "; geshow2copy: the engine's index lists copied into the list" :
				"; geshow2nocopy: the engine's index lists handed to the GE as they are (the GE may draw a later frame's)");
		}
	}
	/* geshow2: the list this frame draws into starts once the last frame is
	shown (geshow3: its own list, its stall held back until that show) */
	if (!show3_on)
		show2_settle(SHOW2_HAND_OVER);
	else if (show3_check && show3_out < 2)
		show3_stamp_out();
	/* the records and what they point to that the main CPU wrote, to RAM */
	sceKernelDcacheWritebackAll();
	INFLIGHT_STALL[0] = 0;
	inflight_forwarded_stall = 0;
	args[0] = (unsigned int)defer_buffer;
	/* mechunk on the ME: the job reads its end from the published word */
	args[1] = mode == 3 && !psp_me_simulated() ? 0 : defer_used - from;
	args[2] = defer_me_thread;
	args[3] = 0; /* (psp_me.c sets it: on the ME or not) */
	args[4] = mode; /* the newest stall address back in the results, and published as it moves */
	args[5] = from;
	if (mode == 3)
	{
		CHUNK_LINE[1] = 0;
		CHUNK_LINE[2] = 0;
		__asm__ __volatile__("sync");
		CHUNK_LINE[0] = defer_used;
	}
	psp_me_window[0] = 1;
	inflight_me_out = 1;
	/* simulated (the emulator), the job runs inside psp_me_submit, here */
	defer_replaying = 1;
	if (!psp_me_submit(ME_SLOT_RENDER, ME_JOB_GE, args, 6))
	{
		defer_replaying = 0;
		inflight_me_out = 0;
		psp_me_window[0] = 0;
		return 0;
	}
	defer_replaying = 0;
	me_out = 1;
	me_wrote = 1;
	inflight_handed++;
	sceKernelWakeupThread(inflight_thread);
	return 1;
}

static void inflight_hand_over(void)
{
	(void)inflight_hand_over_from(0, 2);
}

/* mechunk: the records written so far handed on (mode 0), or the frame's
final mark (1, its end; 2, a call that runs at once). On the ME the first
piece hands the job over and each later one moves the published end; in the
emulator (no ME code runs) each piece is carried out here as the simulated
ME would, and the final mark hands over the rest as a job, so the frame
ends, and its newest stall address reaches the GE, as before */
static void chunk_publish(int mode)
{
	unsigned int mark = sceKernelGetSystemTimeLow();
	int simulated = psp_me_simulated();

	if (!chunk_frame)
		return;
	if (mode)
		chunk_frame = 0;
	if (simulated)
	{
		if (mode && !chunk_published)
		{
			/* nothing went in pieces: the whole frame as before */
			if (mode == 1)
				inflight_hand_over();
			return;
		}
		if (mode)
		{
			(void)inflight_hand_over_from(chunk_published, 3);
			chunk_published = 0;
			return;
		}
		if (!chunk_published)
		{
			inflight_me_out = 1;
			psp_me_window[0] = 1;
		}
		defer_replaying = 1;
		psp_me_context = defer_me_thread ? defer_me_thread : 1;
		replay_inflight = 1;
		defer_replay(defer_buffer + chunk_published, defer_used - chunk_published, 0);
		replay_inflight = 0;
		psp_me_context = 0;
		defer_replaying = 0;
		chunk_published = defer_used;
		chunk_simulated++;
		chunk_publishes++;
		return;
	}
	if (!inflight_me_out)
	{
		/* the frame's first piece (a final mark with none before: the whole
		frame, as before; a call that runs at once: carried out here) */
		if (mode == 1)
			inflight_hand_over();
		if (mode)
			return;
		if (!inflight_hand_over_from(0, 3))
		{
			chunk_frame = 0;
			return;
		}
	}
	else
	{
		sceKernelDcacheWritebackAll();
		__asm__ __volatile__("sync");
		CHUNK_LINE[0] = defer_used;
		if (mode)
		{
			__asm__ __volatile__("sync");
			CHUNK_LINE[1] = 1;
		}
		psp_me_touch(ME_SLOT_RENDER);
	}
	chunk_published = mode ? 0 : defer_used;
	chunk_publishes++;
	chunk_publish_us += sceKernelGetSystemTimeLow() - mark;
}

/* a piece, unless a texture the records bind wasn't marked this frame: from
there the rest of the frame waits for its end (psp_tex.c may load over a
texture that isn't marked while the ME runs) */
static void chunk_piece(void)
{
	if (psp_tex_chunk_missed)
	{
		chunk_stops_texture++;
		chunk_stopped = 1;
		return;
	}
	chunk_publish(0);
}

/* whether the next frame goes in pieces: "mechunk" with the frame in flight
on the ME and shown by geshow3; and the gate: the main CPU waited for the ME
past mechunkgateN ms (2) at the last frame not sent in pieces, which every
64th frame is, to look again ("mechunkall": every frame) */
static int chunk_wanted(void)
{
	static PSP_MAIN_SIDE unsigned int frames = 0;
	extern int psp_keep_number(const char *prefix, int fallback);

	if (chunk_on < 0)
	{
		chunk_on = psp_keep("mechunk") && !psp_keep("inflighttextold") && !psp_keep("gemeoverlap") &&
			!psp_keep("mebench");
		chunk_all = psp_keep("mechunkall");
		chunk_words = (unsigned int)psp_keep_number("mechunkkb", 8);
		chunk_words = (chunk_words < 4 ? 4 : chunk_words > 32 ? 32 : chunk_words) * 1024 / sizeof(defer_word);
		chunk_gate_us = (unsigned int)psp_keep_number("mechunkgate", 2) * 1000;
		if (chunk_on)
			platform_log("GE: mechunk: the frame handed to the ME in pieces of %u KB as it is written%s",
				(unsigned int)(chunk_words * sizeof(defer_word) / 1024),
				chunk_all ? " (mechunkall: every frame)" : "; gated on the main CPU's wait for the ME");
	}
	if (!chunk_on || !inflight_on || defer_me_on <= 0 || !show3_on || me_overlap || !pretouch_on)
		return 0;
	frames++;
	if (chunk_all)
		return 1;
	if ((frames & 63) == 0)
		return 0;
	if (chunk_wait_last_us > chunk_gate_us)
		return 1;
	chunk_gated++;
	return 0;
}

/* the frame back from the Media Engine: waited for, the GE given the rest,
what the ME wrote read fresh, and the main CPU's part of its end */
static void inflight_wait_me(void)
{
	unsigned long failed = defer_me_failed;
	unsigned int wait_mark = sceKernelGetSystemTimeLow();
	int was_chunked = chunk_out;

	inflight_me_out = 0;
	defer_wait_me(); /* the last stall address passed on with it */
	if (!was_chunked)
		chunk_wait_last_us = sceKernelGetSystemTimeLow() - wait_mark;
	psp_me_window[0] = 0;
	sceKernelDcacheWritebackInvalidateAll();
	{
		/* texlinecheck and heapwalk: what the ME's frame left (psp_tex.c, psp_libc.c) */
		extern void psp_texline_check(void);
		extern void psp_heap_walk(const char *where);

		psp_texline_check();
		psp_heap_walk("the frame back from the ME");
	}
	me_wrote = 0;
	if (defer_me_failed != failed)
	{
		/* the ME stopped (HOME, sleep) with the frame perhaps half carried
		out: the list ended where the main CPU last saw it, so the GE stops
		there and nothing waits for ever; the main CPU carries the frames out
		from here */
		inflight_me_failed++;
		defer_me_on = 0;
		inflight_finish_left = 0;
		stall_force = 1;
		sceGuFinish();
		stall_force = 0;
		inflight_list_ended = 1;
		platform_log("GE: geinflight: the Media Engine failed with a frame out; the main CPU carries the frames out from here");
	}
	defer_used = 0;
	defer_resets++;
	defer_drains++;
	inflight_pending = 0;
	inflight_show();
}

/* geinflight's buffer in the texture RAM block (inflight_room): the PSP
slept since the records waiting were begun (the block went back to the
system and came again: a new generation), or the block is away. They may
not be there any more, so they're dropped rather than carried out; a frame
whose start was carried out before is ended where it is, so the next one
can begin. 1 if they were dropped */
static int inflight_room_lost(void)
{
	unsigned int generation;

	if (!inflight_room || !defer_used)
		return 0;
	if (psp_tex_inflight_room(&generation) == (void *)defer_buffer && generation == inflight_room_generation)
		return 0;
	inflight_room_drops++;
	platform_log("GE: geinflight: %u KB of records begun before a sleep dropped, not carried out (%lu times)",
		(unsigned int)(defer_used * sizeof(defer_word) / 1024), inflight_room_drops);
	defer_used = 0;
	defer_resets++;
	inflight_part_words = 0;
	inflight_pending = 0;
	if (frame_open)
	{
		/* as its record would be: carried out, not written down again */
		defer_replaying = 1;
		inflight_list_ended = frame_end_list();
		defer_replaying = 0;
		inflight_show();
	}
	return 1;
}

/* at each frame's first window, and after a sleep at the record the game
thread waited at: if the block came back without the front room (the
system kept the partition), the buffer moves to the heap; without room
there, deferring stops for good at a frame's start, and mid-frame (the
calls written down can't be run as they come) the game stops */
static void inflight_room_check(int mid_frame)
{
	unsigned int generation, words = inflight_buffer_words;
	void *room;

	if (!inflight_room)
		return;
	room = psp_tex_inflight_room(&generation);
	/* the same room, or the block given back for a sleep that hasn't come
	(the threads stop soon after the power callback) */
	if (room == (void *)defer_buffer || (!room && generation == inflight_room_generation))
		return;
	(void)inflight_room_lost();
	inflight_room = 0;
	defer_buffer = memalign(64, words * sizeof(defer_word));
	if (!defer_buffer)
	{
		words = DEFER_INFLIGHT_KB * 1024 / sizeof(defer_word);
		defer_buffer = memalign(64, words * sizeof(defer_word));
	}
	if (defer_buffer)
	{
		defer_base = defer_buffer;
		defer_limit = inflight_buffer_words = words;
		platform_log("GE: geinflight: the texture RAM block came back without its front room; %u KB of the heap for the records",
			(unsigned int)(words * sizeof(defer_word) / 1024));
		return;
	}
	if (mid_frame)
	{
		extern void psp_exit_game(const char *why);

		psp_exit_game("geinflight: the texture RAM block came back from a sleep without its front room, and the heap has no room for the records");
	}
	defer_asked = 0;
	defer_on = 0;
	platform_log("GE: geinflight: the texture RAM block came back without its front room and the heap has none: gedefer off");
}

/* the power callback's thread (psp_platform.c), the ME stopped: the game
thread held at its next record before the block goes back. A thread that
isn't writing records (the ticks, a load) isn't waited for past 300 ms: it
holds at its first one */
void psp_ge_inflight_room_suspend(void)
{
	int waited;

	if (!inflight_room)
		return;
	inflight_room_away = 1;
	for (waited = 0; !inflight_room_parked && waited < 300; waited++)
		sceKernelDelayThread(1000);
}

/* awake, the block taken again (psp_tex_resume): the game thread goes on */
void psp_ge_inflight_room_resume(void)
{
	inflight_room_away = 0;
}

/* ----- everything waiting, carried out in order */
static void defer_drain(void)
{
	/* geinflight: a call that runs at once while the frame is out with the
	Media Engine (a text update, a full buffer) waits for it; a frame going
	over in pieces gets its final mark first, so the ME's job ends */
	if (chunk_frame)
	{
		chunk_stops_drain++;
		chunk_publish(2);
	}
	if (inflight_me_out)
	{
		inflight_wait_me();
		if (!defer_used)
			return;
	}
	if (inflight_room_lost())
		return;
	/* geshow2: carried out here, into the list the show starts again */
	show2_settle(SHOW2_DRAIN);
	if (me_overlap)
	{
		/* build 118: the half being written out too, then both done, and
		what the ME wrote (this file's state, pspgu's, the list) read fresh
		from RAM before the call that runs at once runs here */
		defer_hand_over();
		defer_wait_me();
		if (me_wrote)
		{
			sceKernelDcacheWritebackInvalidateAll();
			me_wrote = 0;
		}
		defer_drains++;
		inflight_pending = 0;
		return;
	}
	defer_replaying = 1;
	if (!defer_on_me())
	{
		psp_me_context = defer_me_thread;
		defer_replay(defer_buffer, defer_used, 0);
		psp_me_context = 0;
	}
	/* the GE given what the run wrote (the newest stall address it kept) */
	if (stall_me_address)
	{
		stall_forwarded++;
		list_write_back(stall_me_address); /* build 114: listcachedall */
		gebusy_me_handed((unsigned int)stall_me_address);
		ge_stall_pass(stall_me_qid, stall_me_address);
		stall_me_address = NULL;
	}
	defer_used = 0;
	defer_resets++;
	defer_replaying = 0;
	defer_drains++;
	inflight_pending = 0;
	inflight_show();
}

/* B41 (psp_sleepguard.c): a sleep halo_sleep.prx put off is noticed at an
in-flight frame's end and carried out at the end of the next, which isn't in
flight */
void psp_sleepguard_notice(void);
void psp_sleepguard_frame(void);

/* geinflight: the main CPU's part of a frame's end, once its list has ended
and the GE has been given all of it */
static void inflight_show(void)
{
	if (!inflight_list_ended)
		return;
	inflight_list_ended = 0;
	psp_me_wake_poll();
	psp_me_ring_test();
	/* geshow3: the last frame's show, before this frame's end lets go of
	anything, so only this frame stays out with the GE */
	if (show3_on)
		show2_settle(SHOW3_ME_END);
	show2_now = show2_on > 0 && inflight_thread >= 0;
	frame_end_show();
	show2_now = 0;
}

/* geshow3: whether a stall address for the list started before the last
frame was shown waits for that show. The newest is kept first, then the show
looked at: the forwarding thread (above this one) marks the show, then passes
on what is kept, so between them nothing is lost */
static int show3_stall_held(int qid, void *stall)
{
	if (qid != show3_gate_qid)
		return 0;
	show3_gate_stall = stall;
	__asm__ volatile("" ::: "memory");
	if ((int)(show3_shown - show3_gate_show) >= 0)
		return 0;
	show3_held++;
	return 1;
}

static int ge_stall_pass(int qid, void *stall)
{
	if (show3_gate_qid >= 0 && !psp_on_me() && show3_stall_held(qid, stall))
		return 0;
	return __real_sceGeListUpdateStallAddr(qid, stall);
}

/* geshow3check: a reuse of what the GE reads, of a kind whose last use was
in the frame counter's frame stamp */
void psp_ge_show3_reuse(int kind, unsigned int stamp)
{
	if (!show3_check || PSP_ME_WORK())
		return;
	show3_checked[kind]++;
	if (show3_out > 0 && (int)(stamp - show3_out_stamp[0][kind]) >= 0)
		show3_reused[kind]++;
}

/* geshow2: the game thread takes the end of the show it asked (the GE done
with the frame, the next list started), before anything of its own reaches
the list or moves what the GE reads */
static void show2_settle(int why)
{
	unsigned int start;

	if (!show2_owed || psp_on_me())
		return;
	show2_owed = 0;
	start = sceKernelGetSystemTimeLow();
	sceKernelWaitSema(show2_sema, 1, NULL);
	show2_waited[why]++;
	show2_wait_us[why] += sceKernelGetSystemTimeLow() - start;
	if (show3_on)
	{
		psp_ge_show_busy = 0;
		if (show3_out > 0)
		{
			memcpy(show3_out_stamp[0], show3_out_stamp[1], sizeof(show3_out_stamp[0]));
			show3_out--;
		}
	}
}

/* geshow2's ring of engine triangles (psp_fixups.py): on, and a range about
to reach the last frame's waits until the GE has done with it */
int psp_ge_triangle_ring(void)
{
	return show2_ring;
}

void psp_ge_triangles_wait(void)
{
	if (!show2_owed)
		return;
	show2_ring_waits++;
	show2_settle(SHOW2_OTHER);
}

/* geshow2: the show of the frame whose list has ended, asked of the
forwarding thread; the game thread goes on to the next frame's record */
static void show2_ask(void)
{
	show2_shows++;
	psp_ge_show_busy = 1;
	show2_owed = 1;
	show2_list_started = 1;
	show2_asked = 1;
	sceKernelWakeupThread(inflight_thread);
}

/* geshow2: a list the show started for a frame that didn't come (a load's
own frames start theirs), finished empty so the GE's queue doesn't stall
at it */
static void show2_list_drop(void)
{
	show2_settle(SHOW2_OTHER);
	if (!show2_list_started)
		return;
	show2_list_started = 0;
	stall_force = 1;
	sceGuFinish();
	sceGuSync(0, 0);
	stall_force = 0;
}

/* the messages the Media Engine's work logged: each format with a count */
#define ME_LOG_FORMATS 16
static const char *me_log_format[ME_LOG_FORMATS];
static unsigned long me_log_count[ME_LOG_FORMATS], me_log_lost = 0;

static void ge_log(const char *format, ...)
{
	char line[512]; /* the log's own line limit (psp_platform.c) */
	va_list arguments;

	if (PSP_ME_WORK())
	{
		int k;

		for (k = 0; k < ME_LOG_FORMATS && me_log_format[k] && me_log_format[k] != format; k++)
			;
		if (k < ME_LOG_FORMATS)
		{
			me_log_format[k] = format;
			me_log_count[k]++;
		}
		else
			me_log_lost++;
		return;
	}
	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	(platform_log)("%s", line);
}

/* at the frame's end, on the main CPU: what the Media Engine's work logged */
static void me_log_write(void)
{
	int k;

	for (k = 0; k < ME_LOG_FORMATS && me_log_format[k]; k++)
	{
		(platform_log)("GE (the Media Engine's work, %lu times): %s", me_log_count[k], me_log_format[k]);
		me_log_format[k] = NULL;
		me_log_count[k] = 0;
	}
	if (me_log_lost)
		(platform_log)("GE (the Media Engine's work): %lu more messages of other kinds", me_log_lost);
	me_log_lost = 0;
}
static int list_in_vram = 0, vram_busy = 0, vram_busy_calibrating = 0, vram_busy_sprites = 8;
static unsigned int vram_busy_target_us = 15000;

static inline int ge_draw_skipped(void)
{
	if (!ge_skip || !((ge_skip & GE_SKIP_ALL) || (ge_skip & ge_category)))
		return 0;
	ge_skipped_draws++;
	return 1;
}
/* every draw in this file goes through the gate (a parenthesized name,
(sceGuDrawArray)(...), is the real call: the VRAM load) */
/* build 115: "gereads" counts what each draw makes the GE read (ge_reads_note,
after the copies' arena below); the arguments are taken once */
static int ge_reads_on = 0;
static void ge_reads_note(int vtype, int count, const void *indices, const void *vertices);
/* build 116: "vrammodelsN": a model buffer's VRAM copy, when it has one
(vm_redirect, by the model buffers' copies below) */
static int vm_on = -1; /* -1: decided at the next frame (vm_reset) */
static const void *vm_redirect(const void *vertices, int count);
static void vm_reset(void);
static void vm_frame_end(void);
static void vm_report(void);
static void vm_level_note(void *vertices, unsigned int bytes);
static void vm_level_move(void *from, void *to);
#define sceGumDrawArray(p, v, c, i, d) do { if (!ge_draw_skipped()) { int ge_v_ = (v), ge_c_ = (c); \
	const void *ge_i_ = (i), *ge_d_ = (d); if (vm_on > 0) ge_d_ = vm_redirect(ge_d_, ge_c_); \
	if (ge_reads_on > 0) ge_reads_note(ge_v_, ge_c_, ge_i_, ge_d_); \
	sceGumDrawArray(p, ge_v_, ge_c_, ge_i_, ge_d_); } } while (0)
#define sceGuDrawArray(p, v, c, i, d) do { if (!ge_draw_skipped()) { int ge_v_ = (v), ge_c_ = (c); \
	const void *ge_i_ = (i), *ge_d_ = (d); if (vm_on > 0) ge_d_ = vm_redirect(ge_d_, ge_c_); \
	if (ge_reads_on > 0) ge_reads_note(ge_v_, ge_c_, ge_i_, ge_d_); \
	sceGuDrawArray(p, ge_v_, ge_c_, ge_i_, ge_d_); } } while (0)
/* draws stop this close to the display list's end (each draw adds its
commands, and its copied indices and cut-up corners, to the list) */
#define DISPLAY_LIST_MARGIN 0x3000
/* and the 3D world stops this much earlier, so the HUD always has room */
#define HUD_RESERVE 0xC000
/* build 82: and the world's solid draws stop this much earlier still, so
the see-through draws (wall symbols, decals, the beam) always have room:
they are flushed last, and when the world had filled its share they were
the ones skipped (b79 run 3 at red base in the tank: 188 draws skipped,
the reticle's bracket missing a piece, the 2D items flickering as the
list's size crossed the limit from frame to frame) */
#define TRANSPARENT_RESERVE 0x8000
static unsigned long list_full_skips = 0, hud_reserve_low = 0;
static int list_limit = DISPLAY_LIST_BYTES - HUD_RESERVE - TRANSPARENT_RESERVE;
static void transparent_flush(void);
static void taa_resolve(void);
static void blip_place(void);
static int hud_phase = 0;

/* build 116 (b113 #6): the world drawn with the GE's ordered dither, as a
16-bit frame needs to keep the Xbox's 32-bit gradients: without it the
dark areas and the multiply passes (lightmap, detail maps) went in visible
steps. The HUD and the 2D items keep flat colours (a dithered flat colour
is a speckle). Keep word "nodither": as before.

build 117: the dither only where a draw lays down a new colour. A blended
draw reads the frame back and rounds it again, and the dither moves each
pixel by the offset of its place in the 4x4 matrix: the same offset in
every pass. Where a pass changes a pixel little (a layer of smoke, a
shadow's quad outside the shadow, a detail map near its neutral), the
rounding goes the same way pass after pass, and the passes build a fixed
pattern: b116's test 1 showed it on shadows and particles, swinging up to
140 of 255 under the Pelican's dust [M]. So the dither is on for draws that
don't blend, and for the level's lightmap pass, which sets the level's
brightness (where b113 #6's steps showed); off for every other blended
draw. Keep word "ditherall": b116's dither on every draw of the world.

"dithershift" (opt-in, the A/B for the dark bands): the passes that lay
light over the level (its detail passes and the sky's layers, with the
lightmap pass) dithered too, each with the matrix moved to the next of its 8
places. A pixel's rounding then goes a different way from pass to pass, so
no fixed pattern can build. Particles, shadows and decals stay undithered:
the GE truncates, and reads the frame back with its top bits copied into
the low ones (PPSSPP's model; "ditherprobe" measures the PSP's), so a pass
that leaves a dark pixel almost as it was moves it down a step whenever its
offset is negative, wherever the pattern stands; under many layers that
adds up. "dithershiftall": every blended draw of the world dithered and
moved, for the comparison */
/* the shift is built in only with PSP_DITHERSHIFT=1: its tests sit in the
blend switches and in the detail and see-through loops, hot paths where
code that does nothing still costs (b133's nwbatch hooks, c10 +7% while
off). As 0, every test below folds away */
#ifndef PSP_DITHERSHIFT
#define PSP_DITHERSHIFT 0
#endif
static int dither_off = -1, dither_all = -1, dither_shift = -1; /* 1 the passes, 2 every blend */
static int dither_world = 0; /* the world's turn: the dither may be on */
static int dither_now = -1, blend_now = 0; /* what the GE was last given */
static int dither_place = 0;
static unsigned long dither_moves = 0; /* passes dithered at a moved place, since the last report */
static ScePspIMatrix4 dither_moved[8];

/* the matrix at each of its 8 places: moved by (2, 2) it repeats. In this
order every two passes in a row nearly cancel (-4 then 2, -3 then 3, ...)
for every pixel, and eight give each pixel every offset once */
static void dither_moves_make(const ScePspIMatrix4 *matrix)
{
	static const unsigned char places[8][2] = { { 0, 0 }, { 0, 1 }, { 2, 0 }, { 2, 1 }, { 1, 0 }, { 1, 1 }, { 3, 0 }, { 3, 1 } };
	const int *from = &matrix->x.x; /* the 16 cells, row by row */
	int k, row, column;

	for (k = 0; k < 8; k++)
	{
		int *to = &dither_moved[k].x.x;

		for (row = 0; row < 4; row++)
			for (column = 0; column < 4; column++)
				to[row * 4 + column] = from[((row + places[k][1]) & 3) * 4 + ((column + places[k][0]) & 3)];
	}
}

static void dither_set(int on)
{
	if (on == dither_now)
		return;
	if (on)
		sceGuEnable(GU_DITHER);
	else
		sceGuDisable(GU_DITHER);
	dither_now = on;
}

/* a pass that lays light over what's drawn: the dither on, at its next place */
static void ge_dither_pass(void)
{
	if (!PSP_DITHERSHIFT || !dither_world || dither_shift <= 0)
		return;
	dither_place = (dither_place + 1) & 7;
	dither_moves++;
	sceGuSetDither(&dither_moved[dither_place]);
	dither_set(1);
}

/* after the blend is on, for a pass that lays light over the level (its
detail passes, the sky's layers): under "dithershift" those, and only
those, are dithered ("dithershiftall" has done it in ge_blend) */
static void ge_layer_dither(void)
{
	if (PSP_DITHERSHIFT && dither_shift == 1)
		ge_dither_pass();
}

/* the world's turn begins (1) or ends (0) */
static void ge_dither(int on)
{
	/* a 4x4 Bayer matrix spanning one 5-bit step (8 levels): enough to hide
	red's and blue's steps, and two of green's 6-bit steps, so the pattern
	shows most in green. "ditherhalf" (opt-in, a look call): half the
	span, half the pattern; the 5-bit steps come back as half-steps */
	static const ScePspIMatrix4 matrix = { { -4, 0, -3, 1 }, { 2, -2, 3, -1 }, { -3, 1, -4, 0 }, { 3, -1, 2, -2 } };
	static const ScePspIMatrix4 half = { { -2, 0, -2, 0 }, { 1, -1, 1, -1 }, { -2, 0, -2, 0 }, { 1, -1, 1, -1 } };
	/* "ditherlift" (on by default, keep_defaults; noditherlift): every offset 4 higher.
	The matrix above is centred for a GE that rounds; one that truncates
	(PPSSPP's, and ditherprobe's row 0 on the PSP) loses half a step with it
	on every dithered draw, which shows most in the dark. Lifted, a
	truncating write is right on average */
	static ScePspIMatrix4 chosen;

	if (dither_off < 0)
	{
		const int *from = psp_keep("ditherhalf") ? &half.x.x : &matrix.x.x;
		int *to = &chosen.x.x, lift = psp_keep("ditherlift") ? 4 : 0, cell;

		dither_off = psp_keep("nodither");
		dither_all = psp_keep("ditherall");
		for (cell = 0; cell < 16; cell++)
			to[cell] = from[cell] + lift;
		dither_shift = psp_keep("dithershiftall") ? 2 : psp_keep("dithershift");
		if (dither_shift && !PSP_DITHERSHIFT)
		{
			platform_log("GE: %s kept, but this build has no dither shift (PSP_DITHERSHIFT=1 builds it)",
				dither_shift == 2 ? "dithershiftall" : "dithershift");
			dither_shift = 0;
		}
		if (dither_shift)
			dither_moves_make(&chosen);
	}
	if (dither_off)
		return;
	if (on)
		sceGuSetDither(&chosen);
	dither_place = 0; /* each frame starts at the same place: frame-locked shots repeat */
	dither_world = on;
	dither_now = -1; /* given again, so each frame starts from a known state */
	dither_set(on && (dither_all || !blend_now));
}

/* blending on or off, and the dither with it (build 117) */
static void ge_blend(int on)
{
	if (on)
		sceGuEnable(GU_BLEND);
	else
		sceGuDisable(GU_BLEND);
	blend_now = on;
	if (PSP_DITHERSHIFT && on && dither_shift == 2)
		ge_dither_pass();
	else if (dither_world)
		dither_set(dither_all || !on);
}

/* blending on with the dither kept: the level's lightmap pass (build 117) */
static void ge_blend_dithered(void)
{
	sceGuEnable(GU_BLEND);
	blend_now = 1;
	if (PSP_DITHERSHIFT && dither_shift > 0)
		ge_dither_pass();
	else if (dither_world)
		dither_set(1);
}

/* the HUD's turn: the reserve is its. The world's see-through draws are
flushed first, inside the world's share (build 34: in build 33 the flush
came after the switch and the effects of a firefight ate the HUD's room) */
static void list_hud_phase(void)
{
	if (hud_phase)
		return;
	transparent_flush();
	taa_resolve();
	ge_dither(0); /* build 116 */
	hud_phase = 1;
	list_limit = list_bytes;
}

int psp_keep(const char *name);

/* ---------- build 103: what fills the display list, and the level's room

b102's runs 3 and 6 (The Library): in the Flood fight the level
vanished (0 level draws while 340 KB of the list was in use; 25,000 draws
skipped), and in run 6 about 1,100 draws were skipped every frame for the
last half of the run, with the player idling in a corner. Which kind of draw fills
the list wasn't logged. Each public entry now says what kind of draw it is
(ge_kind); the bytes the list grows by are charged to the kind running,
with its skipped draws; the report gives the average and the fullest frame,
and the order the kinds came in that frame.

The level's room: until the level's first draw of the frame, every other
kind stops LEVEL_RESERVE bytes earlier, so what is drawn before the level
can't leave it nothing (run 3's 0 level draws). Keep word "nolevelreserve":
as before */
enum { KIND_OTHER, KIND_SKY, KIND_LEVEL, KIND_LIGHT, KIND_LEVEL_SEETHROUGH, KIND_MODEL, KIND_SEETHROUGH_QUEUE,
	KIND_DECAL, KIND_SPRITE, KIND_WIDGET, KIND_SHADOW, KIND_SEETHROUGH_FLUSH, KIND_SCREEN, KIND_HUD, KINDS };
static const char *const kind_names[KINDS] = { "other", "sky", "level", "lights", "level see-through (stencil bake)",
	"models", "see-through queued", "decals", "sprites (particles)", "flares", "shadows", "see-through drawn", "screen quads", "HUD" };
static const char kind_letters[KINDS] = { 'o', 'k', 'L', 'l', 'B', 'M', 'q', 'd', 'p', 'f', 's', 'T', 'Q', 'H' };
#define LEVEL_RESERVE 0x18000 /* 96 KB */
/* The decals' and particles' room. The light pass comes before them in the
frame, and where many dynamic lights reach a lot of level (a10's lit
corridors in a firefight) it can take most of the list by itself: then the
blood, scorch marks and floor stencils, and the particles, were the draws
left out, and whole decals vanished for as long as the fight lasted. Light
draws now stop LIGHT_RESERVE bytes earlier; what the light pass loses is
some light on the farthest surfaces in the fullest frames. Keep word
"nolightreserve": as before */
#define LIGHT_RESERVE 0x10000 /* 64 KB */
static int light_reserve_on = -1;
static unsigned long light_reserve_held = 0;
static int ge_kind_now = KIND_OTHER, level_reserve_on = -1, level_begun = 0;
static unsigned int kind_mark = 0, kind_seen = 0;
static unsigned int kind_frame_bytes[KINDS], kind_peak_bytes[KINDS], kind_frame_skips[KINDS], kind_peak_skips[KINDS];
static unsigned long long kind_bytes[KINDS];
static unsigned long kind_skips[KINDS], kind_frames = 0, level_reserve_held = 0;
static unsigned int kind_peak_total = 0;
/* build 107: what the models' share is made of (b106 on b30's beach: 175 KB a
frame of models, and up to 235 model draws a frame skipped). Each skinned
batch loads its slots' bone matrices (13 words each) for every pass */
static unsigned long model_bone_loads = 0, model_take_bytes = 0, model_part_calls = 0;
/* build 107: bone loads go into sub-lists that each pass calls (model_geometry) */
static unsigned long model_bone_calls = 0, model_bone_sub_bytes = 0;
/* build 107: rigid parts near the eye, and those drawn whole (rigid_part_whole);
bending batches near the eye drawn whole, with no copy */
static unsigned long near_parts = 0, near_parts_whole = 0, near_batches_whole = 0;
/* build 118: the near path's sphere test (batch_sphere_inside) */
static int near_sphere_full = -1, near_sphere_check = 0;
static unsigned long near_batches_by_sphere = 0, near_sphere_mismatches = 0;
/* build 119: the near path's run test (near_cluster_verdict), and the runs'
spheres on the heap */
#define NEAR_CLUSTER_TRIANGLES 16
static int near_cluster_full = -1, near_cluster_check = 0;
static unsigned long near_cluster_kept = 0, near_cluster_dropped = 0, near_cluster_mixed = 0, near_cluster_mismatches = 0;
static unsigned long near_cluster_bytes = 0;
/* build 119: the rigid near path's runs (rigid_run_verdict) */
/* three words read on every draw or stream use, kept in one cache line:
apart, each cost a data-cache miss of its own (recon's pass table: ~860
misses a tick on b30) */
static struct
{
	int rigid_runs_full;          /* the rigid runs' mode, worked out once */
	unsigned int stream_frame;    /* the frame the level's streams were last used in */
	unsigned long level_copies_left; /* the level's copies left out of exact reach */
} ge_hot __attribute__((aligned(16))) = { -1, 1, 0 };

static int rigid_runs_check = 0;
static unsigned long rigid_runs_kept = 0, rigid_runs_dropped = 0, rigid_runs_mixed = 0, rigid_runs_mismatches = 0,
	rigid_runs_made = 0, rigid_runs_bytes = 0;
static int skin_axis_loop = -1, skin_axis_check = 0; /* build 119: model_geometry's corner sums, "skinaxisloop" */
static unsigned long skin_axis_checked = 0, skin_axis_differ = 0;
/* build 119's probe: the near path's corner loop, timed per batch on the
PSP (the rig: a30's model geometry is 28.6 ms a frame), and the bone slots'
matrices per batch */
static unsigned long near_loop_us = 0, near_loop_batches = 0, near_loop_triangles = 0, near_corners_worked = 0,
	slot_us = 0, slot_batches = 0;
static char kind_order[KINDS + 1], kind_peak_order[KINDS + 1];
static int kind_order_length = 0;

static void gebusy_kind_cut(int kind);
static int blip_depth_live; /* blipdepth (the motion sensor, below) */
static unsigned long blip_depth_world_after;
static unsigned long blip_depth_after_kind[KINDS], blip_depth_after_tested;

static void ge_kind(int kind)
{
	unsigned int now;

	gebusy_kind_cut(kind); /* build 115 */
	/* (the see-through flush is counted where it has something to draw, and
	the frame's end marks its kind as OTHER without drawing) */
	if (__builtin_expect(blip_depth_live, 0) && kind != KIND_HUD && kind != KIND_SCREEN && kind != KIND_OTHER &&
		kind != KIND_SEETHROUGH_FLUSH)
	{
		blip_depth_world_after++;
		blip_depth_after_kind[kind]++;
		if (sceGuGetStatus(GU_DEPTH_TEST))
			blip_depth_after_tested++;
	}
	now = sceGuCheckList();

	if (now > kind_mark)
		kind_frame_bytes[ge_kind_now] += now - kind_mark;
	kind_mark = now;
	ge_kind_now = kind;
	if (!PSP_ME_WORK())
	{
		extern int psp_tex_screen_pass; /* psp_tex.c: the HUD's textures pinned */

		psp_tex_screen_pass = kind == KIND_HUD || kind == KIND_SCREEN;
	}
	if (!(kind_seen & (1u << kind)) && kind_order_length < KINDS)
	{
		kind_seen |= 1u << kind;
		kind_order[kind_order_length++] = kind_letters[kind];
	}
	if (kind == KIND_LEVEL)
		level_begun = 1;
}

static void ge_kinds_frame_begin(void)
{
	memset(kind_frame_bytes, 0, sizeof(kind_frame_bytes));
	memset(kind_frame_skips, 0, sizeof(kind_frame_skips));
	kind_mark = sceGuCheckList();
	kind_seen = 0;
	kind_order_length = 0;
	ge_kind_now = KIND_OTHER;
	if (!PSP_ME_WORK())
	{
		extern int psp_tex_screen_pass;

		psp_tex_screen_pass = 0;
	}
	level_begun = 0;
	if (level_reserve_on < 0)
		level_reserve_on = !psp_keep("nolevelreserve");
	if (light_reserve_on < 0)
		light_reserve_on = !psp_keep("nolightreserve");
}

/* build 107: when the list is full, the smallest models on the screen go
first. b106 on b30's beach: with the list full, whichever draws came last
were skipped (up to 235 model draws a frame), a different set each frame, so
rocks, crates and trees nearby flickered in and out. Now a model whose
bounding sphere is smaller on the screen than model_size_floor (its
diameter, in pixels) isn't drawn. The floor rises by a quarter each frame
that skipped model draws for room, up to MODEL_SIZE_FLOOR_MOST, and comes
back down slowly once the list has room again, so what goes missing is the
farthest, smallest models, the same ones from frame to frame. The first-person
weapon and the sky are never left out. Keep word "nosizefloor": as before */
#define MODEL_SIZE_FLOOR_MOST 8.0f
static float model_size_floor = 0.0f, model_size_floor_peak = 0.0f, model_pixels = 1.0e9f;
static int size_floor_on = -1;
static unsigned long model_floor_left_out = 0;

static void model_size_floor_update(unsigned int total)
{
	if (size_floor_on < 0)
		size_floor_on = !psp_keep("nosizefloor");
	if (!size_floor_on)
		return;
	if (kind_frame_skips[KIND_MODEL])
		model_size_floor = model_size_floor < 2.0f ? 2.0f : model_size_floor * 1.25f;
	else if (total < (unsigned int)list_limit * 3 / 4)
		model_size_floor = model_size_floor < 1.5f ? 0.0f : model_size_floor * 0.95f;
	if (model_size_floor > MODEL_SIZE_FLOOR_MOST)
		model_size_floor = MODEL_SIZE_FLOOR_MOST;
	if (model_size_floor > model_size_floor_peak)
		model_size_floor_peak = model_size_floor;
}

static void ge_kinds_frame_end(void)
{
	unsigned int total = 0;
	int k;

	ge_kind(KIND_OTHER);
	for (k = 0; k < KINDS; k++)
	{
		kind_bytes[k] += kind_frame_bytes[k];
		kind_skips[k] += kind_frame_skips[k];
		total += kind_frame_bytes[k];
	}
	model_size_floor_update(total);
	kind_frames++;
	if (total >= kind_peak_total)
	{
		kind_peak_total = total;
		memcpy(kind_peak_bytes, kind_frame_bytes, sizeof(kind_peak_bytes));
		memcpy(kind_peak_skips, kind_frame_skips, sizeof(kind_peak_skips));
		memcpy(kind_peak_order, kind_order, kind_order_length);
		kind_peak_order[kind_order_length] = 0;
	}
}

/* ---------- the draw census ("drawcensus" in keep.txt; recon only)

Three counts that size drawing levers before any is built (docs/recon_134.md).
Each reads what the drawing already works out and changes nothing drawn.
- Near the eye: the model definitions the near-plane work comes from (rigid
  parts tested corner by corner, bending batches whose corners were worked
  out). render_objects.c names the object being drawn in psp_census_definition
  (psp_fixups.py, FIXES_RECON #modelcensus).
- The change-colour pass: the distinct base texture, multipurpose map and
  colour triples it multiplies together, and what a tinted copy of each base
  texture would take, so baking the tint once can be weighed against a pass
  a draw.
- The light pass: its triangles that sit in runs of 16 (in the draw's order)
  whose every corner lies beyond the light's radius, which a sphere a run
  would skip whole; and the draws split between spotlights and point lights.
Built only with PSP_RECON=1: even switched off, a hook in hot code costs
(docs/shelf.md), so the release builds carry none of it. */
#ifdef PSP_RECON
extern long psp_census_definition; /* render_objects.c (psp_fixups.py, FIXES_RECON #modelcensus) */
static int census_on = -1;
static unsigned long census_frames = 0;

#define CENSUS_NEAR_SLOTS 64
static struct
{
	long definition;
	int used;
	unsigned long rigid_parts, rigid_triangles, bending_triangles;
} census_near_table[CENSUS_NEAR_SLOTS];

#define CENSUS_TINTS 512
/* each triple's passes since the start and its tinted copy's size at 16 bits,
so the report can say what a cache of the most drawn would hold and cover */
static struct { unsigned int base, change, color; unsigned long passes, bytes16; } census_tints[CENSUS_TINTS];
static int census_tint_count = 0;
static unsigned long census_tint_passes = 0, census_tint_unkept = 0, census_tint_bytes16 = 0, census_tint_bytes8 = 0;

static unsigned long census_light_draws[2] = { 0, 0 }, census_light_looked[2] = { 0, 0 };
/* the light pass by surface: within a frame, how many lights draw each level
vertex buffer, and how many of their triangles another light already drew that
frame (a triangle is its buffer and its three vertex indices: each light hands
over a triangle list of its own, so the lists themselves never repeat). What
drawing a surface once for all the lights that reach it would save */
#define CENSUS_SURFACES 512
static struct { const void *vertices; const unsigned short *indices; long triangles; int lights; } census_surfaces[CENSUS_SURFACES];
static int census_surface_count = 0;
static unsigned long census_surface_lost = 0, census_surfaces_lit[4] = { 0 }, census_surface_draws = 0;
static unsigned long census_surface_triangles = 0, census_surface_repeat_triangles = 0, census_triangles_lost = 0;
#define CENSUS_TRIANGLES 8192 /* a frame's triangles seen, by hash (32 KB) */
static unsigned int census_triangle_seen[CENSUS_TRIANGLES];
static unsigned long census_light_run_triangles = 0, census_light_runs_out = 0;

char *tag_get_name(long tag_index);
int psp_tex_size(unsigned int key, int *width, int *height);

static int census_wanted(void)
{
	if (census_on < 0)
		census_on = psp_keep("drawcensus");
	return census_on;
}

static void census_near(int rigid, long triangles)
{
	long definition = psp_census_definition;
	unsigned int slot, probe;

	if (!census_wanted() || definition == -1)
		return;
	slot = ((unsigned int)definition * 2654435761u) % CENSUS_NEAR_SLOTS;
	for (probe = 0; probe < CENSUS_NEAR_SLOTS; probe++, slot = (slot + 1) % CENSUS_NEAR_SLOTS)
	{
		if (!census_near_table[slot].used)
		{
			census_near_table[slot].used = 1;
			census_near_table[slot].definition = definition;
		}
		if (census_near_table[slot].definition == definition)
		{
			if (rigid)
			{
				census_near_table[slot].rigid_parts++;
				census_near_table[slot].rigid_triangles += (unsigned long)triangles;
			}
			else
				census_near_table[slot].bending_triangles += (unsigned long)triangles;
			return;
		}
	}
}

static void census_tint(unsigned int base, unsigned int change, unsigned int color)
{
	int k, width, height;

	if (!census_wanted())
		return;
	census_tint_passes++;
	for (k = 0; k < census_tint_count; k++)
		if (census_tints[k].base == base && census_tints[k].change == change && census_tints[k].color == color)
		{
			census_tints[k].passes++;
			return;
		}
	if (census_tint_count == CENSUS_TINTS)
	{
		census_tint_unkept++;
		return;
	}
	census_tints[census_tint_count].base = base;
	census_tints[census_tint_count].change = change;
	census_tints[census_tint_count].color = color;
	census_tints[census_tint_count].passes = 1;
	census_tints[census_tint_count].bytes16 = 0;
	census_tint_count++;
	if (psp_tex_size(base, &width, &height))
	{
		census_tints[census_tint_count - 1].bytes16 = (unsigned long)width * height * 2;
		census_tint_bytes16 += (unsigned long)width * height * 2;
		census_tint_bytes8 += (unsigned long)width * height;
	}
}

static void census_light_surface(const void *vertices, const unsigned short *indices, long triangles)
{
	int k;

	long t;

	census_surface_draws++;
	census_surface_triangles += (unsigned long)triangles;
	for (t = 0; t < triangles; t++)
	{
		const unsigned short *v = indices + 3 * t;
		unsigned int hash = ((unsigned int)vertices * 2654435761u) ^ (v[0] * 40503u) ^ (v[1] * 2246822519u) ^ (v[2] * 3266489917u);
		unsigned int slot, probe;

		hash |= 1; /* 0 is an empty slot */
		for (slot = hash % CENSUS_TRIANGLES, probe = 0; probe < 32; probe++, slot = (slot + 1) % CENSUS_TRIANGLES)
		{
			if (census_triangle_seen[slot] == hash)
			{
				census_surface_repeat_triangles++;
				break;
			}
			if (!census_triangle_seen[slot])
			{
				census_triangle_seen[slot] = hash;
				break;
			}
		}
		if (probe == 32)
			census_triangles_lost++;
	}
	for (k = 0; k < census_surface_count; k++)
		if (census_surfaces[k].vertices == vertices)
		{
			census_surfaces[k].lights++;
			return;
		}
	if (census_surface_count == CENSUS_SURFACES)
	{
		census_surface_lost++;
		return;
	}
	census_surfaces[census_surface_count].vertices = vertices;
	census_surfaces[census_surface_count].indices = indices;
	census_surfaces[census_surface_count].triangles = triangles;
	census_surfaces[census_surface_count].lights = 1;
	census_surface_count++;
}

static void census_light_run(long count, float nearest, float radius_squared)
{
	census_light_run_triangles += (unsigned long)count;
	if (nearest > radius_squared)
		census_light_runs_out += (unsigned long)count;
}

static void census_report(void)
{
	char line[700];
	int length, shown, k, best;
	unsigned long done[CENSUS_NEAR_SLOTS] = { 0 };
	float frames = (float)census_frames;

	if (census_on <= 0 || !census_frames)
		return;
	length = snprintf(line, sizeof(line), "drawcensus: near the eye, a frame over %lu frames, by definition (rigid parts, their triangles; bending triangles):",
		census_frames);
	for (shown = 0; shown < 8 && length < (int)sizeof(line) - 120; shown++)
	{
		unsigned long most = 0;

		best = -1;
		for (k = 0; k < CENSUS_NEAR_SLOTS; k++)
		{
			unsigned long triangles = census_near_table[k].rigid_triangles + census_near_table[k].bending_triangles;

			if (census_near_table[k].used && !done[k] && triangles > most)
			{
				most = triangles;
				best = k;
			}
		}
		if (best < 0)
			break;
		done[best] = 1;
		length += snprintf(line + length, sizeof(line) - length, " %s (%.1f, %.0f; %.0f);",
			tag_get_name(census_near_table[best].definition), census_near_table[best].rigid_parts / frames,
			census_near_table[best].rigid_triangles / frames, census_near_table[best].bending_triangles / frames);
	}
	platform_log("%s", line);
	platform_log("drawcensus: change-colour passes %.1f a frame; %d distinct base, multipurpose map and colour triples since the start (%lu more not kept); their base textures as tinted copies %lu KB at 16 bits, %lu KB at 8",
		census_tint_passes / frames, census_tint_count, census_tint_unkept, census_tint_bytes16 / 1024, census_tint_bytes8 / 1024);
	{
		/* the triples by passes, most first: what a cache of the top N would
		cover, and its size */
		static const int sizes[5] = { 8, 16, 32, 64, 128 };
		static unsigned char taken[CENSUS_TINTS];
		unsigned long total = 0, covered = 0, bytes = 0;
		int n = 0, next = 0;

		for (k = 0; k < census_tint_count; k++)
			total += census_tints[k].passes;
		memset(taken, 0, sizeof(taken));
		length = snprintf(line, sizeof(line), "drawcensus: tint cache, the most drawn triples since the start (of %lu passes):", total);
		while (next < 5 && n < census_tint_count)
		{
			unsigned long most = 0;

			best = -1;
			for (k = 0; k < census_tint_count; k++)
				if (!taken[k] && census_tints[k].passes >= most)
				{
					most = census_tints[k].passes;
					best = k;
				}
			if (best < 0)
				break;
			taken[best] = 1;
			covered += census_tints[best].passes;
			bytes += census_tints[best].bytes16;
			n++;
			if (n == sizes[next] || n == census_tint_count)
			{
				length += snprintf(line + length, sizeof(line) - length, " top %d cover %.0f%% in %lu KB;",
					n, total ? 100.0f * covered / total : 0.0f, bytes / 1024);
				while (next < 5 && sizes[next] <= n)
					next++;
			}
		}
		platform_log("%s", line);
	}
	platform_log("drawcensus: light pass by surface, a frame: %.1f draws on %.1f surfaces (lit by 1 light %.1f, 2 %.1f, 3 %.1f, 4 or more %.1f; %lu draws not kept); %.0f triangles drawn, %.0f of them a triangle another light already drew (%.0f%%; %lu not tracked)",
		census_surface_draws / frames,
		(census_surfaces_lit[0] + census_surfaces_lit[1] + census_surfaces_lit[2] + census_surfaces_lit[3]) / frames,
		census_surfaces_lit[0] / frames, census_surfaces_lit[1] / frames, census_surfaces_lit[2] / frames, census_surfaces_lit[3] / frames,
		census_surface_lost, census_surface_triangles / frames, census_surface_repeat_triangles / frames,
		census_surface_triangles ? 100.0f * census_surface_repeat_triangles / census_surface_triangles : 0.0f, census_triangles_lost);
	census_surface_draws = census_surface_triangles = census_surface_repeat_triangles = census_surface_lost = census_triangles_lost = 0;
	memset(census_surfaces_lit, 0, sizeof(census_surfaces_lit));
	platform_log("drawcensus: light pass %.1f draws a frame (%.1f spotlights, %.1f point lights), %.1f triangles looked at (%.1f of them the spotlights'); %.1f in runs of 16 wholly beyond the light's radius (%.0f%% of the runs' triangles)",
		(census_light_draws[0] + census_light_draws[1]) / frames, census_light_draws[1] / frames, census_light_draws[0] / frames,
		(census_light_looked[0] + census_light_looked[1]) / frames, census_light_looked[1] / frames, census_light_runs_out / frames,
		census_light_run_triangles ? 100.0f * census_light_runs_out / census_light_run_triangles : 0.0f);
	memset(census_near_table, 0, sizeof(census_near_table));
	census_tint_passes = 0;
	census_light_draws[0] = census_light_draws[1] = census_light_looked[0] = census_light_looked[1] = 0;
	census_light_run_triangles = census_light_runs_out = 0;
	census_frames = 0;
}

static void census_frame(void)
{
	int k;

	if (census_on <= 0)
		return;
	census_frames++;
	for (k = 0; k < census_surface_count; k++)
		census_surfaces_lit[census_surfaces[k].lights >= 4 ? 3 : census_surfaces[k].lights - 1]++;
	census_surface_count = 0;
	memset(census_triangle_seen, 0, sizeof(census_triangle_seen));
}
#define CENSUS(call) call
#else
#define CENSUS(call) ((void)0)
#endif

static void __attribute__((noinline, cold)) ge_kinds_report(void)
{
	char line[600];
	int length, k;

	if (!kind_frames)
		return;
	length = snprintf(line, sizeof(line), "GE: display list by kind, KB a frame over %lu frames (fullest frame; draws skipped a frame):",
		kind_frames);
	for (k = 0; k < KINDS && length < (int)sizeof(line) - 60; k++)
		if (kind_bytes[k] || kind_skips[k])
			length += snprintf(line + length, sizeof(line) - length, " %s %.1f (%.1f; %.1f)", kind_names[k],
				kind_bytes[k] / 1024.0 / kind_frames, kind_peak_bytes[k] / 1024.0, (float)kind_skips[k] / kind_frames);
	platform_log("%s", line);
	if (kind_bytes[KIND_MODEL])
	{
		/* bone loads written in the list directly, or once into a sub-list (which list_take counted) */
		double frames_seen = (double)kind_frames;
		double bone = model_bone_loads * 52.0 + model_bone_sub_bytes + model_bone_calls * 8.0;

		platform_log("GE: the models' share, KB a frame: bone matrices %.1f (%.0f loads in the list, %.0f calls of sub-lists), data copied in %.1f (kept indices, cut corners, palettes), the rest %.1f (commands); %.0f part draws a frame",
			bone / 1024.0 / frames_seen, model_bone_loads / frames_seen, model_bone_calls / frames_seen,
			(model_take_bytes - model_bone_sub_bytes) / 1024.0 / frames_seen,
			((double)kind_bytes[KIND_MODEL] - bone - (model_take_bytes - model_bone_sub_bytes)) / 1024.0 / frames_seen,
			model_part_calls / frames_seen);
		platform_log("GE: models left out for room, smallest first: %.1f part draws a frame (the size floor now %.1f px, at most %.1f px since the last report); rigid parts near the eye %.1f a frame, %.1f of them drawn whole with no copy; bending batches near the eye drawn whole %.1f a frame, %.1f of them by their sphere (build 118)%s",
			model_floor_left_out / frames_seen, model_size_floor, model_size_floor_peak, near_parts / frames_seen,
			near_parts_whole / frames_seen, near_batches_whole / frames_seen, near_batches_by_sphere / frames_seen,
			near_sphere_check ? (near_sphere_mismatches ? "; nearspherecheck: SOME THE SPHERE PASSED WERE NOT WHOLE" : "; nearspherecheck: all whole") : "");
		if (near_sphere_check && near_sphere_mismatches)
			platform_log("GE: nearspherecheck: %lu batches the sphere passed were cut or dropped by the corner loop", near_sphere_mismatches);
		/* build 119 */
		platform_log("GE: near runs of %d triangles (%s): %.1f a frame kept whole, %.1f dropped whole, %.1f looked at corner by corner; their spheres %lu KB%s",
			NEAR_CLUSTER_TRIANGLES, near_cluster_full > 0 ? "off: nearclusterfull" : "on", near_cluster_kept / frames_seen,
			near_cluster_dropped / frames_seen, near_cluster_mixed / frames_seen, near_cluster_bytes / 1024,
			near_cluster_check ? (near_cluster_mismatches ? "; nearclustercheck: SOME TRIANGLES DISAGREED WITH THEIR RUN" : "; nearclustercheck: all agreed") : "");
		if (near_cluster_check && near_cluster_mismatches)
			platform_log("GE: nearclustercheck: %lu triangles disagreed with their run's verdict", near_cluster_mismatches);
		if (skin_axis_check)
			platform_log("GE: skinaxischeck: %lu corner sums compared with the loop's, %lu differed", skin_axis_checked, skin_axis_differ);
		platform_log("GE: rigid near runs of %d triangles (%s): %.1f a frame kept whole, %.1f dropped whole, %.1f looked at corner by corner; %lu parts' spheres made (%lu KB)%s",
			NEAR_CLUSTER_TRIANGLES, ge_hot.rigid_runs_full > 0 ? "off: rigidrunsfull" : "on", rigid_runs_kept / frames_seen,
			rigid_runs_dropped / frames_seen, rigid_runs_mixed / frames_seen, rigid_runs_made, rigid_runs_bytes / 1024,
			rigid_runs_check ? (rigid_runs_mismatches ? "; rigidrunscheck: SOME TRIANGLES DISAGREED WITH THEIR RUN" : "; rigidrunscheck: all agreed") : "");
		if (rigid_runs_check && rigid_runs_mismatches)
			platform_log("GE: rigidrunscheck: %lu triangles disagreed with their run's verdict", rigid_runs_mismatches);
	}
	near_cluster_kept = near_cluster_dropped = near_cluster_mixed = near_cluster_mismatches = 0;
	skin_axis_checked = skin_axis_differ = 0;
	rigid_runs_kept = rigid_runs_dropped = rigid_runs_mixed = rigid_runs_mismatches = rigid_runs_made = 0;
	/* build 119's probe */
	if (probes_on)
		platform_log("GE: near corner loop: %lu us a frame (%lu batches, %lu triangles, %lu corners worked out); bone slots %lu us a frame (%lu batches) (over %lu frames)",
		near_loop_us / kind_frames, near_loop_batches / kind_frames, near_loop_triangles / kind_frames, near_corners_worked / kind_frames,
		slot_us / kind_frames, slot_batches / kind_frames, kind_frames);
	near_loop_us = near_loop_batches = near_loop_triangles = near_corners_worked = slot_us = slot_batches = 0;
	model_floor_left_out = near_parts = near_parts_whole = near_batches_whole = 0;
	near_batches_by_sphere = near_sphere_mismatches = 0;
	model_size_floor_peak = model_size_floor;
	model_bone_loads = model_take_bytes = model_part_calls = model_bone_calls = model_bone_sub_bytes = 0;
	platform_log("GE: fullest frame %u KB, kinds in order %s (L level, o other, k sky, l lights, B level see-through, M models, q see-through queued, d decals, p particles, f flares, s shadows, T see-through drawn, Q screen quads, H HUD); the level's reserve %s, held back %lu draws; the decals' and particles' reserve %s, light draws held back %lu",
		kind_peak_total / 1024, kind_peak_order, level_reserve_on > 0 ? "on" : "off", level_reserve_held,
		light_reserve_on > 0 ? "on" : "off", light_reserve_held);
	memset(kind_bytes, 0, sizeof(kind_bytes));
	memset(kind_skips, 0, sizeof(kind_skips));
	kind_frames = 0;
	kind_peak_total = 0;
	level_reserve_held = 0;
	light_reserve_held = 0;
}

/* the limit for the kind running: lower before the level has begun, and
lower for the light pass */
static int kind_limit(void)
{
	if (level_reserve_on > 0 && !level_begun && ge_kind_now != KIND_LEVEL && ge_kind_now != KIND_SKY && !hud_phase)
		return list_limit - LEVEL_RESERVE - (ge_kind_now == KIND_LIGHT && light_reserve_on > 0 ? LIGHT_RESERVE : 0);
	if (ge_kind_now == KIND_LIGHT && light_reserve_on > 0 && !hud_phase)
		return list_limit - LIGHT_RESERVE;
	return list_limit;
}

static int list_full(void)
{
	int limit = kind_limit();

	if (sceGuCheckList() < limit - DISPLAY_LIST_MARGIN)
		return !show3_room(DISPLAY_LIST_MARGIN);
	if (!list_full_skips++)
		platform_log("GE: the display list is full; draws skipped until the frame ends");
	kind_frame_skips[ge_kind_now]++;
	if (limit != list_limit && sceGuCheckList() < list_limit - DISPLAY_LIST_MARGIN)
	{
		if (ge_kind_now == KIND_LIGHT && light_reserve_on > 0)
			light_reserve_held++;
		else
			level_reserve_held++;
	}
	return 1;
}

/* build 40: the first-person weapon's see-through parts (the assault
rifle's ammo display) are the nearest, so sorted far to near they came last,
and a full list skipped exactly them (the display went blank in
sustained fire). They may use a quarter of the HUD's reserve */
static int list_full_first_person(void)
{
	int saved = list_limit, full;

	if (list_limit < list_bytes - HUD_RESERVE * 3 / 4)
		list_limit = list_bytes - HUD_RESERVE * 3 / 4;
	full = list_full();
	list_limit = saved;
	return full;
}

/* room in the display list for a draw's own data, or NULL: a large draw
(a light over many triangles) can need more than the margin above, and
writing past the list's end corrupts memory (build 29) */
int psp_tex_bind_lightmap(unsigned int key);
int psp_keep(const char *name);

/* build 48: the display list is written through the uncached view (pspgu's
sceGuStart sets bit 30 of its address), so every word the CPU puts there
is an uncached store over the bus, and sceGuGetMemory's space too: the
vertices and indices a draw copies there (cut triangles, kept indices,
decals, lights, shadows). A region of LIST_CACHED_MINIMUM bytes or more now
comes back as the cached view instead, on whole cache lines of its own (64
bytes of slack each side, so no line is shared with a command word written
uncached), each line made ours with "cache 0x18" (create dirty exclusive:
no read from memory; halo_bench v9 measured half the cost for whole lines).
The lines are written back and dropped from the cache ("cache 0x1B") before
the GE may read them: at the next stall address update, which every draw
makes (see __wrap_sceGeListUpdateStallAddr). Keep word "listuncached": as
before */
#define LIST_CACHED_MINIMUM 256
static unsigned int pending_start = 0, pending_end = 0;
static int list_cached = -1;
static unsigned long cached_takes = 0, cached_bytes = 0, cached_lines_flushed = 0;

static void list_flush_pending(void)
{
	unsigned int line;

	if (pending_end <= pending_start)
		return;
	for (line = pending_start; line < pending_end; line += 64)
		__builtin_allegrex_cache(0x1B, line);
	__asm__ volatile("sync");
	cached_lines_flushed += (pending_end - pending_start) / 64;
	pending_start = pending_end = 0;
}

/* build 114: "listcachedall" (lever F; optimization_ranking.md X18): the
command words through the cache too. pspgu writes every command word through
the list's uncached view (sceGuStart sets bit 30), one bus store each:
b113's Test 3 shows 880-980 uncached stores per million cycles while
drawing, and the PSP's level passes cost 175 us a draw against the
emulator's 18 (engine_performance.md), a gap only the PSP's bus explains
[U]. The GE takes only an address's low bits, so the list can be written
through the cached view as long as every line is in memory before the GE
may read it: each time the stall address is handed on (the two places that
call the kernel), the lines from the last hand-over to the new stall
address are written back and dropped ("cache 0x1B", as build 48's regions).
Off with gemeon (the ME writes the list, through its own cache), with
benchrun's list in VRAM, and with "listuncached". Exact: the same words
reach the GE. The emulator can't check the write-back (it has no caches):
only the PSP shows whether a line was missed (a GE hang or a garbled
frame) */
struct gu_list_view { unsigned int *start, *current; int parent_context; };
extern struct gu_list_view *gu_list; /* pspgu's (guInternal.h: GuDisplayList) */

/* geshow3: room for needed more bytes where this frame's list is. The list
limits are the whole list's, as without geshow3, so the same draws are
skipped for them; a list in a part beside the frame out that outgrows the
part waits for the GE's finish of that frame (its list is then free) and
grows, upward into the rest of the list, or by a jump to the list's start.
sceGuCheckList goes on counting the whole frame's bytes. 0: no room even so
(a draw skipped, counted; never expected) */
static int show3_room(unsigned int needed)
{
	unsigned int view = (unsigned int)gu_list->current & 0x40000000u;
	unsigned int at = (unsigned int)gu_list->current & ~0x40000000u, used, waited = 0, mark;

	if (!show3_part_end || at + needed + 16 <= show3_part_end)
		return 1;
	if (show3_part_grow == SHOW3_GROW_NONE)
	{
		show3_room_skips++;
		return 0;
	}
	/* the finish never reported counts as no room: the GE may still read it */
	mark = replay_clock(psp_on_me());
	while (SHOW3_GE_WORD[0] != SHOW3_GE_WORD[1])
	{
		if (psp_on_me())
		{
			inflight_serve();
			waited = replay_clock(1) - mark > 0x4000000u; /* ~0.4 s of the ME's count */
		}
		else
		{
			sceKernelDelayThread(200);
			waited = replay_clock(0) - mark > 400000u;
		}
		if (waited)
		{
			show3_part_grow = SHOW3_GROW_NONE;
			show3_room_skips++;
			return 0;
		}
	}
	if (show3_part_grow == SHOW3_GROW_UP)
	{
		show3_grown++;
		show3_part_end = show3_part_next;
	}
	else
	{
		/* BASE then JUMP, as sceGuGetMemory's jump over its region; the GE
		reaches it only once a later stall address is handed on */
		unsigned int *word = gu_list->current;
		unsigned int base = show3_part_base;

		used = (unsigned int)sceGuCheckList() + 8;
		word[0] = (16u << 24) | ((base >> 8) & 0xF0000u);
		word[1] = (8u << 24) | (base & 0xFFFFFFu);
		gu_list->current = (unsigned int *)(view | base);
		gu_list->start = (unsigned int *)((view | base) - used);
		show3_jumped++;
		show3_part_jumped = 1;
		show3_part_end = show3_part_next;
	}
	show3_part_grow = SHOW3_GROW_NONE;
	at = (unsigned int)gu_list->current & ~0x40000000u;
	if (at + needed + 16 <= show3_part_end)
		return 1;
	show3_room_skips++;
	return 0;
}
static int list_all_cached = -1, list_cached_frame = 0;
static unsigned int list_back_from = 0;
/* while sceGuGetMemory runs: the region it hands out, not filled yet (its
own stall update may be handed to the GE); 0 otherwise */
static unsigned int list_filling_from = 0;
static unsigned long list_lines_back = 0;

/* right after the frame's sceGuStart */
static void list_cached_frame_start(int in_vram)
{
	if (list_all_cached < 0)
		list_all_cached = psp_keep("listcachedall") && !psp_keep("gemeon") && !psp_keep("listuncached");
	list_cached_frame = list_all_cached && !in_vram;
	if (!list_cached_frame)
		return;
	/* sceGuStart wrote its first words through the uncached view, into the
	line the next cached store reads in: they reach RAM first */
	__asm__ volatile("sync");
	gu_list->start = (unsigned int *)((unsigned int)gu_list->start & ~0x40000000u);
	gu_list->current = (unsigned int *)((unsigned int)gu_list->current & ~0x40000000u);
	list_back_from = (unsigned int)gu_list->current & ~63u;
}

/* before the GE is told it may read up to stall */
static void list_write_back(const void *stall)
{
	unsigned int end = (unsigned int)stall & ~0x40000000u, line;

	/* inside sceGuGetMemory: only up to its region (the JUMP over it is
	written; the region is filled after, and joins the pending range) */
	if (list_filling_from && list_filling_from < end)
		end = list_filling_from;
	if (!list_cached_frame || end <= list_back_from)
		return;
	for (line = list_back_from; line < end; line += 64)
		__builtin_allegrex_cache(0x1B, line);
	__asm__ volatile("sync");
	list_lines_back += (end - list_back_from + 63) / 64;
	/* the line holding the stall address may get more words */
	list_back_from = end & ~63u;
}

/* listcachedall: a region taken with sceGuGetMemory is filled after that
call moved the stall address, possibly past the region and handed on, so
list_write_back alone would never write it. Its lines join build 48's
pending range instead, written back at the next hand-over, before the GE
can reach the draw that reads it (that draw comes after the region).
sceGuGetMemory is wrapped (psp_build.py links it with --wrap), so every
region is covered, pspgu's own too (sceGuClear fills one itself) */
static void list_region_pending(const void *taken, int bytes)
{
	unsigned int start, end;

	if (!list_cached_frame || !taken)
		return;
	start = ((unsigned int)taken & ~0x40000000u) & ~63u;
	end = (((unsigned int)taken & ~0x40000000u) + bytes + 63) & ~63u;
	if (pending_end <= pending_start || start < pending_start)
		pending_start = start;
	if (end > pending_end)
		pending_end = end;
}

void *__real_sceGuGetMemory(int size);
void *__wrap_sceGuGetMemory(int size)
{
	void *taken;

	if (!list_cached_frame)
		return __real_sceGuGetMemory(size);
	/* the region starts two words past the list's current end */
	list_filling_from = ((unsigned int)gu_list->current & ~0x40000000u) + 8;
	taken = __real_sceGuGetMemory(size);
	list_filling_from = 0;
	list_region_pending(taken, size);
	return taken;
}

static void *list_take(int bytes)
{
	void *taken;

	if (sceGuCheckList() + bytes + 2048 + 128 > kind_limit())
	{
		if (!list_full_skips++)
			platform_log("GE: the display list is full (%d more bytes wanted); draws skipped until the frame ends", bytes);
		kind_frame_skips[ge_kind_now]++;
		return NULL;
	}
	if (!show3_room((unsigned int)bytes + 2048 + 128))
		return NULL;
	if (ge_kind_now == KIND_MODEL)
		model_take_bytes += bytes;
	if (list_cached < 0)
		list_cached = !psp_keep("listuncached");
	if (!list_cached || bytes < LIST_CACHED_MINIMUM)
		return sceGuGetMemory(bytes); /* (build 114: listcachedall's pending range, in the wrapper) */
	taken = sceGuGetMemory(bytes + 128);
	if (!taken)
		return NULL;
	{
		/* the cached view of the whole lines inside the region */
		unsigned int start = (((unsigned int)taken & ~0x40000000u) + 63) & ~63u, end = (start + bytes + 63) & ~63u, line;

		for (line = start; line < end; line += 64)
			__builtin_allegrex_cache(0x18, line);
		if (pending_end <= pending_start)
			pending_start = start;
		pending_end = end;
		cached_takes++;
		cached_bytes += bytes;
		return (void *)start;
	}
}

/* every pspgu draw (and sceGuGetMemory, sceGuFinish) moves the GE's stall
address with this system call, which stores the GE's register and waits
("sync") for the CPU's write buffer to drain (uofw kd/ge/stall.S). Build
48: the cached list regions are written back first, and only every
STALL_BATCH-th update is passed on (the GE then runs up to 16 draws behind
the CPU instead of one; it idles most of the frame anyway). sceGuFinish's
always goes through (stall_force, psp_ge_frame_end). Keep word "stalleach":
every update passed on, as before */
#define STALL_BATCH 16
int __real_sceGeListUpdateStallAddr(int qid, void *stall);
static int stall_batch = -1, stall_pending = 0;
static PSP_MAIN_SIDE int stall_force = 0;
static unsigned long stall_calls = 0, stall_forwarded = 0, stall_timed = 0, stall_timed_us = 0;

/* build 79: "geserial" (and the debug menu): no stall update reaches the GE
until the frame's list is finished, so the GE starts when the CPU is done
and the frame end's wait for it is the GE's whole busy time. A measurement,
not a way to play: the two no longer overlap */
static int ge_serial = -1;

/* build 113: "gebusy": how long the GE works, timed by the GE itself
(b113_candidates.md, instruments). We log the main CPU's time and the ME's,
but the GE only through the frame end's wait for it. Each stall update
passed on (a chunk of up to STALL_BATCH draws) now ends with a SIGNAL
command; when the GE reaches it, it raises an interrupt and gebusy_signal
notes the time. A chunk's busy time runs from when the GE could start it
(handed over, or the chunk before finished, whichever is later) to then; the
frame's last chunk ends at the list's FINISH (gebusy_finish). Summed over
the heartbeat's window for the frameend: line. An interrupt per 16 draws,
a few microseconds each. Not while the ME builds the lists (gemeon): it
passes its stall updates on later. The emulator doesn't time the GE: only
the PSP's number means anything */
#define GEBUSY_SLOTS 256
static int gebusy_on = -1, gebusy_inserting = 0;
static void *gebusy_stall;
static PSP_MAIN_SIDE unsigned int gebusy_next = 0;
static PSP_MAIN_SIDE volatile unsigned int gebusy_handed[GEBUSY_SLOTS], gebusy_last_reached = 0, gebusy_finish_handed = 0;
static PSP_MAIN_SIDE volatile unsigned long gebusy_us = 0, gebusy_chunks = 0, gebusy_finishes = 0;
static PSP_MAIN_SIDE unsigned int gebusy_window_start = 0;
/* build 115: the GE's time by the kind of draw it ran (ge_kind_now's kinds):
each chunk is one kind's (a change of kind ends the chunk: gebusy_cut), so its
busy time goes to that kind. The emulator doesn't time the GE: PSP only */
static PSP_MAIN_SIDE volatile unsigned char gebusy_kind[GEBUSY_SLOTS];
static PSP_MAIN_SIDE volatile unsigned char gebusy_finish_kind = 0;
static PSP_MAIN_SIDE volatile unsigned long gebusy_kind_us[KINDS];
static int gebusy_cut = 0;

/* "gebusy2" (gebusy with the Media Engine's lists): when the ME replays the
frame (gemeon, geinflight), its stall updates pass the wrapper below as the
ME's work and reach the GE later, from the main CPU, so gebusy's SIGNALs
timed only what the main CPU kicks itself: the list's finish. Here the ME
ends each chunk it publishes with a SIGNAL too. The ME reads no clock: every
time is the main CPU's. Whoever passes an ME stall address to the GE notes
the time for each chunk that address covers (gebusy_me_handed), and the
SIGNAL's interrupt closes the chunk as gebusy's do. The SIGNAL's id carries
the chunk's slot and kind, so the ME writes nothing the main CPU reads from
its cache: each chunk's stall address and the count go to an uncached board.
Also kept: the GE's idle time inside a list (it had reached the stall and
waited for more), and its busy time while the main CPU waited (for the ME's
job, or at the frame's end for the GE) */
#define GEBUSY_ME_ID 0x8000
static int gebusy_me_on = 0, me_tail_on = 0;
static unsigned int gebusy_me_next = 0; /* the ME's (or its stand-in's) */
static volatile unsigned int gebusy_me_board_word[GEBUSY_SLOTS + 16] __attribute__((aligned(64)));
#define GEBUSY_ME_BOARD ((volatile unsigned int *)(0x40000000u | (unsigned int)gebusy_me_board_word))
static PSP_MAIN_SIDE volatile unsigned int gebusy_me_handed_at[GEBUSY_SLOTS];
static PSP_MAIN_SIDE unsigned int gebusy_me_marked = 0;
static PSP_MAIN_SIDE volatile unsigned long gebusy_me_chunks = 0, gebusy_idle_us = 0, gebusy_waited_us[2];
static PSP_MAIN_SIDE volatile unsigned int gebusy_in_list = 0;
static PSP_MAIN_SIDE volatile unsigned int gebusy_wait_open = 0, gebusy_wait_which = 0, gebusy_wait_from = 0;
static PSP_MAIN_SIDE volatile unsigned int gebusy_waited_which = 0, gebusy_waited_from = 0, gebusy_waited_to = 0;

/* how much of [a, b] lies in [c, d] (the clock wraps: differences only) */
static unsigned int gebusy_span(unsigned int a, unsigned int b, unsigned int c, unsigned int d)
{
	unsigned int from = (int)(c - a) > 0 ? c : a, to = (int)(d - b) < 0 ? d : b;

	return (int)(to - from) > 0 ? to - from : 0;
}

static void gebusy_add(unsigned int start, unsigned int now, int kind)
{
	if ((int)(gebusy_last_reached - start) > 0)
		start = gebusy_last_reached;
	else if (gebusy_me_on && gebusy_in_list)
		gebusy_idle_us += start - gebusy_last_reached;
	if ((int)(now - start) > 0)
	{
		/* "lanetrace" (psp_lanetrace.c): the span on the main CPU's clock */
		PSP_LANE(PSP_LANE_GE, start, now, (unsigned int)kind, 0);
		gebusy_us += now - start;
		if (kind >= 0 && kind < KINDS)
			gebusy_kind_us[kind] += now - start;
		if (gebusy_me_on)
		{
			if (gebusy_waited_to)
				gebusy_waited_us[gebusy_waited_which] += gebusy_span(start, now, gebusy_waited_from, gebusy_waited_to);
			if (gebusy_wait_open)
				gebusy_waited_us[gebusy_wait_which] += gebusy_span(start, now, gebusy_wait_from, now);
		}
	}
	gebusy_last_reached = now;
}

/* the main CPU passes an ME stall address on: each chunk the ME has
published up to it can start now. Before the call that passes it, so the
GE can't reach a SIGNAL whose slot still holds an older time. Both the
forwarding thread and the game thread pass addresses on: interrupts off
keeps one from cutting into the other. Within a frame the list only grows,
and a frame's last address covers all of its chunks before the next frame
publishes any, so the address order is the chunks' order */
static void gebusy_me_handed(unsigned int stall)
{
	unsigned int flags, now, count;

	if (!gebusy_me_on)
		return;
	flags = sceKernelCpuSuspendIntr();
	now = sceKernelGetSystemTimeLow();
	count = GEBUSY_ME_BOARD[GEBUSY_SLOTS];
	if (count - gebusy_me_marked > GEBUSY_SLOTS)
		gebusy_me_marked = count - GEBUSY_SLOTS;
	while (gebusy_me_marked != count && GEBUSY_ME_BOARD[gebusy_me_marked & (GEBUSY_SLOTS - 1)] <= stall)
		gebusy_me_handed_at[gebusy_me_marked++ & (GEBUSY_SLOTS - 1)] = now;
	sceKernelCpuResumeIntr(flags);
}

/* the main CPU starts (1) or ends (0) a wait: which 0 for the ME's job, 1
for the GE at the frame's end */
static void gebusy_wait(int begin, int which)
{
	unsigned int flags, now;

	if (!gebusy_me_on)
		return;
	flags = sceKernelCpuSuspendIntr();
	now = sceKernelGetSystemTimeLow();
	if (begin)
	{
		gebusy_wait_which = (unsigned int)which;
		gebusy_wait_from = now;
		gebusy_wait_open = 1;
	}
	else if (gebusy_wait_open)
	{
		gebusy_waited_which = gebusy_wait_which;
		gebusy_waited_from = gebusy_wait_from;
		gebusy_waited_to = now;
		gebusy_wait_open = 0;
	}
	sceKernelCpuResumeIntr(flags);
}

/* on the ME (or its stand-in): the chunk being published ends with a
SIGNAL. Its id: GEBUSY_ME_ID, the kind, the slot */
static void *gebusy_me_signal(void *stall)
{
	unsigned int slot = gebusy_me_next & (GEBUSY_SLOTS - 1);

	gebusy_inserting = 1;
	gebusy_stall = stall;
	sceGuSignal(GU_SIGNAL_NOWAIT, (int)(GEBUSY_ME_ID | ((unsigned int)ge_kind_now & 0x7F) << 8 | slot));
	gebusy_inserting = 0;
	stall = gebusy_stall;
	GEBUSY_ME_BOARD[slot] = (unsigned int)stall;
	gebusy_me_next++;
	__asm__ __volatile__("sync");
	GEBUSY_ME_BOARD[GEBUSY_SLOTS] = gebusy_me_next;
	return stall;
}

/* the frame's end on the ME (geinflight): the draws since the last stall
update sit unpublished until the main CPU's finish, which runs only after
the next frame's ticks. gebusy2 publishes them here, as a chunk of their
own; "metail" publishes them here without the instrument */
static void gebusy_me_tail(void)
{
	if ((!gebusy_me_on && !me_tail_on) || !stall_pending)
		return;
	gebusy_cut = 1;
	sceGuGetMemory(4);
	gebusy_cut = 0;
}

/* the GE's interrupts (its callbacks: interrupt context, so only the
clock and a few sums) */
static void gebusy_signal(int id)
{
	if (id & GEBUSY_ME_ID)
	{
		gebusy_add(gebusy_me_handed_at[id & (GEBUSY_SLOTS - 1)], sceKernelGetSystemTimeLow(), (id >> 8) & 0x7F);
		gebusy_me_chunks++;
	}
	else
		gebusy_add(gebusy_handed[id & (GEBUSY_SLOTS - 1)], sceKernelGetSystemTimeLow(), gebusy_kind[id & (GEBUSY_SLOTS - 1)]);
	gebusy_chunks++;
	gebusy_in_list = 1;
}

static void gebusy_finish(int id)
{
	(void)id;
	/* the wait for the finish isn't idle time in the list: the GE had every
	draw, and only the list's end was still to come */
	gebusy_in_list = 0;
	gebusy_add(gebusy_finish_handed, sceKernelGetSystemTimeLow(), gebusy_finish_kind);
	gebusy_finishes++;
}

/* geshow3: the GE reached a list's finish (a shown frame's carries its id) */
static void show3_finish_callback(int id)
{
	if (gebusy_on > 0)
		gebusy_finish(id);
	if (id)
	{
		SHOW3_GE_WORD[0] = (unsigned int)id;
		show3_finished = (unsigned int)id;
		sceKernelSetEventFlag(show3_flag, 1);
	}
}

/* gebusy2's own GE callbacks, given to every list in place of pspgu's.
pspgu's signal callback notes each signal in its settings (a history and its
count), and those words share a cache line with pspgu's list context, whose
write position the Media Engine moves as it builds the frame. A SIGNAL from
the ME's chunk interrupts the main CPU while the ME works, so the main CPU's
cache took that line, with the position as RAM held it then, and marked it
dirty. Writing the main CPU's cache back after the ME's frame then put that
old position back over the ME's, and the frame's finish went to the wrong
place in the list. The GE never reached it, and the game waited for the GE
for ever. These touch only the main CPU's own words (PSP_MAIN_SIDE) */
static PSP_MAIN_SIDE int gebusy_cbid = -1;

static void gebusy_ge_signal(int id, void *arg)
{
	(void)arg;
	gebusy_signal(id & 0xFFFF);
}

static void gebusy_ge_finish(int id, void *arg)
{
	(void)arg;
	/* pspgu's finish callback isn't called on these lists, so geshow3's
	finish id comes this way too (it counts gebusy's finish itself) */
	if (show3_on > 0)
		show3_finish_callback(id & 0xFFFF);
	else
		gebusy_finish(id & 0xFFFF);
}

/* sceGeListEnQueue's wrapper (psp_trace.c): the callbacks a list gets */
int psp_ge_list_callback(int cbid)
{
	return gebusy_cbid >= 0 ? gebusy_cbid : cbid;
}

static void gebusy_start(void)
{
	ge_reads_on = psp_keep("gereads") ? 1 : 0; /* build 115 */
	if (ge_reads_on)
		platform_log("GE: gereads on (what each draw makes the GE read, by where it lives: a report every 300 frames)");
	probes_on = psp_keep("probes") ? 1 : 0; /* build 119 */
	if (probes_on)
		platform_log("GE: probes on (the near corner loop, the level's passes, the light draw and the decal calls split by piece)");
	me_tail_on = psp_keep("metail") ? 1 : 0;
	if (me_tail_on)
		platform_log("GE: metail on (the Media Engine publishes the frame's last draws to the GE at its frame's end)");
	gebusy_me_on = psp_keep("gebusy2") ? 1 : 0;
	gebusy_on = psp_keep("gebusy") || gebusy_me_on;
	if (!gebusy_on)
		return;
	if (gebusy_me_on)
	{
		PspGeCallbackData callbacks = { gebusy_ge_signal, NULL, gebusy_ge_finish, NULL };

		gebusy_cbid = sceGeSetCallback(&callbacks);
		if (gebusy_cbid < 0)
		{
			platform_log("GE: gebusy2 off: no GE callbacks of its own (%08x); gebusy alone", (unsigned int)gebusy_cbid);
			gebusy_cbid = -1;
			gebusy_me_on = 0;
		}
	}
	if (gebusy_me_on)
		platform_log("GE: gebusy2 on (the Media Engine's chunks end with a SIGNAL too: the GE's busy time whoever kicks the list; a gebusy2: line after each frameend: line)");
	else
	{
		sceGuSetCallback(GU_CALLBACK_SIGNAL, gebusy_signal);
		sceGuSetCallback(GU_CALLBACK_FINISH, gebusy_finish);
	}
	gebusy_window_start = sceKernelGetSystemTimeLow();
	platform_log("GE: gebusy on (a SIGNAL every %d draws; the GE's busy time in the frameend: line)", STALL_BATCH);
}

/* the window's part of the frameend: line, then a new window */
static void gebusy_window(char *text, int size, unsigned long frames_in_window)
{
	unsigned int now = sceKernelGetSystemTimeLow(), wall = now - gebusy_window_start;
	unsigned long n = frames_in_window ? frames_in_window : 1;

	if (gebusy_on <= 0 || size <= 0)
		return;
	snprintf(text, size, "; GE busy %.1f ms a frame, %.1f%% of the clock (%lu chunks, %lu finishes)%s",
		gebusy_us / 1000.0 / n, wall ? 100.0 * gebusy_us / wall : 0.0, gebusy_chunks, gebusy_finishes,
		gebusy_me_on ? "" : " (the ME's lists not timed: gebusy2)");
	if (gebusy_me_on)
	{
		platform_log("gebusy2: the ME's chunks %.1f a frame, of %.1f; the GE idle in a list %.1f ms a frame; "
			"busy while the main CPU waited for the ME %.1f, for the GE %.1f ms a frame",
			(double)gebusy_me_chunks / n, (double)gebusy_chunks / n, gebusy_idle_us / 1000.0 / n,
			gebusy_waited_us[0] / 1000.0 / n, gebusy_waited_us[1] / 1000.0 / n);
		gebusy_me_chunks = gebusy_idle_us = 0;
		gebusy_waited_us[0] = gebusy_waited_us[1] = 0;
	}
	{
		/* build 115: the same time by kind, ms a frame (a line of its own) */
		char line[420];
		int k, at = snprintf(line, sizeof(line), "gebusy by kind, ms a frame:");

		for (k = 0; k < KINDS && at < (int)sizeof(line) - 1; k++)
			if (gebusy_kind_us[k] >= 100)
				at += snprintf(line + at, sizeof(line) - at, " %s %.1f;", kind_names[k], gebusy_kind_us[k] / 1000.0 / n);
		if (gebusy_us)
			platform_log("%s", line);
		memset((void *)gebusy_kind_us, 0, sizeof(gebusy_kind_us));
	}
	gebusy_us = gebusy_chunks = gebusy_finishes = 0;
	gebusy_window_start = now;
}

int __wrap_sceGeListUpdateStallAddr(int qid, void *stall)
{
	/* build 113: gebusy's own SIGNAL (below) moves the stall address; it
	comes back here, and the new address is passed on with the chunk */
	if (gebusy_inserting)
	{
		gebusy_stall = stall;
		return 0;
	}
	stall_calls++;
	if (stall_batch < 0)
		stall_batch = !psp_keep("stalleach");
	if (ge_serial < 0)
		ge_serial = psp_keep("geserial");
	if (ge_serial && !stall_force)
		return 0;
	if (stall_batch && !stall_force && !gebusy_cut && ++stall_pending < STALL_BATCH)
		return 0;
	stall_pending = 0;
	list_flush_pending();
	if (PSP_ME_WORK())
	{
		if (gebusy_me_on && !stall_force)
			stall = gebusy_me_signal(stall);
		stall_me_qid = qid;
		stall_me_address = stall;
		/* geinflight on the Media Engine: the list's lines to RAM, then the
		address published for the main CPU's forwarding thread */
		if (psp_on_me() && replay_inflight)
		{
			list_write_back(stall);
			INFLIGHT_STALL[1] = (unsigned int)qid;
			__asm__ __volatile__("sync");
			INFLIGHT_STALL[0] = (unsigned int)stall;
		}
		return 0;
	}
	if (gebusy_on > 0)
	{
		if (stall_force)
		{
			gebusy_finish_handed = sceKernelGetSystemTimeLow();
			gebusy_finish_kind = (unsigned char)ge_kind_now;
		}
		else
		{
			/* the chunk ends with a SIGNAL: the GE reports when it gets there */
			unsigned int slot = gebusy_next++ & (GEBUSY_SLOTS - 1);

			gebusy_kind[slot] = (unsigned char)ge_kind_now; /* build 115 */

			gebusy_inserting = 1;
			gebusy_stall = stall;
			sceGuSignal(GU_SIGNAL_NOWAIT, (int)slot);
			gebusy_inserting = 0;
			stall = gebusy_stall;
			gebusy_handed[slot] = sceKernelGetSystemTimeLow();
		}
	}
	if (stall_force)
		gebusy_me_handed(~0u); /* a finish covers every chunk the ME published */
	stall_forwarded++;
	list_write_back(stall); /* build 114: listcachedall */
	if ((stall_forwarded & 63) == 0)
	{
		/* now and then, what the call itself costs */
		unsigned int t0 = ge_clock();
		int result = ge_stall_pass(qid, stall);

		stall_timed_us += ge_clock() - t0;
		stall_timed++;
		return result;
	}
	return ge_stall_pass(qid, stall);
}
/* build 115: under gebusy, a change of kind hands the draws so far on as a
chunk of the old kind (a stall update with its SIGNAL; sceGuGetMemory makes
pspgu update the stall address), so no chunk mixes kinds */
static void gebusy_kind_cut(int kind)
{
	if (gebusy_on <= 0 || kind == ge_kind_now || !stall_pending || (PSP_ME_WORK() && !gebusy_me_on))
		return;
	gebusy_cut = 1;
	sceGuGetMemory(4);
	gebusy_cut = 0;
}
#define SKY_COLOR 0xFFD8B080 /* ABGR: a pale blue */
#define FOG_NEAR 8.0f
#define FOG_FAR 160.0f
/* models are drawn unlit for now: their texture times this grey */
#define MODEL_LIGHT 0xFFB4B4B4

/* build 105: the display list (384 KB) is no longer part of the program's
zeroed data: the heap is sized to fill whatever the program leaves below the
game state, so those 384 KB went to the heap. The list itself is taken when
the GE starts, from the free space left in user RAM (b104 run 1: 552 KB free
in play) when a block that large is free there, else from the heap, which
leaves things as they were. The emulator shows why it waits for the GE: taken
first thing at start-up, the block landed where the tag window is placed
later (at a fixed address), and the map couldn't load. In play the emulator
had 576 KB free but no block over 308 KB, so there it comes from the heap;
the log says which, and how the free space lies, on the PSP */
static unsigned int *display_list = NULL;
static const char *display_list_where = "not yet";
static int display_list_free_kb = 0, display_list_largest_kb = 0;

/* at least this much stays free outside the heap for what is made later
(threads' stacks, the blackbox's buffer: b104 took 32 KB more after load) */
#define DISPLAY_LIST_LEAVES_FREE (128 * 1024)

static void transparent_table_acquire(void); /* build 114: the see-through table, with the list */

static void display_list_acquire(void)
{
	SceUID block = -1;

	if (display_list)
		return;
	if (psp_keep("list512"))
		display_list_size = DISPLAY_LIST_BYTES;
	if ((int)sceKernelTotalFreeMemSize() >= display_list_size + 64 + DISPLAY_LIST_LEAVES_FREE)
		block = sceKernelAllocPartitionMemory(2, "halo display list", PSP_SMEM_High, display_list_size + 64, NULL);
	if (block >= 0)
	{
		display_list = (unsigned int *)(((unsigned int)sceKernelGetBlockHeadAddr(block) + 63) & ~63u);
		display_list_where = "free user RAM outside the heap";
	}
	else
	{
		display_list = memalign(64, display_list_size);
		display_list_where = display_list ? "the heap (no room for it outside)" : "nowhere: no memory";
	}
	display_list_free_kb = sceKernelTotalFreeMemSize() / 1024;
	display_list_largest_kb = sceKernelMaxFreeMemSize() / 1024;
	/* build 114: no line of the block left in the cache from earlier use
	(listcachedall writes the list through the cache; a stale dirty line
	written back later would land over the GE's words) */
	if (display_list)
		sceKernelDcacheWritebackInvalidateRange(display_list, display_list_size);
	transparent_table_acquire();
}

static void display_list_report(void)
{
	platform_log("GE: display list %d KB at %08x, from %s; then %d KB free in user RAM (largest block %d KB), now %d KB (largest %d KB)",
		display_list_size / 1024, (unsigned int)display_list, display_list_where, display_list_free_kb,
		display_list_largest_kb, sceKernelTotalFreeMemSize() / 1024, sceKernelMaxFreeMemSize() / 1024);
}
static int ge_ready = 0, frame_open = 0;
/* the boot screen has the GE (psp_ge_boot_start, with the loading screen
below); frozen: a crash report is on the screen, and it draws no more */
static int boot_up = 0;
static volatile int boot_frozen = 0;
/* the buffer being drawn (relative to VRAM), swapped each frame */
static void *draw_buffer = (void *)0;
static unsigned long frames = 0, draws_this_frame = 0, triangles_this_frame = 0;

/* the frame in hand, for log lines that name one (phasemark, psp_platform.c) */
unsigned long psp_ge_frame_number(void)
{
	return frames;
}
static unsigned long clipped_this_frame = 0, culled_this_frame = 0, model_draws_this_frame = 0;

/* the camera, for the CPU's checks: position, and its right, up and forward
directions in the world */
static float camera_right[3], camera_up[3], camera_forward[3];
static float guard_x, guard_y;
/* the screen's side planes as the guard planes are written (x = screen_x z),
with a margin of SCREEN_MARGIN_PIXELS for the GE's rounding and the TAA's
jitter: a triangle or cut piece wholly past one of them should draw no pixel.
by default they are left out (clip_triangle; "cutlessoff" keeps them) */
#define SCREEN_MARGIN_PIXELS 8.0f
static float screen_x = 1.0f, screen_y = 1.0f;
static int cut_less = 0, guard_wide = 0;
/* recon, "cutlessmaskN": off-screen triangles and pieces
left out only for the kinds whose bits are set in N (1 << CUT_LEVEL ...) */
static int cut_less_mask = -1;
/* build 118: the lengths of the guard band's side planes' normals as
outside() writes them (x = guard_x z has (1, 0, -guard_x)), set with
guard_x and guard_y, so a sphere can be tested against those planes
without a square root each time (model_sphere_inside) */
static float guard_x_length = 1.0f, guard_y_length = 1.0f;
/* build 36: the game's own near plane (0.0625 world units on Blood Gulch),
set each frame; it was a fixed 0.25, and the player stands about 0.2 from a
wall he walks into, so the wall was cut away and the view went through it */
static float near_clip = 0.25f;
/* build 38: the first-person weapon has its own near plane (the game's
0.0117 world units against the world's 0.0625, as the Xbox's
rasterizer_set_frustum_z) and the nearest slice of the depth buffer, so it
is never cut off by the near plane nor sinks into a wall in front of it */
#define WORLD_DEPTH_NEAR 61439
#define FIRST_PERSON_DEPTH_FAR 61440
static int model_first_person = 0, model_near = 0;
/* build 118: the model's bounding sphere (the object's, which the game
culls it by) in camera space, for transparent_cut's fast path; set by
psp_ge_model_shape, radius 0 when there is none (the sky, a flag, the
first-person weapon, after the model ends or a new window begins) */
static float model_sphere[3], model_sphere_radius = 0.0f;
static unsigned long first_person_flushes = 0;
/* build 118: frames by how long they were shown (psp_ge_frame_end) */
static PSP_MAIN_SIDE unsigned int swap_last = 0, swap_longest = 0;
static PSP_MAIN_SIDE unsigned long swap_shown[5];
static float projection_fov = 1.2f, projection_near = 0.0625f, projection_far = 1000.0f;
static float first_person_near = 0.0117f, first_person_far = 1024.0f;
static unsigned long near_models = 0, near_triangles_dropped = 0;
/* the GE's front face for the Xbox's D3DCULL_CCW (build 35) */
#define LEVEL_FRONT GU_CW
#define NEAR_CLIP near_clip
/* how far off the screen a corner may land (the guard planes): 6 screen
widths and heights, 1440 pixels across and 816 up and down. The GE takes
screen positions within 2048 pixels of the screen's centre (its 4096-pixel
drawing space, the screen in its middle: ge_initialize's offset); past that a
corner wraps round. "guardwide" puts the planes at GUARD_PIXELS on both axes
(128 pixels of margin), so fewer triangles are cut, but not pixel for pixel:
a triangle drawn whole instead of as pieces rounds its colours and edges a
little differently (docs/review_cutstream.md), so it is the maintainer's call */
#define GUARD 6.0f
#define GUARD_PIXELS 1920.0f

/* build 37: CPU time in the renderer's entry points, by kind, for the log */
#define GE_ENTRIES 9
static unsigned long ge_entry_us[GE_ENTRIES];
/* build 47: inside the two biggest entries (level draws and model parts,
5 ms a frame each in b46), and decals per call. Each mark is a
sceKernelGetSystemTimeLow call; their own cost is measured once at the
first report (one timer reading) so the parts can be read net of it */
#define GE_PARTS 11
/* build 53: 0 all passes; 1 base only; 2 base + lightmap; 3 lightmap on white;
4 base + detail maps (psp_sdl.c cycles it with select + left under "passes") */
volatile int psp_pass_mode = 0;
int psp_keep(const char *name);
/* build 47, diagnostics for the dark lines on surfaces seen edge-on
(b46): "clearmagenta" clears to magenta and leaves out the sky, so a crack
between triangles shows magenta; "nomip" draws every texture at its full
size, so a line that is the smallest mip level's colour shows the texture */
static int magenta = -1;
static int tex_level_mode = GU_TEXTURE_AUTO;
static float tex_bias = 0.0f;
#define SHOWCASE_BIAS_DEFAULT 8 /* quarter levels: two levels coarser, judged right on a PSP-1000 by hand (sharper levels shimmer on the hull); a bias smears the ring and the sky some, so 4 is still to be tried by hand; showcasebias0 as before */
static int in_sky;
static unsigned long ge_part_us[GE_PARTS], ge_part_calls[GE_PARTS];
static const char *const ge_part_names[GE_PARTS] = {
	"level: stream + index range", "level: classify corners", "level: cut up", "level: passes (binds, lists)",
	"model: convert", "model: node matrix", "model: near-plane filter", "model: bind + state + lighting",
	"model: geometry", "model: two-sided + change/glow passes", "decal calls" };
static unsigned int part_mark;
static inline void part_start(void)
{
	part_mark = ge_clock();
}
static inline void part_add(int part)
{
	unsigned int now = ge_clock();

	ge_part_us[part] += now - part_mark;
	ge_part_calls[part]++;
	part_mark = now;
}
static const char *const ge_entry_names[GE_ENTRIES] = { "level draws", "model parts", "see-through models", "see-through level", "sprites", "decals", "lights", "shadows", "see-through drawing" };
static int list_peak = 0, clip_peak = 0;
static float view_far = 1000.0f;

/* the game's atmospheric fog (build 23): Halo fades linearly from its
minimum to its maximum distance, up to its density; the GE fades linearly
to all fog at its far distance, so the far distance is put where a full
fade would end and the two agree up to Halo's maximum (beyond it the GE
keeps thickening where Halo holds). Until the game says, no fog. */
static float fog_near = 1e6f, fog_far = 2e6f;
static unsigned int fog_color = SKY_COLOR;
static unsigned long fog_changes = 0;
/* build 116: the colour each frame is cleared to, as the Xbox clears a
window: its atmospheric fog's colour, fog on or off (rasterizer_xbox.c,
_rasterizer_window_begin), here the last window's (one frame late at most).
Builds 14-115 cleared to a fixed pale blue, which showed wherever nothing
was drawn: a10's space, gaps indoors. Keep word "clearblue": as before */
static unsigned int window_clear_color = SKY_COLOR;
static int clear_blue = -1;
/* build 116: the window's planar fog, as it reaches the eye (psp_fixups.py,
rasterizer.c): the colour, and where it would be all fog (0: none). The
Xbox draws planar fog as a pass of its own over the level and the objects,
from the depth below the fog's plane and the distance
(rasterizer_xbox_environment_fog.c); with the eye inside the fog (a cluster
"fully fogged", c10's swamp, or the eye below the plane) that comes down to
a distance fog in the planar colour, which the GE has. psp_ge_fog fogs with
whichever of the two is thicker. Builds 23-115 had the atmospheric fog
only: c10's swamp ended in a hard edge at its far clip. Keep word
"planarfogoff": as before */
static float planar_far = 0.0f;
static unsigned int planar_color = 0;
static int planar_fog_off = -1;
static unsigned long planar_fog_windows = 0;

/* build 114: "drawdistN" (B9): the far clip at N world units outside
cutscenes, the fog closing at it (render.c, psp_fixups.py FIXES_114); 0 off */
float psp_draw_distance(void)
{
	extern int psp_keep_loaded(void);
	extern int psp_keep_number(const char *prefix, int fallback);
	static float limit = -1.0f;

	if (limit < 0.0f && psp_keep_loaded())
	{
		int n = psp_keep_number("drawdist", 0);

		limit = n > 0 ? (float)n : 0.0f;
		if (limit > 0.0f && !PSP_ME_WORK())
			platform_log("GE: drawdist: the far clip at %d world units outside cutscenes, the fog closing at it", n);
	}
	return limit > 0.0f ? limit : 0.0f;
}

void psp_ge_fog(const float *color, float minimum_distance, float maximum_distance, float density)
{
	GE_DEFER(defer_fog(color, minimum_distance, maximum_distance, density)); /* build 112: gedefer */
	float near = 1e6f, far = 2e6f;
	unsigned int c;
	int r, g, b;

	if (density > 0.001f && maximum_distance > minimum_distance)
	{
		if (density > 1.0f)
			density = 1.0f;
		near = minimum_distance;
		far = minimum_distance + (maximum_distance - minimum_distance) / density;
	}
	r = (int)(color[0] * 255.0f); g = (int)(color[1] * 255.0f); b = (int)(color[2] * 255.0f);
	r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
	c = 0xFF000000 | (b << 16) | (g << 8) | r;
	window_clear_color = c;
	/* build 116: the planar fog, where it is thicker (above) */
	if (planar_far > 0.0f && planar_far < far)
	{
		near = 0.0f;
		far = planar_far;
		c = planar_color;
		planar_fog_windows++;
	}
	if (near != fog_near || far != fog_far || c != fog_color)
	{
		if (fog_changes++ < 8)
			platform_log("GE: fog %.1f to %.1f (density %.2f); the GE's %.1f to %.1f, colour %06x%s", minimum_distance,
				maximum_distance, density, near, far, c & 0xFFFFFF, c == planar_color && near == 0.0f ? " (planar)" : "");
		fog_near = near;
		fog_far = far;
		fog_color = c;
	}
	if (frame_open)
		sceGuFog(fog_near, fog_far, fog_color);
}

/* build 116: the window's planar fog (above), called just before psp_ge_fog
each window: density is the fog's maximum density times how deep the eye is
in it (0 to 1), 0 for none */
void psp_ge_fog_planar(float red, float green, float blue, float maximum_distance, float density)
{
	GE_DEFER(defer_words(DEFER_FOG_PLANAR, 5, defer_bits(red), defer_bits(green), defer_bits(blue),
		defer_bits(maximum_distance), defer_bits(density), 0, 0, 0));
	int r, g, b;

	if (planar_fog_off < 0)
		planar_fog_off = psp_keep("planarfogoff");
	planar_far = 0.0f;
	if (planar_fog_off || density <= 0.001f || maximum_distance <= 0.0f)
		return;
	if (density > 1.0f)
		density = 1.0f;
	/* all fog where a full fade would end, so the fog reaches the planar
	density at its maximum distance, as psp_ge_fog does the atmospheric */
	planar_far = maximum_distance / density;
	r = (int)(red * 255.0f); g = (int)(green * 255.0f); b = (int)(blue * 255.0f);
	r = r < 0 ? 0 : r > 255 ? 255 : r; g = g < 0 ? 0 : g > 255 ? 255 : g; b = b < 0 ? 0 : b > 255 ? 255 : b;
	planar_color = 0xFF000000 | (b << 16) | (g << 8) | r;
}

/* The planar fog seen from above: the window's fog plane and its planar
fog (psp_fixups.py FIXES_OCEAN). The Xbox fogs every level surface by a
pass of its own, ZFUNC EQUAL over what was drawn, blending the planar
colour by a density worked out per vertex from how deep the vertex lies
below the fog's plane and how far it is from the eye
(rasterizer_xbox_environment_fog.c, structure_render_fog). With the eye
above the plane nothing of that reaches psp_ge_fog_planar, so a sea floor
was fogged by the atmospheric fog alone: b30's ocean, where the water's
added colour lies over the seabed, came out violet instead of the deep
green-blue the planar colour gives it. Here each level draw with part of its
box below the plane gets that pass, with the eye above. Keep word
"oceanfogold": as before; "oceanfogseg": the distance counted only along
the part of the sight line below the plane */
static float submerged_plane[4];
static unsigned int submerged_color = 0;
static float submerged_density = 0.0f, submerged_inverse_depth = 0.0f, submerged_inverse_distance = 0.0f;
static int submerged_old = -1, submerged_segment = -1;
static unsigned long submerged_draws = 0, submerged_triangles = 0, submerged_vertices = 0, submerged_us = 0,
	submerged_dropped = 0, submerged_windows = 0, submerged_changes = 0;

void psp_ge_fog_submerged(float i, float j, float k, float d, unsigned int color, float density, float depth,
	float distance)
{
	GE_DEFER(defer_words(DEFER_FOG_SUBMERGED, 8, defer_bits(i), defer_bits(j), defer_bits(k), defer_bits(d), color,
		defer_bits(density), defer_bits(depth), defer_bits(distance)));
	if (submerged_old < 0)
	{
		submerged_old = psp_keep("oceanfogold");
		submerged_segment = psp_keep("oceanfogseg");
	}
	if (density > 1.0f)
		density = 1.0f;
	if (density <= 0.001f || depth <= 0.0f || distance <= 0.0f)
		density = 0.0f;
	else
		submerged_windows++;
	if (density > 0.0f && (color != submerged_color || density != submerged_density) && submerged_changes++ < 8)
		platform_log("GE: planar fog below the plane %.3f %.3f %.3f d %.2f: colour %06x, density %.2f, depth %.2f, distance %.1f%s",
			i, j, k, d, color & 0xFFFFFF, density, depth, distance, submerged_old ? " (oceanfogold: not drawn)" : "");
	submerged_plane[0] = i;
	submerged_plane[1] = j;
	submerged_plane[2] = k;
	submerged_plane[3] = d;
	submerged_color = color & 0xFFFFFF;
	submerged_density = density;
	submerged_inverse_depth = density > 0.0f ? 1.0f / depth : 0.0f;
	submerged_inverse_distance = density > 0.0f ? 1.0f / distance : 0.0f;
}

/* the GE's own state, once: buffers, viewport, depth, fog */
static void cut_take(void); /* below, by model_cut */

/* "ditherprobe": what the GE does to a colour on its way into the 16-bit
frame, measured once at start-up in the buffer not yet shown. Row 0: each
value 0-255 drawn as it is; where each channel's code steps says whether
the GE truncates (red at 8, 16, ...) or rounds (at 4, 12, ...). Rows 1-4
start as row 0 and take one blended pass that ought to leave them as they
are: 1 the frame times one; 2 the same, dithered by -1 everywhere; 3
dithered by +7; 4 the frame times white. The codes that move in rows 2 and
3 say how the frame is read back for blending: with its low bits 0, every
code drops in row 2 and none rises in row 3; with them filled from the top
bits (PPSSPP), only the darkest drop and all but the darkest rise. Rows 1
and 4 say whether "times one" is exact. That decides which dither matrix
leaves an unchanged pixel unchanged (dithershift's open question) */
static void __attribute__((noinline, cold)) ge_dither_probe(void)
{
	static const ScePspIMatrix4 minus = { { -1, -1, -1, -1 }, { -1, -1, -1, -1 }, { -1, -1, -1, -1 }, { -1, -1, -1, -1 } };
	static const ScePspIMatrix4 plus = { { 7, 7, 7, 7 }, { 7, 7, 7, 7 }, { 7, 7, 7, 7 }, { 7, 7, 7, 7 } };
	static const char *const rows[5] = { "drawn as it is", "the frame times one", "dithered by -1",
		"dithered by +7", "the frame times white" };
	static const char *const names[3] = { "red", "green", "blue" };
	static const int shifts[3] = { 0, 5, 11 }, masks[3] = { 0x1F, 0x3F, 0x1F };
	struct probe_vertex { unsigned int color; short x, y, z, pad; } *v;
	const volatile unsigned short *frame = (const volatile unsigned short *)0x44000000; /* draw buffer 0, uncached */
	int x, row, channel;

	sceGuStart(GU_DIRECT, display_list);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_FOG);
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDisable(GU_BLEND);
	sceGuDisable(GU_DITHER);
	/* every value, one column each, down all five rows */
	v = sceGuGetMemory(256 * 2 * sizeof(*v));
	for (x = 0; x < 256; x++)
	{
		unsigned int grey = 0xFF000000 | (x << 16) | (x << 8) | x;

		v[2 * x].color = v[2 * x + 1].color = grey;
		v[2 * x].x = x; v[2 * x].y = 0;
		v[2 * x + 1].x = x + 1; v[2 * x + 1].y = 5;
		v[2 * x].z = v[2 * x + 1].z = 0;
	}
	(sceGuDrawArray)(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 512, NULL, v);
	sceGuEnable(GU_BLEND);
	for (row = 1; row < 5; row++)
	{
		v = sceGuGetMemory(2 * sizeof(*v));
		v[0].color = v[1].color = 0xFFFFFFFF;
		v[0].x = 0; v[0].y = row;
		v[1].x = 256; v[1].y = row + 1;
		v[0].z = v[1].z = 0;
		if (row == 4)
			sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_FIX, 0, 0);
		else
			sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0, 0xFFFFFF);
		if (row == 2 || row == 3)
		{
			sceGuSetDither(row == 2 ? &minus : &plus);
			sceGuEnable(GU_DITHER);
		}
		else
			sceGuDisable(GU_DITHER);
		(sceGuDrawArray)(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
	}
	sceGuDisable(GU_DITHER);
	sceGuDisable(GU_BLEND);
	sceGuEnable(GU_FOG);
	sceGuEnable(GU_DEPTH_TEST);
	stall_force = 1;
	sceGuFinish();
	sceGuSync(0, 0);
	stall_force = 0;

	for (row = 0; row < 5; row++)
	{
		char line[400];
		int length = snprintf(line, sizeof(line), "ditherprobe: row %d (%s):", row, rows[row]);

		for (channel = 0; channel < 3 && length < (int)sizeof(line); channel++)
		{
			int shift = shifts[channel], mask = masks[channel];

			if (row == 0)
			{
				/* the first four inputs at which the code steps up, and the top code */
				int steps = 0, at[4] = { -1, -1, -1, -1 };

				for (x = 1; x < 256 && steps < 4; x++)
					if (((frame[x] >> shift) & mask) != ((frame[x - 1] >> shift) & mask))
						at[steps++] = x;
				length += snprintf(line + length, sizeof(line) - length, " %s steps at %d %d %d %d, 255 is %d;",
					names[channel], at[0], at[1], at[2], at[3], (frame[255] >> shift) & mask);
			}
			else
			{
				/* against row 0: how many inputs moved each way, and the
				highest that dropped, the lowest that rose */
				int up = 0, down = 0, top_down = -1, low_up = -1;

				for (x = 0; x < 256; x++)
				{
					int before = (frame[x] >> shift) & mask, after = (frame[row * BUFFER_WIDTH + x] >> shift) & mask;

					if (after < before)
					{
						down++;
						top_down = x;
					}
					else if (after > before)
					{
						up++;
						if (low_up < 0)
							low_up = x;
					}
				}
				length += snprintf(line + length, sizeof(line) - length, " %s down %d (highest input %d), up %d (lowest %d);",
					names[channel], down, top_down, up, low_up);
			}
		}
		platform_log("%s", line);
	}
}

static void ge_initialize(void)
{
	/* build 108: the scratchpad's pieces (psp_scratch.c), keep.txt read by now */
	extern void psp_profile_tables_take(void);

	/* the boot screen's thread ends first: from here the game's thread alone
	draws, and its stack is free again before the display list looks for room */
	if (boot_up)
	{
		extern void psp_loading_boot_screen_stop(void);

		psp_loading_boot_screen_stop();
	}
	cut_take();
	psp_profile_tables_take();
	display_list_acquire();
	display_list_report();
	/* (the boot screen started the GE already: a second start would register
	its callbacks twice) */
	if (!boot_up)
		sceGuInit();
	gebusy_start(); /* build 113 */
	sceGuStart(GU_DIRECT, display_list);
	blip_place();
	sceGuDrawBuffer(GU_PSM_5650, (void *)0, BUFFER_WIDTH);
	sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, (void *)FRAME_BYTES, BUFFER_WIDTH);
	sceGuDepthBuffer((void *)(2 * FRAME_BYTES), BUFFER_WIDTH);
	sceGuOffset(2048 - SCREEN_WIDTH / 2, 2048 - SCREEN_HEIGHT / 2);
	sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
	sceGuDepthRange(WORLD_DEPTH_NEAR, 0);
	sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDepthFunc(GU_GEQUAL);
	sceGuEnable(GU_DEPTH_TEST);
	/* the winding isn't settled yet: both sides */
	sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_LIGHTING);
	lit_forget(0); /* build 114: statememo */
	/* build 62: Gouraud, as the Xbox shades. Left flat from the first
	untextured builds (build 14), every lit model took one colour a
	triangle once the GE lit models (build 22 on): the facets on the
	weapons' slides and barrels the tests kept showing. "flatshade": as before */
	sceGuShadeModel(psp_keep("flatshade") ? GU_FLAT : GU_SMOOTH);
	sceGuEnable(GU_CLIP_PLANES);
	sceGuFog(fog_near, fog_far, fog_color);
	sceGuTexEnvColor(0xFFFFFFFF);
	/* build 50: "mipbias1" / "mipbias2" pick each texture's next smaller
	level half a level / a whole level sooner (blurrier far off, less
	shimmer). The GE's automatic choice, as PPSSPP models it
	(GPU/Software/Rasterizer.cpp CalculateSamplingParams), looks only at how
	s changes across the screen and how t changes down it, so a surface
	seen at a grazing angle gets a level too sharp for it: grass and dirt
	alternate from row to row at their border (b49) */
	tex_level_mode = psp_keep("nomip") ? GU_TEXTURE_CONST : GU_TEXTURE_AUTO;
	tex_bias = psp_keep("mipbias2") ? 1.0f : psp_keep("mipbias1") ? 0.5f : 0.0f;
	sceGuTexLevelMode(tex_level_mode, tex_bias);
	sceGuEnable(GU_FOG);
	stall_force = 1;
	sceGuFinish();
	sceGuSync(0, 0);
	stall_force = 0;
	draw_buffer = (void *)0; /* as sceGuDrawBuffer has it, whatever the boot screen swapped */
	/* without the boot screen the buffers hold whatever VRAM held, and the
	menu's settle draws its veil from the one on the screen */
	if (!boot_up)
		memset((void *)0x44000000, 0, 2 * FRAME_BYTES);
	if (psp_keep("ditherprobe"))
		ge_dither_probe();
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);
	ge_ready = 1;
	{
		/* from here the screen shows the GE's frames: the on-screen log's VRAM
		goes to textures */
		extern void psp_screen_log_stop(void);
		extern void psp_tex_screen_free(void);

		psp_screen_log_stop();
		psp_tex_screen_free();
	}
	platform_log("GE: drawing the level (480x272, 16-bit; flat colors and fog)");
}

/* a frame from the game's camera: Halo's world is z-up, and its camera gives
its position, forward and up directions and vertical field of view */
static float camera_eye[3];

/* The GE's world space has its origin at ge_eye, the eye rounded so that each
coordinate fits the GE's 24-bit matrix words exactly (with levellocal, its
offset from level_origin does); the view matrix moves the last fraction,
ge_eye_camera (ge_eye seen from the eye, along the camera's right, up and
forward). Why: the level's vertices are world
positions, so its model matrix carries the eye's whole position, and the GE
drops the low 8 bits of every matrix word. Hundreds of units out (c40's
control room is 692 units from the origin, a10's flyby 593), that is up to
1/64 of a unit, and as the eye moves the dropped part jumps about, so the
level shook against the models, whose matrices are small (LK22). With the
origin on a value the GE keeps exactly, the level's translation arrives
whole, and the remainder in the view is a fraction of a unit, which its
24 bits carry to within a millionth. "levelexactoff": the origin at the eye,
the view a pure turn, as before */
static float ge_eye[3], ge_eye_camera[3];
static int level_exact = -1;

/* the value the GE keeps of a matrix word: the low 8 bits dropped (pspgu
sends a float's top 24 bits) */
static float ge_matrix_float(float f)
{
	union { float f; unsigned int u; } bits;

	bits.f = f;
	bits.u &= ~0xFFu;
	return bits.f;
}

/* a model matrix made relative to the GE's world origin */
static void relative(ScePspFMatrix4 *m)
{
	m->w.x -= ge_eye[0];
	m->w.y -= ge_eye[1];
	m->w.z -= ge_eye[2];
}

/* the model matrix for anything given in world coordinates: the world moved
so the camera is at its origin */
static void world_model(void)
{
	ScePspFMatrix4 m;

	gumLoadIdentity(&m);
	relative(&m);
	sceGumLoadMatrix(&m);
}

/* The level's copies give their positions from an origin near the eye, not
from the world's. Why: with the level's translation exact in the matrix
words (ge_eye), the level still shook on the PSP-1000 in c40's opening, while
the emulator, which models only those words, showed it still. So the GE loses
precision on large numbers somewhere else too [U]: most likely it folds the
model and view matrices into one in its own precision, and then a vertex
given 700 units out, and the translation that brings it back to the eye,
each lose about 1/64 of a unit, differently as the camera moves. Models never
shook because their vertices and translations are small (relative()). Given
from level_origin, the level's numbers are the size of the scene round the
eye too ("geprecision" measures the GE directly).
- level_origin is a point of a LEVEL_ORIGIN_STEP lattice. It follows the eye
  (level_origin_follow, as a frame begins) once the eye is more than
  LEVEL_ORIGIN_SLACK from it on an axis, so it moves seldom.
- Positions are snapped to 1/LEVEL_SNAP of a unit as a copy is made (0.24 mm,
  far under a pixel). A lattice point minus a snapped position is then exact
  in a float within LEVEL_REACH, so a copy moved from one origin to another
  loses nothing, however often it moves.
- Each copy carries the origin its positions are given from (its own matrix,
  level_model). The copies drawn in a frame are moved to level_origin at the
  frame's end, when nothing reads them (level_copies_follow); until then a
  copy draws from its own origin, as correctly, with larger numbers.
- With every copy on one origin, a vertex two materials share goes through
  the same numbers in both, as it did in world coordinates: no seams.
- The CPU's tests of a copy's positions use level_eye, the eye seen from the
  copy's origin. The difference of two exact values is one rounding, so they
  get the same floats as the world's would (for the snapped positions).
Everything drawn on the level's own triangles (lights, shadows, the stencil
bake's layers, see-through surfaces, cut pieces) is made from the copy's
positions and drawn through the copy's matrix, so equal depths stay equal.
Decals, which the game gives in world coordinates, are given from
level_origin as they are written. "levellocaloff": every origin the world's,
nothing snapped, as before */
#define LEVEL_ORIGIN_STEP 32.0f
#define LEVEL_ORIGIN_SLACK 24.0f
#define LEVEL_SNAP 4096.0f
#define LEVEL_REACH 4000.0f
static int level_local = -1;
static const float world_origin[3] = { 0.0f, 0.0f, 0.0f };
static float level_origin[3];
static int level_origin_set = 0;
static float level_eye[3];
/* a copy was drawn from an origin other than level_origin this frame */
static int level_copies_behind = 0;
static unsigned long level_origin_moves = 0, level_copies_moved = 0,
	level_vertices_moved = 0, level_move_us = 0;

/* a model matrix for positions given from origin: origin moved to ge_eye,
in full floats on the CPU */
static void level_matrix(ScePspFMatrix4 *m, const float *origin)
{
	gumLoadIdentity(m);
	m->w.x = origin[0];
	m->w.y = origin[1];
	m->w.z = origin[2];
	relative(m);
}

static void level_model(const float *origin)
{
	ScePspFMatrix4 m;

	level_matrix(&m, origin);
	sceGumMatrixMode(GU_MODEL);
	sceGumLoadMatrix(&m);
}

/* the eye seen from a copy's origin, for the CPU's tests of its positions */
static inline void level_frame(const float *origin)
{
	if (origin[0] != level_origin[0] || origin[1] != level_origin[1] || origin[2] != level_origin[2])
		level_copies_behind = 1;
	level_eye[0] = camera_eye[0] - origin[0];
	level_eye[1] = camera_eye[1] - origin[1];
	level_eye[2] = camera_eye[2] - origin[2];
}

/* a position on 1/LEVEL_SNAP of a unit, rounded half away from zero */
static inline float level_snap(float x)
{
	float scaled = x * LEVEL_SNAP;

	return (float)(int)(scaled + (scaled >= 0.0f ? 0.5f : -0.5f)) * (1.0f / LEVEL_SNAP);
}

/* whether every position of a box (world) is exact from origin */
static int level_reach(const float *low, const float *high, const float *origin)
{
	int k;

	for (k = 0; k < 3; k++)
		if (high[k] - origin[k] > LEVEL_REACH || origin[k] - low[k] > LEVEL_REACH)
			return 0;
	return 1;
}

/* level_origin re-centred on the lattice point nearest the eye, once the eye
has moved LEVEL_ORIGIN_SLACK from it on any axis */
static void level_origin_follow(const float *eye)
{
	int k, far = !level_origin_set;

	for (k = 0; k < 3 && !far; k++)
		far = fabsf(eye[k] - level_origin[k]) > LEVEL_ORIGIN_SLACK;
	if (!far)
		return;
	for (k = 0; k < 3; k++)
		level_origin[k] = floorf(eye[k] * (1.0f / LEVEL_ORIGIN_STEP) + 0.5f) * LEVEL_ORIGIN_STEP;
	if (level_origin_set)
		level_origin_moves++;
	level_origin_set = 1;
}

/* build 64: "nearauto", the near plane moved out as far as what is near the
eye allows. near4 (four times finer depth everywhere; the rock bases stop
shimmering, b59) cut away walls within 0.25 of the eye (the bunker
wall in b63 run B, shots 33 and 35). Each frame:
- the level: the game's collision asks whether any structure surface lies
  within 1.4 x 4 x near of the eye, then within 1.4 x 2 x near (1.4: the
  near plane's corners lie farther from the eye than its middle);
- objects: the nearest bounding sphere of the models drawn last frame (the
  sky and the first-person weapon aside), one frame late.
The near plane is then 4, 2 or 1 times the game's. The cost is measured
(the log's "near plane" line) */
extern unsigned char collision_test_sphere(const float *center, float radius, long ignore_object_index);
/* the same answer by a walk that stops at the first surface or edge it
finds (psp_fixups.py, collisions.c#anysphere): most of these spheres touch
the level, and the full test gathered everything inside them first.
"nearautofull": the full test */
extern unsigned char psp_collision_test_sphere_any(const float *center, float radius, long ignore_object_index);
static int near_auto_full = -1, near_auto_check = -1;
static unsigned long near_auto_checked = 0, near_auto_differed = 0;
static int near_auto = 0;
static int near_scale = -1; /* build 77: file scope, for the debug menu */
static int showcase_near = -1;
/* build 79's GE antialias flag ("geaa", GU_LINE_SMOOTH) is gone in build 80:
on the hardware it is for lines (PPSSPP's reading: the texture is looked up
by screen position and alpha halved), and with triangles it painted every
texture as blocks and checkerboards (b79's shots 000/002/014/015) */
/* build 79: texture coordinates re-centred per level draw (the ground
wobble). Blood Gulch's ground is one base texture across the whole terrain
with the detail maps tiled x100 and x60 over it, so the detail pass's
coordinates reach hundreds of repeats, and the cliffs' tens; if the GE
carries them at reduced precision, fractions of a texel are lost and the
pattern swims as the view moves (b64-b78: only the ground, worst on the
detail maps up close). Each draw's coordinates are shifted by whole repeats
to put the vertex nearest the eye near zero (sceGuTexOffset, applied after
the pass's scale, so the shift is an integer in that texture's own repeats:
exact for a repeating texture). "uvwide": off */
static int uv_centre = -1;
static float uv_centre_u = 0.0f, uv_centre_v = 0.0f, uv_shift_largest = 0.0f;
static unsigned long uv_centred_draws = 0;
/* the whole-repeat shift that puts (u, v) x scale near zero */
static void uv_shift(float scale_u, float scale_v)
{
	float su = -floorf(uv_centre_u * scale_u + 0.5f), sv = -floorf(uv_centre_v * scale_v + 0.5f);

	if (fabsf(su) > uv_shift_largest)
		uv_shift_largest = fabsf(su);
	if (fabsf(sv) > uv_shift_largest)
		uv_shift_largest = fabsf(sv);
	sceGuTexOffset(su, sv);
}
static float near_object_distance = 1e30f, near_object_distance_last = 1e30f;
/* "neardump": the nearest model's sphere radius, for the screenshot frames' line */
static float near_object_radius = 0.0f, near_object_radius_last = 0.0f;
static unsigned long near_auto_us = 0, near_auto_frames = 0, near_auto_chosen[3], near_auto_by_objects = 0;
static unsigned long near_auto_chosen_high[2]; /* build 84: x8, x16 */

/* build 84: candidates x16, x8, x4, x2, x1 (b64: x4, x2, x1). Each
doubling halves a depth step at every distance, so the stencils' nudges can
be half the lift for the same number of steps (b82-b83's runs: the
sweet spot between a stencil sinking, which is counted in steps, and
bleeding through what stands in front, which is counted in units, moved
with the distance; a finer step moves the two apart). The price is the
same as before: anything nearer the eye than the plane is cut open, so the
level is tested with a sphere at 1.4 x each candidate and the first that is
clear wins. The nearest model's sphere from the previous frame counts too,
but a sphere that CONTAINS the eye no longer does (the vehicle being driven:
b79's tank runs sat at x1 on most frames, "held down by an object", four
times coarser depth than on foot; a model that close is cut triangle by
triangle anyway, psp_ge_draw_model) */
static int player_seated = 0;
/* first-person models drawn in the frame being drawn and in the one before
(psp_ge_model_shape): nearcine's test for whose eye the camera is */
static unsigned int first_person_models = 0, first_person_models_last = 0;
static unsigned long near_cine_frames = 0, near_cine_first_person = 0;
static float near_plane_auto(const float *eye, float game_near)
{
	extern int psp_player_seated(void);
	unsigned int start = ge_clock();
	int scale, level_scale = 1, object_scale;

	/* build 90: for psp_ge_model_shape this frame */
	player_seated = psp_player_seated();
	if (near_auto_full < 0)
	{
		near_auto_full = psp_keep("nearautofull") != 0;
		near_auto_check = psp_keep("nearautocheck") != 0;
	}
	near_object_distance_last = near_object_distance;
	near_object_distance = 1e30f;
	first_person_models_last = first_person_models;
	first_person_models = 0;
	near_object_radius_last = near_object_radius;
	near_object_radius = 0.0f;
	/* build 106: the nearest model's limit first, and the level tested
	only at the scales it leaves (each test is a sphere query of the level,
	1.2 ms a frame in the emulator at b30's beach, where the marines held
	the plane at x1 anyway). The same choice as testing the level first: a
	sphere clear of the level leaves every smaller one about the same
	centre clear too */
	for (object_scale = 16; object_scale > 1 && near_object_distance_last < game_near * object_scale * 1.4f;)
		object_scale /= 2;
	if (object_scale < 16)
		near_auto_by_objects++;
	for (scale = object_scale; scale > 1; scale /= 2)
	{
		int touches = near_auto_full > 0 ? collision_test_sphere(eye, game_near * scale * 1.4f, -1) :
			psp_collision_test_sphere_any(eye, game_near * scale * 1.4f, -1);

		/* "nearautocheck": both tests, their answers compared */
		if (near_auto_check > 0)
		{
			near_auto_checked++;
			if (!collision_test_sphere(eye, game_near * scale * 1.4f, -1) != !touches)
				near_auto_differed++;
		}
		if (!touches)
		{
			level_scale = scale;
			break;
		}
	}
	scale = level_scale;
	{
		/* "neardump": on a screenshot's frame, what decided its near plane */
		extern volatile int psp_screenshot_requested;
		static int dump = -1;

		if (dump < 0)
			dump = psp_keep("neardump");
		if (dump && psp_screenshot_requested)
			platform_log("neardump: near plane x%d (objects allow x%d, the level x%d); nearest model's sphere %.3f from the eye (radius %.3f), seated %d",
				scale, object_scale, level_scale, near_object_distance_last, near_object_radius_last, player_seated);
	}
	near_auto_chosen[scale >= 4 ? 2 : scale == 2 ? 1 : 0]++;
	if (scale >= 8)
		near_auto_chosen_high[scale == 16 ? 1 : 0]++;
	near_auto_frames++;
	near_auto_us += ge_clock() - start;
	/* "nearcine": in a cutscene, a model nearer the eye than the game's own
	plane gets a plane a quarter as far. The 16:9 view shows the sides that
	4:3 left off screen, and there the cinematic camera's close-ups cut
	faces open (a10's Keyes). The depth range coarsens only while it lasts.
	Not while the camera is the player's own eye (his first-person weapon
	drawn the frame before): b30's flight is a cutscene seen from the
	Pelican's bench, the cabin at the eye, and the quarter plane made the
	16-bit depth four times coarser for all of it: the far ocean tied with
	the seabed again and the shore and the beach structure fought (B21).
	"nearcinefp": in first person too, as before. "nearcineoff": the game's
	plane as before */
	if (scale == 1 && near_object_distance_last < game_near * 1.4f)
	{
		extern unsigned char cinematic_in_progress(void);
		static int near_cine = -1;

		if (near_cine < 0)
			near_cine = psp_keep("nearcineoff") ? 0 : psp_keep("nearcinefp") ? 2 : 1;
		if (near_cine && cinematic_in_progress())
		{
			if (first_person_models_last && near_cine == 1)
				near_cine_first_person++;
			else
			{
				near_cine_frames++;
				return game_near * 0.25f;
			}
		}
	}
	return game_near * scale;
}

/* ---------- build 77: temporal blend (cheap anti-aliasing)

Each frame's world (before the HUD) is blended with the last frame's world,
kept in a history buffer in RAM, and the projection is shifted by a quarter
pixel one way on even frames and the other way on odd ones: an edge's two
samples, half a pixel apart, average into a softer step. The GE does it (one
full-screen sprite pass and one copy, well under a millisecond of its time;
the CPU only queues them), and the GE had time to spare (b75: it waited for
the CPU 0.4 ms a frame). The price is ghosting when the view moves; the
adaptive setting lowers the blend while the view turns. The history holds
the world only, so the HUD and the debug menu never feed back into it.
Settings live in the debug menu (psp_debug.c): strength off/25/37/50%,
jitter, adaptive */
int psp_taa_strength = 0, psp_taa_jitter = 0, psp_taa_adaptive = 1;
/* build 78: jitter off by default (b77's run: everything vibrated). The
blend feeds its own output back (out = a x last out + (1 - a) x new), so the
two jittered positions never get equal weight: at 50% the newer one weighs
2/3 and the older 1/3, alternating each frame, and the image rocks by about
a sixth of a pixel at 15 Hz; at 25%, more. An exact average of the last two
frames would be still, but needs a second 255 KB buffer we don't have.

The spatial soften instead: the frame copied, then drawn back over itself
twice, half a pixel down-right and half a pixel up-left, with bilinear
filtering (each lands on a texel corner, the average of four), at fixed
weights: light 2/3 the pixel and 1/6 each average, medium 1/2 and 1/4,
strong 1/3 and 1/3. No history, so no ghosting and no vibration; the price
is a slight blur of textures as well as edges. It uses the same buffer, so
the temporal blend is off while it is on */
int psp_soften = 0;
float psp_taa_alpha_used = 0.0f;
static unsigned short *taa_history = NULL;
static int taa_valid = 0, taa_done = 0, taa_failed = 0;
static float taa_last_forward[3] = { 0.0f, 0.0f, 0.0f };
static float taa_jitter_x = 0.0f, taa_jitter_y = 0.0f;
static unsigned long taa_frame = 0, taa_frames_blended = 0;
#define TAA_BYTES (SCREEN_WIDTH * SCREEN_HEIGHT * 2)

/* the perspective, shifted by this frame's jitter: a translation in
normalised device units applied after the projection moves every point the
same fraction of a pixel on the screen */
static void perspective(float fov_degrees, float n, float f)
{
	sceGumLoadIdentity();
	if (taa_jitter_x != 0.0f || taa_jitter_y != 0.0f)
	{
		ScePspFVector3 shift = { taa_jitter_x, taa_jitter_y, 0.0f };

		sceGumTranslate(&shift);
	}
	sceGumPerspective(fov_degrees, (float)SCREEN_WIDTH / SCREEN_HEIGHT, n, f);
}

/* ---------- "geprecision": the GE's precision with large coordinates,
measured on whatever runs the build (the emulator models only the matrix
words' 24 bits; the level's shake on the PSP says the GE loses more).

As the first frame begins, a white quad 2 units ahead of a camera 800 units
from the world's origin (c40's control room) is drawn alone, over and over,
with the camera moved 1/256 of a unit sideways each time (the "move" sweep)
or turned 2 milliradians (the "turn" sweep), 32 steps each. The quad's right
edge is slanted, so the white pixels counted along every row give where the
edge lies to a small fraction of a pixel. A GE exact to the floats' precision
moves the edge smoothly, a straight line against the step (a parabola for
the turn); what the GE rounds shows as the steps' scatter about that line.
Each way of giving the quad to the GE is one line of the log:
  world, eye in model: world positions, the eye's whole position in the
    model matrix's translation (the level before the exact origin);
  world, exact origin: world positions, ge_eye in the model matrix and the
    rest in the view (levelexact);
  local 28 away: positions from an origin 28 units from the eye, with the
    exact origin (levellocal, near its farthest from level_origin);
  local at the quad: positions from an origin at the quad (the best case);
  local 28 away, eye in model: as the third, without the exact origin;
  near the origin: the whole scene a few units from the world's origin
    (control: nothing is large).
Read: a scatter (rms) well over the control's in the first two and not in
the local ones means large positions themselves lose precision in the GE,
and the levellocal fix is the right one. The emulator should show scatter
only in the first */
#define GEPREC_STEPS 32
#define GEPREC_LEFT 160
#define GEPREC_RIGHT 336

struct geprec_corner
{
	float x, y, z;
};

/* the edge's mean column over the screen's rows, from what the GE drew */
static float geprec_edge(void)
{
	const volatile unsigned short *frame = (const volatile unsigned short *)0x44000000;
	unsigned long white = 0;
	int x, y;

	for (y = 0; y < SCREEN_HEIGHT; y++)
		for (x = GEPREC_LEFT; x < GEPREC_RIGHT; x++)
			white += frame[y * BUFFER_WIDTH + x] != 0;
	return GEPREC_LEFT + (float)white / SCREEN_HEIGHT;
}

/* one picture: the quad (world corners) from eye looking along forward, given
to the GE as mode says; returns the edge's column */
static float geprec_draw(int mode, const float *eye, const float *forward, const float corners[4][3],
	const float *origin)
{
	static const int order[6] = { 0, 1, 2, 0, 2, 3 };
	ScePspFVector3 zero = { 0.0f, 0.0f, 0.0f }, ahead = { forward[0], forward[1], forward[2] }, up = { 0.0f, 0.0f, 1.0f };
	ScePspFMatrix4 model;
	struct geprec_corner *v;
	float g[3], from[3];
	int exact = mode != 0 && mode != 4, local = mode >= 2 && mode <= 4, k;

	for (k = 0; k < 3; k++)
	{
		from[k] = local ? origin[k] : 0.0f;
		/* as psp_ge_window_begin rounds ge_eye */
		g[k] = !exact ? eye[k] : local ? from[k] + ge_matrix_float(eye[k] - from[k]) : ge_matrix_float(eye[k]);
	}
	sceGuStart(GU_DIRECT, display_list);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_FOG);
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDisable(GU_BLEND);
	sceGuDisable(GU_CULL_FACE);
	sceGuClearColor(0);
	sceGuClear(GU_COLOR_BUFFER_BIT);
	sceGuColor(0xFFFFFFFF);
	sceGumMatrixMode(GU_PROJECTION);
	perspective(70.0f, 0.0625f, 1024.0f);
	sceGumMatrixMode(GU_VIEW);
	sceGumLoadIdentity();
	sceGumLookAt(&zero, &ahead, &up);
	if (exact)
	{
		ScePspFVector3 shift = { g[0] - eye[0], g[1] - eye[1], g[2] - eye[2] };

		sceGumTranslate(&shift);
	}
	gumLoadIdentity(&model);
	model.w.x = from[0] - g[0];
	model.w.y = from[1] - g[1];
	model.w.z = from[2] - g[2];
	sceGumMatrixMode(GU_MODEL);
	sceGumLoadMatrix(&model);
	v = sceGuGetMemory(6 * sizeof(struct geprec_corner));
	for (k = 0; k < 6; k++)
	{
		v[k].x = corners[order[k]][0] - from[0];
		v[k].y = corners[order[k]][1] - from[1];
		v[k].z = corners[order[k]][2] - from[2];
	}
	sceGumDrawArray(GU_TRIANGLES, GU_VERTEX_32BITF | GU_TRANSFORM_3D, 6, NULL, v);
	sceGuFinish();
	sceGuSync(0, 0);
	return geprec_edge();
}

static void __attribute__((cold)) ge_precision_test(void)
{
	static const char *const names[6] = { "world, eye in model", "world, exact origin", "local 28 away",
		"local at the quad", "local 28 away, eye in model", "near the origin" };
	float base[2][3] = { { 361.37f, -714.21f, 76.11f }, { 3.37f, -2.21f, 1.13f } };
	int mode, sweep;

	stall_force = 1;
	platform_log("geprecision: a quad 2 units ahead, its edge's column (pixels) over %d steps: the camera moved 1/256 unit sideways (move) or turned 2 mrad (turn); fit a parabola, rms and largest scatter about it",
		GEPREC_STEPS);
	for (mode = 0; mode < 6; mode++)
		for (sweep = 0; sweep < 2; sweep++)
		{
			const float *e0 = base[mode == 5];
			float forward0[3] = { 0.8f, -0.6f, -0.1f }, right[3], length, corners[4][3], origin[3], edge[GEPREC_STEPS];
			float s[5] = { 0.0f }, t[3] = { 0.0f }, rms = 0.0f, most = 0.0f, a, b, c, det;
			int k, step;

			length = sqrtf(forward0[0] * forward0[0] + forward0[1] * forward0[1] + forward0[2] * forward0[2]);
			for (k = 0; k < 3; k++)
				forward0[k] /= length;
			/* right = forward x up (up the world's z) */
			right[0] = forward0[1];
			right[1] = -forward0[0];
			right[2] = 0.0f;
			length = sqrtf(right[0] * right[0] + right[1] * right[1]);
			right[0] /= length;
			right[1] /= length;
			{
				/* the corners across and up the view (r, u), 2 units ahead; the
				right edge leans 0.12 units over the screen's height */
				static const float ru[4][2] = { { -5.0f, -1.5f }, { 0.0f, -1.5f }, { 0.12f, 1.5f }, { -5.0f, 1.5f } };
				float camera_upward[3] = { right[1] * forward0[2] - right[2] * forward0[1],
					right[2] * forward0[0] - right[0] * forward0[2], right[0] * forward0[1] - right[1] * forward0[0] };
				int corner;

				for (corner = 0; corner < 4; corner++)
					for (k = 0; k < 3; k++)
						corners[corner][k] = e0[k] + 2.0f * forward0[k] + ru[corner][0] * right[k] + ru[corner][1] * camera_upward[k];
			}
			for (k = 0; k < 3; k++)
				origin[k] = mode == 3 ? floorf(corners[0][k] + 0.5f) : floorf(e0[k] + 0.5f) + (k == 0 ? 20.0f : k == 1 ? -20.0f : 4.0f);
			for (step = 0; step < GEPREC_STEPS; step++)
			{
				float eye[3], forward[3];

				for (k = 0; k < 3; k++)
				{
					eye[k] = e0[k] + (sweep == 0 ? step * (1.0f / 256.0f) * right[k] : 0.0f);
					forward[k] = forward0[k];
				}
				if (sweep == 1)
				{
					float angle = step * 0.002f, cosine = cosf(angle), sine = sinf(angle);

					forward[0] = forward0[0] * cosine - forward0[1] * sine;
					forward[1] = forward0[0] * sine + forward0[1] * cosine;
				}
				edge[step] = geprec_draw(mode, eye, forward, corners, origin);
			}
			/* least squares a + b k + c k^2 */
			for (step = 0; step < GEPREC_STEPS; step++)
			{
				float k1 = (float)step, k2 = k1 * k1;

				s[0] += 1.0f; s[1] += k1; s[2] += k2; s[3] += k2 * k1; s[4] += k2 * k2;
				t[0] += edge[step]; t[1] += edge[step] * k1; t[2] += edge[step] * k2;
			}
			det = s[0] * (s[2] * s[4] - s[3] * s[3]) - s[1] * (s[1] * s[4] - s[3] * s[2]) + s[2] * (s[1] * s[3] - s[2] * s[2]);
			a = (t[0] * (s[2] * s[4] - s[3] * s[3]) - s[1] * (t[1] * s[4] - s[3] * t[2]) + s[2] * (t[1] * s[3] - s[2] * t[2])) / det;
			b = (s[0] * (t[1] * s[4] - s[3] * t[2]) - t[0] * (s[1] * s[4] - s[3] * s[2]) + s[2] * (s[1] * t[2] - t[1] * s[2])) / det;
			c = (s[0] * (s[2] * t[2] - t[1] * s[3]) - s[1] * (s[1] * t[2] - t[1] * s[2]) + t[0] * (s[1] * s[3] - s[2] * s[2])) / det;
			for (step = 0; step < GEPREC_STEPS; step++)
			{
				float r = edge[step] - (a + b * step + c * step * step);

				rms += r * r;
				if (fabsf(r) > most)
					most = fabsf(r);
			}
			rms = sqrtf(rms / GEPREC_STEPS);
			platform_log("geprecision: %-28s %s: edge at %.2f, %+.3f px a step; scatter rms %.3f px, largest %.3f px",
				names[mode], sweep ? "turn" : "move", edge[0], b, rms, most);
		}
	/* the GE as ge_initialize left it, and a black frame */
	sceGuStart(GU_DIRECT, display_list);
	sceGuClearColor(0);
	sceGuClear(GU_COLOR_BUFFER_BIT);
	sceGuEnable(GU_DEPTH_TEST);
	sceGuEnable(GU_FOG);
	sceGuFinish();
	sceGuSync(0, 0);
	stall_force = 0;
}

static int taa_history_ready(void)
{
	if (taa_history)
		return 1;
	if (taa_failed)
		return 0;
	taa_history = memalign(64, TAA_BYTES);
	if (!taa_history)
	{
		platform_log("GE: temporal blend / soften: no room for the %d KB buffer; off", TAA_BYTES / 1024);
		taa_failed = 1;
		psp_taa_strength = psp_soften = 0;
		return 0;
	}
	sceKernelDcacheWritebackInvalidateRange(taa_history, TAA_BYTES);
	platform_log("GE: temporal blend / soften: %d KB buffer at %p", TAA_BYTES / 1024, taa_history);
	return 1;
}

/* one textured full-screen pass of the buffer over the frame, shifted by
(shift, shift) texels, blended as src x keep_src + dst x keep_dst */
static void buffer_pass(float shift, int linear, unsigned int a, unsigned int b)
{
	struct pass_vertex { float u, v, x, y, z; } *v;
	int strip, strips = SCREEN_WIDTH / 32;

	v = list_take(strips * 2 * sizeof(*v));
	if (!v)
		return;
	for (strip = 0; strip < strips; strip++)
	{
		v[2 * strip].u = strip * 32 + shift;
		v[2 * strip].v = shift;
		v[2 * strip].x = (float)(strip * 32);
		v[2 * strip].y = 0.0f;
		v[2 * strip].z = 0.0f;
		v[2 * strip + 1].u = strip * 32 + 32 + shift;
		v[2 * strip + 1].v = SCREEN_HEIGHT + shift;
		v[2 * strip + 1].x = (float)(strip * 32 + 32);
		v[2 * strip + 1].y = (float)SCREEN_HEIGHT;
		v[2 * strip + 1].z = 0.0f;
	}
	sceGuTexFilter(linear ? GU_LINEAR : GU_NEAREST, linear ? GU_LINEAR : GU_NEAREST);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, a | (a << 8) | (a << 16), b | (b << 8) | (b << 16));
	sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, strips * 2, NULL, v);
}

static void buffer_state(int on)
{
	if (on)
	{
		sceGuTexSync();
		sceGuDisable(GU_DEPTH_TEST);
		sceGuDepthMask(GU_TRUE);
		sceGuDisable(GU_FOG);
		sceGuDisable(GU_CULL_FACE);
		sceGuDisable(GU_ALPHA_TEST);
		sceGuEnable(GU_TEXTURE_2D);
		sceGuTexMode(GU_PSM_5650, 0, 0, 0);
		sceGuTexImage(0, 512, 512, SCREEN_WIDTH, taa_history);
		sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
		sceGuTexWrap(GU_CLAMP, GU_CLAMP);
		sceGuTexFlush();
		ge_blend(1);
	}
	else
	{
		ge_blend(0);
		sceGuEnable(GU_DEPTH_TEST);
		sceGuDepthMask(GU_FALSE);
		sceGuEnable(GU_FOG);
	}
}

static void copy_frame_to_buffer(void)
{
	sceGuCopyImage(GU_PSM_5650, 0, 0, SCREEN_WIDTH, SCREEN_HEIGHT, BUFFER_WIDTH,
		(void *)((unsigned int)sceGeEdramGetAddr() + (unsigned int)draw_buffer), 0, 0, SCREEN_WIDTH, taa_history);
}

static unsigned long soften_frames = 0;
static void soften_pass(void)
{
	/* (first pass weight, second pass weight) out of 255: light, medium,
	strong; the second is chosen so both shifted averages end equal */
	static const unsigned int first[4] = { 0, 51, 85, 128 }, second[4] = { 0, 43, 64, 85 };
	int level = psp_soften > 3 ? 3 : psp_soften;

	if (!taa_history_ready())
		return;
	copy_frame_to_buffer();
	buffer_state(1);
	buffer_pass(0.5f, 1, first[level], 255 - first[level]);
	buffer_pass(-0.5f, 1, second[level], 255 - second[level]);
	buffer_state(0);
	soften_frames++;
}

static void taa_resolve(void)
{
	static const float strengths[4] = { 0.0f, 0.25f, 0.375f, 0.5f };
	float alpha;

	/* (build 112: never in the Media Engine's work: it reads VRAM, which the
	ME can't see; temporal blend is settled off anyway) */
	if (taa_done || !frame_open || PSP_ME_WORK())
		return;
	taa_done = 1;
	if (psp_soften > 0 && !taa_failed)
	{
		taa_valid = 0;
		psp_taa_alpha_used = 0.0f;
		soften_pass();
		return;
	}
	if (psp_taa_strength <= 0 || psp_taa_strength > 3 || taa_failed)
	{
		taa_valid = 0;
		psp_taa_alpha_used = 0.0f;
		return;
	}
	if (!taa_history_ready())
		return;
	alpha = strengths[psp_taa_strength];
	if (psp_taa_adaptive)
	{
		/* the turn since the last frame: none up to half a degree, the
		full blend; four degrees and more, none */
		float dot = camera_forward[0] * taa_last_forward[0] + camera_forward[1] * taa_last_forward[1] +
			camera_forward[2] * taa_last_forward[2];
		float degrees = dot >= 1.0f ? 0.0f : dot <= -1.0f ? 180.0f : acosf(dot) * (180.0f / 3.14159265f);
		float keep = 1.0f - (degrees - 0.5f) / 3.5f;

		alpha *= keep < 0.0f ? 0.0f : keep > 1.0f ? 1.0f : keep;
	}
	taa_last_forward[0] = camera_forward[0];
	taa_last_forward[1] = camera_forward[1];
	taa_last_forward[2] = camera_forward[2];
	psp_taa_alpha_used = alpha;
	if (taa_valid && alpha > 0.01f)
	{
		unsigned int a = (unsigned int)(alpha * 255.0f + 0.5f);

		buffer_state(1);
		buffer_pass(0.0f, 0, a, 255 - a);
		buffer_state(0);
		taa_frames_blended++;
	}
	/* this frame's world, blended, is the next frame's history */
	copy_frame_to_buffer();
	taa_valid = 1;
}

/* build 77: the debug menu's live settings (psp_debug.c). which: 0 detail
maps on, 1 nudge ceiling on, 2 level pass mode (0-4), 3 near plane (0 auto,
1 x4, 2 x2, 3 x1), 4 blend strength (0-3), 5 jitter, 6 adaptive. Returns the
value, after setting it when set is non-zero */
static int nudge_wide;
static int shadow_mode = 0;
/* build 86: the dynamic light pass, a test like b85's shadow one (b85's
run: the shadow modes changed nothing, so the bright stripes beside the tank
and on the bunker are this pass). 0 as before: the lit triangles copied into
their own vertex format, depth EQUAL; 1 the level's own vertex array and
format with the kept triangles' indices, one flat colour a draw; 2 depth
GEQUAL with a nudge toward the eye; 3 the pass not drawn at all */
/* build 87: the nudge by default (b86's run 1: it removed the stripes
and looked right; "level array", the level's own vertices, still striped) */
static int light_mode = 2;
void psp_ge_forget_level(void);
static int stencil_mode;
static int level_compact;
/* build 91: detail passes levelled by distance only (see the detail pass) */
static int detail_by_distance = 1;
static int decal_project;
static unsigned long light_index_triangles = 0, light_nudged_draws = 0, lights_skipped = 0;
static int nudge_percent = 0; /* build 84: the lift limit off by default (b83's run) */
/* build 82: a test instrument. A fixed number of depth steps added to every
nudge toward the eye (wall symbols and decals), from the debug menu: 0, 16,
64, 256. With the slope nudges positive and sizeable (b81's tdump at red
base: +3 to +269 steps) the pillars' emblems still sank into their walls,
so the question is how large the hardware's own depth error is across a
large wall triangle; the floor that makes them whole measures it */
static int nudge_floor = 0;
/* build 83: the distance part of the nudge is a LIFT off the surface that
grows with the distance, a share of it with a ceiling, replacing b54's
fixed half centimetre. b82's run: the base emblems needed a floor of
16 steps from the tank at 20-30 units (0.4 units of lift there), none at
4 units, where 256 steps (0.27 units) saw through the pillar; a fixed step
count lifted far stencils through hills (16 steps at 100 units is 10
units). So: lift = min(share x distance, ceiling); 2% and 0.4 units give
0.08 units at 4 units (77 steps, under the 256 that bled), 0.4 at 20 (15
steps, the 16 that worked), 0.6 steps at 100 (the 3-step minimum). Both
from the debug menu */
static float nudge_lift_share = 0.01f, nudge_lift_ceiling = 0.2f; /* build 84: the close-range best (b83) */
float psp_debug_seconds(void);
static int detail_off;
int psp_ge_setting(int which, int set, int value)
{
	if (detail_off < 0)
		detail_off = psp_keep("nodetail");
	if (nudge_wide < 0)
		nudge_wide = psp_keep("nudgewide");
	switch (which)
	{
	case 0:
		if (set) detail_off = !value;
		return !detail_off;
	case 1:
	{
		static const int percents[5] = { 0, 1, 2, 4, 8 };
		int k;

		if (nudge_wide < 0) nudge_wide = psp_keep("nudgewide");
		if (set)
		{
			nudge_percent = percents[value < 0 ? 0 : value > 4 ? 4 : value];
			nudge_wide = 0;
		}
		if (nudge_wide)
			return 0;
		for (k = 0; k < 5; k++)
			if (percents[k] == nudge_percent)
				return k;
		return 2;
	}
	case 2:
		if (set) psp_pass_mode = value;
		return psp_pass_mode;
	case 3:
		if (set)
		{
			near_auto = value == 0;
			near_scale = value == 1 ? 4 : value == 2 ? 2 : value == 3 ? 1 : (near_scale < 0 ? 1 : near_scale);
		}
		return near_auto ? 0 : near_scale == 4 ? 1 : near_scale == 2 ? 2 : 3;
	case 4:
		if (set) psp_taa_strength = value;
		return psp_taa_strength;
	case 5:
		if (set) psp_taa_jitter = value;
		return psp_taa_jitter;
	case 6:
		if (set) psp_taa_adaptive = value;
		return psp_taa_adaptive;
	case 7:
		if (set) psp_soften = value;
		return psp_soften;
	case 20:
		/* build 93: benchrun: the GE skips a kind of draw (GE_SKIP_*) */
		if (set)
			ge_skip = value;
		return ge_skip;
	case 21:
		/* build 93: benchrun: the display list in VRAM (needs benchrun's
		VRAM room) */
		if (set)
			list_in_vram = value && psp_keep("benchrun");
		return list_in_vram;
	case 22:
		/* build 93: benchrun: the GE busy on VRAM only. value > 0: on, sized
		toward value tenths of a ms of GE time (timed alone: serial); -1: the
		size held and the GE overlapped again (the measurement); 0: off */
		if (set)
		{
			if (value > 0 && psp_keep("benchrun"))
			{
				vram_busy = 1;
				vram_busy_target_us = (unsigned int)value * 100;
				vram_busy_calibrating = 1;
				ge_skip = GE_SKIP_ALL;
				ge_serial = 1;
			}
			else if (value < 0 && vram_busy)
			{
				vram_busy_calibrating = 0;
				ge_serial = 0;
			}
			else if (!value)
			{
				if (vram_busy)
				{
					ge_skip = 0;
					ge_serial = 0;
				}
				vram_busy = vram_busy_calibrating = 0;
			}
		}
		return vram_busy ? vram_busy_sprites : 0;
	case 23:
		/* build 102: benchrun: every texture bind (1) or the lightmaps' (2)
		replaced by one 16 x 16 texture in VRAM: the GE reads no texels from
		main RAM (psp_tex.c). Splits b93's "models and the level each carry
		half the bus cost" into texels and vertices */
		if (set)
		{
			extern int psp_tex_standin;

			psp_tex_standin = value && psp_keep("benchrun") ? value : 0;
		}
		{
			extern int psp_tex_standin;

			return psp_tex_standin;
		}
	case 24:
		/* build 102: benchrun: every mipped texture one level smaller (the
		GE's level bias +1): a quarter of the texels for the same pixels */
		{
			static float saved_bias = -1.0f;

			if (set)
			{
				if (saved_bias < 0.0f)
					saved_bias = tex_bias;
				tex_bias = saved_bias + (value ? 1.0f : 0.0f);
				sceGuTexLevelMode(tex_level_mode, tex_bias);
			}
			return saved_bias >= 0.0f && tex_bias > saved_bias;
		}
	case 9:
		if (ge_serial < 0) ge_serial = psp_keep("geserial");
		if (set) ge_serial = value;
		return ge_serial;
	case 10:
		if (uv_centre < 0) uv_centre = !psp_keep("uvwide");
		if (set) uv_centre = value;
		return uv_centre;
	case 12:
	{
		static const float shares[4] = { 0.005f, 0.01f, 0.02f, 0.04f };
		int k;

		if (set)
			nudge_lift_share = shares[value < 0 ? 0 : value > 3 ? 3 : value];
		for (k = 0; k < 4; k++)
			if (shares[k] == nudge_lift_share)
				return k;
		return 2;
	}
	case 13:
	{
		static const float ceilings[4] = { 0.1f, 0.2f, 0.4f, 0.8f };
		int k;

		if (set)
			nudge_lift_ceiling = ceilings[value < 0 ? 0 : value > 3 ? 3 : value];
		for (k = 0; k < 4; k++)
			if (ceilings[k] == nudge_lift_ceiling)
				return k;
		return 2;
	}
	case 14:
		if (set) shadow_mode = value;
		return shadow_mode;
	case 15:
		if (set) light_mode = value < 0 ? 0 : value > 3 ? 3 : value;
		return light_mode;
	case 16:
		/* build 87: the stencil bake. Changing it lets the level's copies go,
		so they are rebuilt with or without the pieces' vertices */
		if (set && value != stencil_mode)
		{
			stencil_mode = value < 0 ? 0 : value > 2 ? 2 : value;
			psp_ge_forget_level();
		}
		return stencil_mode;
	case 19:
		/* build 91: the detail passes' texture level by distance alone (1),
		or with the slant buckets per triangle (0, b64-b90) */
		if (set)
			detail_by_distance = value != 0;
		return detail_by_distance;
	case 18:
		/* build 90: the permanent decals on the level's own triangles */
		if (set)
			decal_project = value != 0;
		return decal_project;
	case 25:
		/* build 103: benchrun: the level's vertex copies in VRAM; rebuilt */
		if (set && (value != 0 && psp_keep("benchrun")) != (level_vram != 0))
		{
			level_vram = value != 0 && psp_keep("benchrun");
			psp_ge_forget_level();
			vram_vertex_refused = vram_vertex_streams = 0;
		}
		return level_vram;
	case 26:
		/* build 103: the VRAM copies' use, for the bench's RESULT line:
		value 0 KB used, 1 copies placed, 2 copies refused (no room) */
		return value == 0 ? (int)(vram_vertex_next / 1024) : value == 1 ? (int)vram_vertex_streams : (int)vram_vertex_refused;
	case 17:
		/* build 87: the level's vertices at 16 bits; the copies are rebuilt */
		if (level_compact < 0)
			level_compact = psp_keep("levelcompact");
		if (set && (value != 0) != (level_compact != 0))
		{
			level_compact = value != 0;
			psp_ge_forget_level();
		}
		return level_compact;
	case 11:
	{
		static const int floors[4] = { 0, 16, 64, 256 };
		int k;

		if (set)
			nudge_floor = floors[value < 0 ? 0 : value > 3 ? 3 : value];
		for (k = 0; k < 4; k++)
			if (floors[k] == nudge_floor)
				return k;
		return 0;
	}
	}
	return 0;
}

/* build 93: benchrun's "GE busy on VRAM only": every real draw skipped,
and at the frame's start the GE given work that touches only VRAM (a 512 x
256 16-bit texture in VRAM's benchrun room, copied over itself as sprites,
the list's own few hundred bytes aside), as long as this view's normal frame
keeps it busy. If the CPU still slows down, main RAM's bus isn't why */

static void vram_busy_load(void)
{
	struct { unsigned short u, v; short x, y, z; } *corners;
	int k;

	sceGuDrawBufferList(GU_PSM_5650, (void *)VRAM_BENCH_START, 512);
	sceGuScissor(0, 0, 512, 256);
	sceGuDisable(GU_DEPTH_TEST);
	ge_blend(0);
	sceGuDisable(GU_FOG);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexMode(GU_PSM_5650, 0, 0, 0);
	sceGuTexImage(0, 512, 256, 512, (void *)(0x04000000 + VRAM_BENCH_START));
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	sceGuTexScale(1.0f, 1.0f);
	sceGuTexOffset(0.0f, 0.0f);
	corners = sceGuGetMemory(2 * sizeof(*corners));
	corners[0].u = 0; corners[0].v = 0; corners[0].x = 0; corners[0].y = 0; corners[0].z = 0;
	corners[1].u = 512; corners[1].v = 256; corners[1].x = 512; corners[1].y = 256; corners[1].z = 0;
	for (k = 0; k < vram_busy_sprites; k++)
		(sceGuDrawArray)(GU_SPRITES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, corners);
	sceGuDrawBufferList(GU_PSM_5650, draw_buffer, BUFFER_WIDTH);
	sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	sceGuEnable(GU_DEPTH_TEST);
	sceGuEnable(GU_FOG);
	sceGuDisable(GU_TEXTURE_2D);
	/* overlapped: the GE told at once (the skipped draws make few stall
	updates, and they are passed on in batches) */
	if (!vram_busy_calibrating)
	{
		stall_force = 1;
		sceGuGetMemory(4);
		stall_force = 0;
	}
}

/* the load's size, from this frame's wait for the GE while calibrating (the
GE alone then: serial, and every real draw skipped) */
static void vram_busy_calibrate(unsigned int waited_us)
{
	int wanted;

	if (!vram_busy || !vram_busy_calibrating)
		return;
	/* too little to time: double it */
	if (waited_us < 200)
		wanted = vram_busy_sprites * 2;
	else
		wanted = (int)((float)vram_busy_sprites * (float)vram_busy_target_us / (float)waited_us + 0.5f);
	/* halfway each frame, within 1..400 sprites */
	wanted = (vram_busy_sprites + wanted) / 2;
	vram_busy_sprites = wanted < 1 ? 1 : wanted > 400 ? 400 : wanted;
}

extern int psp_game_running(void); /* psp_fixups.py: players.c */
static unsigned long win_other_targets = 0; /* windows for another target than the screen, for frameend: */

/* build 108: each window's target, from the game's rasterizer_window_begin
(psp_fixups.py). A window for another target than the screen (the Xbox's
mirrors render into a second one first) is counted for the frameend: line:
the GE has the screen alone, so such a window's draws land on it. b107 drew
b30's floor mirrors that way, the whole level seen from the mirror's camera
under the floor (render.c no longer renders mirrors, psp_fixups.py) */
void psp_ge_window_target(int target)
{
	GE_DEFER(defer_words(DEFER_WINDOW_TARGET, 1, (unsigned int)target, 0, 0, 0, 0, 0, 0, 0)); /* build 112: gedefer */
	if (target != 0)
		win_other_targets++;
}

/* the showcase's maps are full size, where every other map's pack halves
them to the GE's budget, but the GE's automatic level for a model's part
goes by depth, not by the map's size: the hull's 512-wide maps were drawn
from levels two finer than the same view picks on a level, and shimmered as
it moved. "showcasebiasN": N quarter levels coarser, on ui.map only (the
showcase rule: nothing it does reaches the other maps) */
extern int psp_showcase_map;
static float tex_bias_base = -1.0f;
static int showcase_applied = -1, tex_level_mode_base = -1;

int psp_keep_number(const char *prefix, int fallback);

static void showcase_levels(void)
{
	if (!ge_ready)
		return;
	if (tex_bias_base < 0.0f)
		tex_bias_base = tex_bias;
	if (showcase_applied == psp_showcase_map)
		return;
	showcase_applied = psp_showcase_map;
	tex_bias = tex_bias_base + (psp_showcase_map ? psp_keep_number("showcasebias", SHOWCASE_BIAS_DEFAULT) * 0.25f : 0.0f);
	/* the menu's textures at their largest level whatever the slope says:
	the real GE's level choice by slope drew the menu's hull from a small,
	blurred level where the emulator draws it sharp (a PSP run against
	an earlier one; the PSP-3000 with this build matches the emulator).
	The showcase rule allows it (nothing reaches the other maps);
	"showcasesharpoff" leaves the choice to the slope */
	if (tex_level_mode_base < 0)
		tex_level_mode_base = tex_level_mode;
	tex_level_mode = psp_showcase_map && !psp_keep("showcasesharpoff") ? GU_TEXTURE_CONST : tex_level_mode_base;
	sceGuTexLevelMode(tex_level_mode, tex_bias);
}

/* a frame's window begins in two parts. The main CPU's (the first window of
a frame): what reads the game or the level, or writes what the list's builder
reads (the near plane's choice, the level's origin, the shots asked for, the
recorder's frame), and its answer, the world's near plane. The list's: the
GE's state, the camera, the list's start. geinflight writes the list's part
down for the Media Engine's turn (docs/frames_in_flight.md); otherwise the
two run back to back. One window a frame: the GE's frame takes the first
window's camera, and the later ones (letterbox bars, the non-player
overlays: 2-D, on the same screen) draw into it */
/* this frame's first window has had its main CPU's part (cleared at the frame's
end): a later window of the frame, recorded or not, must not start a list */
static PSP_MAIN_SIDE int window_main_done = 0;

/* geshow3: the frame's show asked of the forwarding thread: the swap's
pointers move now (the next frame's list and replay read them), the display
waits for the GE's finish of this one */
static void show3_ask(void)
{
	unsigned int end = ((unsigned int)gu_list->current & ~0x40000000u) - ((unsigned int)display_list & ~0x40000000u);

	show2_shows++;
	psp_ge_show_busy = 1;
	show2_owed = 1;
	show3_list_from = show3_list_start;
	show3_list_to = end;
	if (show3_part_jumped)
	{
		/* in two pieces, the list's start and the part above: the next
		frame waits for this one's show */
		show3_list_from = 0;
		show3_list_to = (unsigned int)display_list_size;
	}
	show3_part_jumped = 0;
	show3_part_end = 0;
	if ((unsigned int)sceGuCheckList() > show3_list_peak)
		show3_list_peak = (unsigned int)sceGuCheckList();
	else
		show3_list_peak -= show3_list_peak / 64;
	show3_buffer = draw_buffer;
	psp_display_hold = 1;
	draw_buffer = sceGuSwapBuffers();
	psp_display_hold = 0;
	show2_asked = 1;
	sceKernelWakeupThread(inflight_thread);
}

/* geshow3: the GE at the finish of the frame being shown. The event flag is
a wake only: the finish's id is what's waited for, looked at every 2 ms
whatever the flag does */
static PSP_MAIN_SIDE unsigned long show3_given_up = 0;

static void show3_wait_finish(unsigned int id)
{
	int looks = 0;

	while (show3_finished != id)
	{
		SceUInt timeout = 2000;

		/* a finish never reported (no frame takes 400 ms on the GE): the
		show goes on rather than the game stopping, and it is counted */
		if (++looks > 200)
		{
			if (!show3_given_up++)
				platform_log("GE: geshow3: the GE's finish %u never came (last %u); shown without it", id, show3_finished);
			return;
		}
		sceKernelWaitEventFlag(show3_flag, 1, PSP_EVENT_WAITOR | PSP_EVENT_WAITCLEAR, NULL, &timeout);
	}
}

/* geshow3: this frame's list, in the part of the display list the frame out
with the GE doesn't use (the larger of the two sides), if that holds well
over the largest list lately; else it waits for that show and takes it all */
static void show3_list_begin(void)
{
	unsigned int size = (unsigned int)display_list_size, at = 0, room = size, need, low, high;

	need = show3_list_peak + show3_list_peak / 4 + 32 * 1024;
	if (need < SHOW3_ROOM_LEAST)
		need = SHOW3_ROOM_LEAST;
	if (show2_owed)
	{
		low = show3_list_from & ~63u;
		at = (show3_list_to + 63) & ~63u;
		high = at < size ? size - at : 0;
		if (low > high)
		{
			at = 0;
			high = low;
		}
		room = high;
		if (room < need)
		{
			show2_settle(SHOW3_ROOM);
			at = 0;
			room = size;
		}
	}
	if (show2_owed)
		show3_rooms_shared++;
	else
		show3_rooms_whole++;
	if (show3_check)
	{
		show3_checked[SHOW3_LIST]++;
		if (show2_owed && at < show3_list_to && at + room > show3_list_from)
			show3_reused[SHOW3_LIST]++;
	}
	/* the whole list's limits: the part's own end is show3_room's */
	list_bytes = (int)size;
	show3_list_start = at;
	show3_part_jumped = 0;
	show3_part_end = 0;
	show3_part_grow = SHOW3_GROW_NONE;
	if (show2_owed)
	{
		unsigned int list = (unsigned int)display_list & ~0x40000000u;

		show3_part_end = list + at + room;
		show3_part_base = list;
		if (at == 0)
		{
			show3_part_grow = SHOW3_GROW_UP;
			show3_part_next = list + size;
		}
		else
		{
			show3_part_grow = SHOW3_GROW_JUMP;
			show3_part_next = list + at;
		}
		SHOW3_GE_WORD[1] = show3_asked & 0xFFFF;
	}
	sceGuStart(GU_DIRECT, (void *)((unsigned int)display_list + at));
	list_cached_frame_start(0);
	if (show2_owed)
	{
		show3_gate_stall = NULL;
		show3_gate_show = show3_asked;
		__asm__ volatile("" ::: "memory");
		show3_gate_qid = psp_ge_enqueued;
	}
	else
		show3_gate_qid = -1;
}

/* the list a frame draws into: here at its window's start, or under geshow2
by the forwarding thread once the last frame is shown */
static void show2_list_start(void)
{
	sceGuStart(GU_DIRECT, list_in_vram ? (void *)(0x04000000 + VRAM_BENCH_START) : (void *)display_list);
	list_cached_frame_start(list_in_vram);
}

static float window_begin_main(const float *position, float z_near, float z_far)
{
	window_main_done = 1;
	if (show_lat_on < 0)
		show_lat_on = psp_keep("showlat") != 0;
	if (show_lat_on)
		frame_begin_us = sceKernelGetSystemTimeLow();
	if (!ge_ready)
	{
		ge_initialize();
		if (psp_keep("geprecision"))
			ge_precision_test();
	}
	{
		/* build 113: "shoteveryN": a screenshot every N seconds of game time,
		at most 24 (emulator runs have no hands for select + right). Asked
		for as the frame begins, so "tdump" lists the frame's see-through
		draws too. "shotrunN": each of those moments takes N frames in a
		row (at most 72 shots then), so a frame-to-frame change (a wobble,
		a flicker) can be told from motion by comparing neighbours.
		"shotfromN": none before N seconds of game time, so the shots can
		cover a stretch late in a map (the PC reference's rule, so a PSP shot
		and a PC frame share their ticks) */
		extern volatile int psp_screenshot_requested;
		extern int psp_keep_number(const char *prefix, int fallback);
		extern long psp_game_time_peek(void);
		static int shot_every = -2, shot_run = 1, shot_run_left = 0, shots_asked = 0, shots_most = 24;
		static long shot_next = -1, shot_from = 0;

		if (shot_every == -2)
		{
			shot_every = psp_keep_number("shotevery", 0);
			shot_from = (long)psp_keep_number("shotfrom", 0) * 30;
			shot_run = psp_keep_number("shotrun", 1);
			if (shot_run < 1)
				shot_run = 1;
			if (shot_run > 1)
				shots_most = 72;
		}
		if (shot_every > 0 && shots_asked < shots_most)
		{
			long now = psp_game_time_peek(), period = (long)shot_every * 30;

			if (shot_next < 0 && shot_from > 0)
				shot_next = shot_from;
			if (shot_next >= 0 && now >= shot_next)
				shot_run_left = shot_run;
			if (shot_run_left > 0)
			{
				psp_screenshot_requested = 1;
				shots_asked++;
				shot_run_left--;
			}
			if (shot_next < 0 || now >= shot_next)
				shot_next = (now / period + 1) * period;
		}
	}
	defer_frame_begin(); /* build 112 */
	if (vm_on < 0)
		vm_reset(); /* build 116: vrammodels, after a new map or BSP */
	/* the list's start hands it to the GE (a system call): the main CPU's,
	before any of the frame's draws are carried out */
	/* build 93: the list in VRAM for benchrun's switch (from the next frame) */
	list_bytes = list_in_vram ? VRAM_BENCH_BYTES : display_list_size;
	if (show2_list_started)
		show2_list_started = 0; /* geshow2: started by the show, after the swap */
	else if (show3_on && !list_in_vram)
		show3_list_begin();
	else
		show2_list_start();
	{
		/* build 49: the depth buffer is 16 bits (the Xbox's 24), and a
		perspective depth spends most of its steps near the eye: the step at
		distance z is about z * z / (near * 61439). At the game's near plane
		a surface 30 units off resolves only to a few tenths of a unit, so
		overlapping surfaces far off flicker and a rock's buried base pokes
		through the ground (b48). "near2" / "near4" move the world's near
		plane out 2 or 4 times (2 or 4 times finer everywhere; the price:
		walls closer than the new plane are cut open). The first-person
		weapon has its own projection and depth range */
		if (near_scale < 0)
		{
			near_scale = psp_keep("near4") ? 4 : psp_keep("near2") ? 2 : 1;
			near_auto = psp_keep("nearauto");
			platform_log("GE: the game's near plane %.4f, far %.1f; world near plane x%d%s", z_near, z_far, near_scale,
				near_auto ? "; nearauto: x4, x2 or x1 by what is near the eye" : "");
		}
		/* build 108: nearauto only in a game. Between maps (a load's
		frames, the error loop when a map is missing) the level and the
		player are gone, and its tests read them (b107's b30 run: a bus
		error at the level's end); the game's own plane then */
		/* the main menu's near plane x4, whatever nearauto would choose: the
		ring's panels overlap and the 16-bit depth could not tell them apart,
		so they cut each other open. The menu's camera never
		comes within the wider plane of anything, and the showcase rule keeps
		it off every level. "showcasenearoff": the levels' own choice */
		if (showcase_near < 0)
			showcase_near = !psp_keep("showcasenearoff");
		if (psp_showcase_map && showcase_near)
			z_near *= 4;
		else if (near_auto && psp_game_running())
			z_near = near_plane_auto(position, z_near);
		else if (!near_auto)
			z_near *= near_scale;
	}
	if (level_exact < 0)
		level_exact = !psp_keep("levelexactoff");
	if (level_local < 0)
		level_local = !psp_keep("levellocaloff");
	if (level_local)
		level_origin_follow(position);
	return z_near;
}

static void window_begin_list(const float *position, const float *forward, const float *up,
	float vertical_field_of_view, float z_near, float z_far)
{
	ScePspFVector3 eye, center, upward;

	if (frame_open)
		return;
	lit_forget(2); /* build 114: statememo */
	sceGuFog(fog_near, fog_far, fog_color);
	list_limit = list_bytes - HUD_RESERVE - TRANSPARENT_RESERVE;
	if (vram_busy)
		vram_busy_load();
	hud_phase = 0;
	blip_depth_live = 0;
	ge_kinds_frame_begin(); /* build 103 */
	taa_done = 0;
	taa_frame++;
	if (psp_taa_strength > 0 && psp_taa_jitter && !psp_soften && !taa_failed)
	{
		/* a quarter pixel each way on alternate frames */
		float sign = (taa_frame & 1) ? 0.25f : -0.25f;

		taa_jitter_x = sign * 2.0f / SCREEN_WIDTH;
		taa_jitter_y = sign * 2.0f / SCREEN_HEIGHT;
	}
	else
		taa_jitter_x = taa_jitter_y = 0.0f;
	if (magenta < 0)
		magenta = psp_keep("clearmagenta");
	if (clear_blue < 0)
		clear_blue = psp_keep("clearblue");
	sceGuClearColor(magenta ? 0xFFFF00FF : clear_blue ? SKY_COLOR : window_clear_color);
	sceGuClearDepth(0);
	sceGuClear(GU_COLOR_BUFFER_BIT | GU_DEPTH_BUFFER_BIT);
	ge_dither(1); /* build 116: the world's draws */
	/* build 63: the world's depth state from a known start each frame, so a
	slip the frame before (a test left off, a nudge left on) can't carry */
	sceGuEnable(GU_DEPTH_TEST);
	sceGuDepthFunc(GU_GEQUAL);
	sceGuDepthOffset(0);

	near_clip = z_near;
	sceGumMatrixMode(GU_PROJECTION);
	perspective(vertical_field_of_view * (180.0f / 3.14159265f), z_near, z_far);
	projection_fov = vertical_field_of_view;
	projection_near = z_near;
	projection_far = z_far;
	model_first_person = 0;
	model_near = 0;
	model_sphere_radius = 0.0f; /* build 118: a new camera; the next model sets its own */
	/* the clipping against the near plane, every frame (the GE's state may
	not survive the PSP sleeping) */
	sceGuEnable(GU_CLIP_PLANES);
	view_far = z_far;
	eye.x = position[0]; eye.y = position[1]; eye.z = position[2];
	center.x = eye.x + forward[0]; center.y = eye.y + forward[1]; center.z = eye.z + forward[2];
	upward.x = up[0]; upward.y = up[1]; upward.z = up[2];
	/* camera-relative: the GE keeps its matrices as 24-bit floats, so a
	view matrix carrying the camera's world position (a hundred units out on
	Blood Gulch) and a model matrix carrying the model's lost about a
	millimetre each; next to the camera that is several pixels, and the
	first-person weapon crumpled as its bones moved. So the view is the
	camera's turn plus the fraction between the eye and ge_eye, and every
	model matrix is made relative to ge_eye on the CPU, in full floats
	(relative(), world_model()) */
	{
		int k;

		/* from level_origin, the level's translation (its origin less
		ge_eye) is the rounded part itself, whole in the GE's words however
		near the world's origin the eye is */
		for (k = 0; k < 3; k++)
			ge_eye[k] = !level_exact ? position[k] :
				level_local ? level_origin[k] + ge_matrix_float(position[k] - level_origin[k]) : ge_matrix_float(position[k]);
	}
	{
		ScePspFVector3 origin = { 0.0f, 0.0f, 0.0f }, ahead;

		ahead.x = forward[0]; ahead.y = forward[1]; ahead.z = forward[2];
		sceGumMatrixMode(GU_VIEW);
		sceGumLoadIdentity();
		sceGumLookAt(&origin, &ahead, &upward);
		if (level_exact)
		{
			/* a point given from ge_eye, moved to be given from the eye */
			ScePspFVector3 shift;

			shift.x = ge_eye[0] - position[0];
			shift.y = ge_eye[1] - position[1];
			shift.z = ge_eye[2] - position[2];
			sceGumTranslate(&shift);
		}
	}
	(void)center;
	frame_open = 1;
	/* inside the frame's list, so the GE takes it */
	showcase_levels();
	draws_this_frame = triangles_this_frame = clipped_this_frame = culled_this_frame = 0;
	model_draws_this_frame = 0;

	/* the camera's directions, for classifying triangles */
	{
		float length;

		camera_eye[0] = eye.x; camera_eye[1] = eye.y; camera_eye[2] = eye.z;
		camera_forward[0] = forward[0]; camera_forward[1] = forward[1]; camera_forward[2] = forward[2];
		/* right = forward x up */
		camera_right[0] = forward[1] * up[2] - forward[2] * up[1];
		camera_right[1] = forward[2] * up[0] - forward[0] * up[2];
		camera_right[2] = forward[0] * up[1] - forward[1] * up[0];
		length = sqrtf(camera_right[0] * camera_right[0] + camera_right[1] * camera_right[1] + camera_right[2] * camera_right[2]);
		if (length > 0.0f)
		{
			camera_right[0] /= length; camera_right[1] /= length; camera_right[2] /= length;
		}
		/* up = right x forward */
		camera_up[0] = camera_right[1] * forward[2] - camera_right[2] * forward[1];
		camera_up[1] = camera_right[2] * forward[0] - camera_right[0] * forward[2];
		camera_up[2] = camera_right[0] * forward[1] - camera_right[1] * forward[0];
		{
			/* a pixel's width at depth 1 (pixels are square: the projection's
			aspect is the screen's) */
			float pixel = tanf(vertical_field_of_view * 0.5f) / (SCREEN_HEIGHT / 2);

			if (guard_wide)
				guard_x = guard_y = GUARD_PIXELS * pixel;
			else
			{
				guard_y = GUARD * tanf(vertical_field_of_view * 0.5f);
				guard_x = guard_y * (float)SCREEN_WIDTH / SCREEN_HEIGHT;
			}
			screen_x = (SCREEN_WIDTH / 2 + SCREEN_MARGIN_PIXELS) * pixel;
			screen_y = (SCREEN_HEIGHT / 2 + SCREEN_MARGIN_PIXELS) * pixel;
		}
		guard_x_length = sqrtf(1.0f + guard_x * guard_x);
		guard_y_length = sqrtf(1.0f + guard_y * guard_y);
	}
	{
		float d[3];

		d[0] = ge_eye[0] - camera_eye[0]; d[1] = ge_eye[1] - camera_eye[1]; d[2] = ge_eye[2] - camera_eye[2];
		ge_eye_camera[0] = d[0] * camera_right[0] + d[1] * camera_right[1] + d[2] * camera_right[2];
		ge_eye_camera[1] = d[0] * camera_up[0] + d[1] * camera_up[1] + d[2] * camera_up[2];
		ge_eye_camera[2] = d[0] * camera_forward[0] + d[1] * camera_forward[1] + d[2] * camera_forward[2];
	}
	sceGumMatrixMode(GU_MODEL);
	world_model();
}

void psp_ge_window_begin(const float *position, const float *forward, const float *up,
	float vertical_field_of_view, float z_near, float z_far)
{
	rso_window();
	if (!(z_near > 0.01f))
		z_near = 0.05f;
	if (!(z_far > z_near))
		z_far = 1000.0f;
	/* geinflight: the main CPU's part now, the list's written down for the
	frame's turn */
	if (!window_main_done)
		inflight_room_check(0);
	if (INFLIGHT_RECORDING())
	{
		/* the frame waiting goes first: its list reads what the main
		CPU's part changes (the level's origin, the recorder's frame) */
		if (inflight_pending)
			inflight_sync();
		if (!window_main_done)
			z_near = window_begin_main(position, z_near, z_far);
		if (defer_window_begin(position, forward, up, vertical_field_of_view, z_near, z_far))
		{
			inflight_window_written = 1;
			return;
		}
	}
	/* B41: the frame after a switch press isn't in flight; the one before it
	may still be out with the ME, whose replay sets frame_open: it is carried
	out first, as the in-flight branch above would */
	if (psp_sleep_frame && (inflight_pending || inflight_me_out))
		inflight_sync();
	if (frame_open)
		return;
	GE_DEFER_DRAIN(); /* build 112: gedefer */
	z_near = window_begin_main(position, z_near, z_far);
	if (!show3_on)
		show2_settle(SHOW2_OTHER); /* geshow2: the list part runs now, into the list the show starts */
	window_begin_list(position, forward, up, vertical_field_of_view, z_near, z_far);
}

/* a point in camera space: x right, y up, z forward (distance ahead) */
static inline void to_camera_from(const float *eye, const float *p, float *out)
{
	float d0 = p[0] - eye[0], d1 = p[1] - eye[1], d2 = p[2] - eye[2];

	out[0] = d0 * camera_right[0] + d1 * camera_right[1] + d2 * camera_right[2];
	out[1] = d0 * camera_up[0] + d1 * camera_up[1] + d2 * camera_up[2];
	out[2] = d0 * camera_forward[0] + d1 * camera_forward[1] + d2 * camera_forward[2];
}

/* a world position */
static inline void to_camera(const float *p, float *out)
{
	to_camera_from(camera_eye, p, out);
}

/* a level copy's position, given from the origin level_frame was given */
static inline void level_to_camera(const float *p, float *out)
{
	to_camera_from(level_eye, p, out);
}

/* a level corner to be cut: camera space for the clipping, the copy's
position carried along for drawing through the copy's matrix, so the cut
corners the cut keeps are the very floats the uncut neighbours have */
static inline void level_to_camera_corner(const float *p, float *corner)
{
	level_to_camera(p, corner);
	corner[7] = p[0];
	corner[8] = p[1];
	corner[9] = p[2];
}

/* which of the five planes a camera-space point is outside of, as bits:
near, right, left, top, bottom */
static inline int outside(const float *v)
{
	int bits = 0;

	if (v[2] < NEAR_CLIP) bits |= 1;
	if (v[0] > guard_x * v[2]) bits |= 2;
	if (v[0] < -guard_x * v[2]) bits |= 4;
	if (v[1] > guard_y * v[2]) bits |= 8;
	if (v[1] < -guard_y * v[2]) bits |= 16;
	return bits;
}

/* build 42: a model corner the GE can't take: behind the near plane (with
a margin), or so far to a side that its screen position overflows the GE's
2048 pixels, where it wraps round and the triangle becomes a long spike
across the screen (the Warthog's roll bars and axles passing the camera) */
static inline int camera_corner_out(float x, float y, float z, float limit)
{
	return z < limit || x > guard_x * z || x < -guard_x * z || y > guard_y * z || y < -guard_y * z;
}

/* build 118: whether a camera-space sphere lies wholly inside the planes
the corner tests use (outside(), camera_corner_out()): its nearest depth at
or past near, and its centre inside each side plane by its radius (those
planes pass through the eye; their normals, as outside() writes them, are
guard_x_length and guard_y_length long). The radius gets 1% and 0.01 units
more for the rounding between the corners' transform and the centre's.
Written so a NaN fails. transparent_cut's model_sphere_inside and the near
path's batch_sphere_inside both ask this */
static int sphere_inside(const float *c, float radius, float near)
{
	float r = radius * 1.01f + 0.01f;

	return radius >= 0.0f && c[2] - r >= near &&
		fabsf(c[0]) + r * guard_x_length <= guard_x * c[2] &&
		fabsf(c[1]) + r * guard_y_length <= guard_y * c[2];
}

/* each vertex's verdict for the draw at hand: corner_stamp in, +1 out,
anything else not yet worked out (a stamp, so nothing is cleared per draw) */
/* build 44: 16K entries (32 KB, was 128 KB): a model buffer's vertices
number a few thousand at most; a corner past the table is worked out
each time */
#define CORNER_VERDICTS 8192 /* build 64: 8K (16 KB; was 16K), room for the angle buckets */
static unsigned short corner_verdict[CORNER_VERDICTS];
static unsigned short corner_stamp = 0;

/* build 60: a stamp a draw, 32 apart, so a verdict can be the stamp plus a
corner's five plane bits (rigid parts) or plus 0 / 1 (skinned ones) */
/* build 106: 64 (was 32): a bending part's corners keep six bits (build
106's model_geometry), a rigid one's five */
#define CORNER_STAMP_STEP 64
static void corner_verdict_reset(void)
{
	corner_stamp += CORNER_STAMP_STEP;
	if (corner_stamp < CORNER_STAMP_STEP || corner_stamp > 0xFFFF - 2 * CORNER_STAMP_STEP)
	{
		memset(corner_verdict, 0xFF, sizeof(corner_verdict));
		corner_stamp = CORNER_STAMP_STEP;
	}
}

/* the same for a position in the GE's world space (from ge_eye) */
static inline int model_corner_out(float x, float y, float z, float limit)
{
	return camera_corner_out(x * camera_right[0] + y * camera_right[1] + z * camera_right[2] + ge_eye_camera[0],
		x * camera_up[0] + y * camera_up[1] + z * camera_up[2] + ge_eye_camera[1],
		x * camera_forward[0] + y * camera_forward[1] + z * camera_forward[2] + ge_eye_camera[2], limit);
}

/* a model-space point through a part's camera-relative matrix into camera
space (build 60) */
/* build 107: a rigid part's camera transform, one product (psp_ge_draw_model_body) */
static float rigid_rows[3][4];
static int near_check = -1;
static unsigned long near_check_corners = 0, near_check_differ = 0;

static inline void model_to_camera(const ScePspFMatrix4 *m, const float *p, float *out)
{
	float x = m->x.x * p[0] + m->y.x * p[1] + m->z.x * p[2] + m->w.x;
	float y = m->x.y * p[0] + m->y.y * p[1] + m->z.y * p[2] + m->w.y;
	float z = m->x.z * p[0] + m->y.z * p[1] + m->z.z * p[2] + m->w.z;

	out[0] = x * camera_right[0] + y * camera_right[1] + z * camera_right[2] + ge_eye_camera[0];
	out[1] = x * camera_up[0] + y * camera_up[1] + z * camera_up[2] + ge_eye_camera[1];
	out[2] = x * camera_forward[0] + y * camera_forward[1] + z * camera_forward[2] + ge_eye_camera[2];
}

/* "farcut" (on; farcutoff: as before): the GE clips at the near plane
only. A level triangle across the far clip is drawn whole, past it, and one
wholly past it is dropped whole, so where the far ocean meets the sky the
edge was a row of whole triangles, some reaching into the sky's band and
some missing, that changed as the view moved (b30's flight on the PSP:
a PSP run against the emulator's frames at the same camera,
which clip each pixel at the far plane as the Xbox does). Such a triangle is
cut there and its near piece drawn; one wholly past it is left out. The bits
sit above outside()'s five, so every other decision reads CODE_PLANES as
before */
#define CODE_PLANES 0x1F
#define CODE_PAST 0x20
#define CODE_SHORT 0x40
#define CODE_FAR (CODE_PAST | CODE_SHORT)
#define CLIP_INNER 0x20 /* clip_triangle: keeps the side short of the far clip */
static int far_cut = -1, far_cut_active = 0;
static float far_cut_at = 0.0f;
static unsigned long far_cut_triangles = 0, far_cut_boxes = 0, far_cut_dropped = 0;

/* once a draw, from the far clip the frame has set */
static inline void far_cut_begin(void)
{
	far_cut_active = far_cut > 0 && view_far > 0.0f;
	far_cut_at = view_far;
}

static inline int far_cut_code(float z)
{
	return !far_cut_active ? 0 : z > far_cut_at ? CODE_PAST : CODE_SHORT;
}

/* the signed distance of a point inside one plane (positive inside) */
static inline float plane_distance(const float *v, int plane)
{
	switch (plane)
	{
	case 0: return v[2] - NEAR_CLIP;
	case 1: return guard_x * v[2] - v[0];
	case 2: return guard_x * v[2] + v[0];
	case 3: return guard_y * v[2] - v[1];
	case 5: return far_cut_at - v[2];
	default: return guard_y * v[2] + v[1];
	}
}

/* a cut corner: camera-space position (0-2), two pairs of texture
coordinates or other values (3-6), its world position (7-9), and one more
value (10), a model corner's normal's third part */
#define CORNER_FLOATS 11
/* the fixed buffers' rooms, in corners: the level's (shared by the lights,
shadows, stencils and the level's see-through draws) and the models' */
#define MAXIMUM_CUT_CORNERS 384
#define MAXIMUM_MODEL_CUT_CORNERS 192

/* Where a draw's cut pieces wait, as floats, until its path turns them into
GE vertices. The first corners go into a fixed buffer (the scratchpad's for
the models, a static one for the level); a draw cut into more pieces than
that holds carries on into blocks taken from the display list, the frame's
own memory, which the GE never reads (sceGuGetMemory jumps over them) and
which the next frame reuses. So no piece is dropped while the list has room:
with the buffer alone the pieces past it were lost without a trace, and a
Pelican's bench lost the wall behind the player (cut_pieces_lost).
"cutstreamoff": the buffer alone, the rest lost */
#define CUT_SPILL_CORNERS 96 /* a multiple of 3, as the buffers' rooms are, so a piece never straddles two blocks */
struct cut_spill
{
	struct cut_spill *next;
	int unused;
	float corners[CUT_SPILL_CORNERS][CORNER_FLOATS];
};
enum { CUT_LEVEL, CUT_RIGID, CUT_SKIN, CUT_SEE, CUT_OTHER, CUT_KINDS };
struct cut_store
{
	float (*head)[CORNER_FLOATS];
	/* dropped: triangles and pieces left out wholly off the screen. A draw's
	decisions that asked whether it had cut pieces (whether to draw at all,
	whether it reaches the detail fade) count them too, so they come out as
	they did when those pieces were drawn */
	int head_room, count, lost, dropped, kind;
	struct cut_spill *first, *last;
	/* the block cut_at read last, and the index of its first corner past head */
	struct cut_spill *read;
	int read_base;
};
static int cut_stream = 1;
static unsigned long cut_pieces_lost = 0, cut_spill_blocks = 0;

static void cut_store_begin(struct cut_store *store, float (*head)[CORNER_FLOATS], int head_room, int kind)
{
	store->head = head;
	store->head_room = head_room;
	store->count = store->lost = store->dropped = 0;
	store->kind = kind;
	store->first = store->last = store->read = NULL;
	store->read_base = 0;
}

/* A spill block is display-list memory, which the next frame writes again
(through the uncached view, or the cached one after "cache 0x18"), so no
line of it may stay dirty in the data cache, to be written back over that
later: list_take's own write-back only covers what is written before the
list's next hand-over, and a block is filled over many pieces. Each write to
a block is written back and dropped at once (only past the fixed buffer) */
static void cut_spill_settle(const void *start, int bytes)
{
	unsigned int line = (unsigned int)start & ~63u, end = (unsigned int)start + bytes;

	for (; line < end; line += 64)
		__builtin_allegrex_cache(0x1B, line);
}

static float (*cut_piece_spilled(struct cut_store *store))[CORNER_FLOATS]
{
	int past = store->count - store->head_room, at = past % CUT_SPILL_CORNERS;

	if (!at)
	{
		struct cut_spill *block = cut_stream ? list_take(sizeof(struct cut_spill)) : NULL;

		if (!block)
			return NULL;
		block->next = NULL;
		cut_spill_settle(&block->next, sizeof(block->next));
		if (store->last)
		{
			store->last->next = block;
			cut_spill_settle(&store->last->next, sizeof(block->next));
		}
		else
			store->first = block;
		store->last = block;
		cut_spill_blocks++;
	}
	store->count += 3;
	return store->last->corners + at;
}

/* room for one piece's three corners, or NULL when there is none */
static inline float (*cut_piece(struct cut_store *store))[CORNER_FLOATS]
{
	if (store->count + 3 <= store->head_room)
	{
		float (*piece)[CORNER_FLOATS] = store->head + store->count;

		store->count += 3;
		return piece;
	}
	return cut_piece_spilled(store);
}

static const float *cut_at_spilled(struct cut_store *store, int index)
{
	int past = index - store->head_room;

	if (!store->read || past < store->read_base)
	{
		store->read = store->first;
		store->read_base = 0;
	}
	while (past >= store->read_base + CUT_SPILL_CORNERS)
	{
		store->read = store->read->next;
		store->read_base += CUT_SPILL_CORNERS;
	}
	return store->read->corners[past - store->read_base];
}

/* corner index of the store, in the order they were written */
static inline const float *cut_at(struct cut_store *store, int index)
{
	return index < store->head_room ? store->head[index] : cut_at_spilled(store, index);
}

/* "cutcount": what the cutting does, by kind, in the 300-frame log. Which
planes a triangle sent to be cut crossed: the near plane, or only the guard
planes (which the GE could have drawn whole if they sat at its own limit);
those wholly off the screen; the pieces of the rest, those wholly off the
screen, and those lost for want of room; the most corners one draw needed in
a frame (the buffer's room is the most it held) */
static int cut_count_on = 0;
static struct cut_tally
{
	unsigned long near, guard_only, far_end, whole_off, pieces, off_screen, lost, peak, frames_over;
} cut_tally[CUT_KINDS];
static int cut_frame_peak[CUT_KINDS];
static const char *const cut_kind_names[CUT_KINDS] = { "level", "rigid parts", "bending parts", "see-through parts", "other (lights, shadows, stencils, level see-through)" };

/* whether a triangle lies wholly past one of the screen's side planes. Each
plane passes through the eye, so a triangle whose three corners are on its
outer side lies wholly in that half-space, whatever their depths (a corner
behind the eye included): no pixel of it, nor of any piece cut from it, is
on the screen */
static inline int piece_off_screen(const float *a, const float *b, const float *c)
{
	return (a[0] > screen_x * a[2] && b[0] > screen_x * b[2] && c[0] > screen_x * c[2]) ||
		(a[0] < -screen_x * a[2] && b[0] < -screen_x * b[2] && c[0] < -screen_x * c[2]) ||
		(a[1] > screen_y * a[2] && b[1] > screen_y * b[2] && c[1] > screen_y * c[2]) ||
		(a[1] < -screen_y * a[2] && b[1] < -screen_y * b[2] && c[1] < -screen_y * c[2]);
}

/* cuts a camera-space triangle to the inside of the planes it crosses
(bits, outside()'s), every value of each corner cut alike. The pieces, a
fan, go into store as a triangle list; returns how many corners it added.
Most triangles sent here reach past the screen's side (the guard planes are
6 screens out), and most of their pieces land wholly off the screen.
By default a triangle wholly off it is not cut at all, and a piece wholly
off it is not stored. Neither should draw a pixel, yet the emulator's frames
move by one or two colour steps in scattered pixels, for reasons not yet
found (docs/review_cutstream.md). the tester saw no difference on the PSP and
chose it on for the frame rate (a10 crosses 15 fps with it); "cutlessoff"
cuts them as before */
static int clip_triangle(float polygon[3][CORNER_FLOATS], int bits, struct cut_store *store)
{
	float buffer[2][12][CORNER_FLOATS];
	int count = 3, plane, index, source = 0, added = 0, k;
	int less = cut_less && (cut_less_mask >> store->kind & 1);

	if (cut_count_on)
	{
		struct cut_tally *tally = &cut_tally[store->kind];

		if (bits & 1)
			tally->near++;
		else if (bits & 0x1E)
			tally->guard_only++;
		else
			tally->far_end++;
	}
	if ((less || cut_count_on) && piece_off_screen(polygon[0], polygon[1], polygon[2]))
	{
		if (cut_count_on)
			cut_tally[store->kind].whole_off++;
		if (less)
		{
			store->dropped++;
			return 0;
		}
	}
	memcpy(buffer[0], polygon, 3 * sizeof(polygon[0]));
	for (plane = 0; plane < 6 && count >= 3; plane++)
	{
		int kept = 0;

		if (!(bits & (1 << plane)))
			continue;
		for (index = 0; index < count; index++)
		{
			float *a = buffer[source][index], *b = buffer[source][(index + 1) % count];
			float da = plane_distance(a, plane), db = plane_distance(b, plane);

			if (da >= 0.0f && kept < 12)
				memcpy(buffer[!source][kept++], a, sizeof(buffer[0][0]));
			if ((da >= 0.0f) != (db >= 0.0f) && kept < 12)
			{
				float t = da / (da - db);

				for (k = 0; k < CORNER_FLOATS; k++)
					buffer[!source][kept][k] = a[k] + (b[k] - a[k]) * t;
				kept++;
			}
		}
		count = kept;
		source = !source;
	}
	if (cut_count_on && count >= 3)
		cut_tally[store->kind].pieces += (unsigned long)(count - 2);
	for (index = 1; index + 1 < count; index++)
	{
		const float *a = buffer[source][0], *b = buffer[source][index], *c = buffer[source][index + 1];
		float (*piece)[CORNER_FLOATS];

		if ((less || cut_count_on) && piece_off_screen(a, b, c))
		{
			if (cut_count_on)
				cut_tally[store->kind].off_screen++;
			if (less)
			{
				store->dropped++;
				continue;
			}
		}
		piece = cut_piece(store);
		if (!piece)
		{
			/* a hole in whatever is at the eye: counted, it should stay 0 */
			store->lost++;
			cut_pieces_lost++;
			if (cut_count_on)
				cut_tally[store->kind].lost++;
			continue;
		}
		memcpy(piece[0], a, sizeof(buffer[0][0]));
		memcpy(piece[1], b, sizeof(buffer[0][0]));
		memcpy(piece[2], c, sizeof(buffer[0][0]));
		if (store->count > store->head_room)
			cut_spill_settle(piece, 3 * sizeof(buffer[0][0]));
		added += 3;
	}
	if (cut_count_on && store->count + 3 * store->lost > cut_frame_peak[store->kind])
		cut_frame_peak[store->kind] = store->count + 3 * store->lost;
	clipped_this_frame++;
	return added;
}

/* a triangle across the far clip: its near piece, also cut to the planes it crosses */
static int clip_at_far(float polygon[3][CORNER_FLOATS], int planes, struct cut_store *store)
{
	far_cut_triangles++;
	return clip_triangle(polygon, planes | CLIP_INNER, store);
}

/* a triangle stored as it is (cut_piece's room, counted as a cut's) */
static int cut_store_whole(float polygon[3][CORNER_FLOATS], struct cut_store *store)
{
	float (*piece)[CORNER_FLOATS] = cut_piece(store);

	if (!piece)
	{
		store->lost++;
		cut_pieces_lost++;
		if (cut_count_on)
			cut_tally[store->kind].lost++;
		return 0;
	}
	memcpy(piece, polygon, 3 * sizeof(polygon[0]));
	if (store->count > store->head_room)
		cut_spill_settle(piece, 3 * sizeof(polygon[0]));
	if (cut_count_on && store->count + 3 * store->lost > cut_frame_peak[store->kind])
		cut_frame_peak[store->kind] = store->count + 3 * store->lost;
	return 3;
}

/* at each frame's end: each kind's most corners in one draw this frame */
static void cut_count_frame_end(void)
{
	int kind;

	if (!cut_count_on)
		return;
	for (kind = 0; kind < CUT_KINDS; kind++)
	{
		struct cut_tally *tally = &cut_tally[kind];

		if ((unsigned long)cut_frame_peak[kind] > tally->peak)
			tally->peak = (unsigned long)cut_frame_peak[kind];
		if (cut_frame_peak[kind] > (kind == CUT_LEVEL || kind == CUT_OTHER ? MAXIMUM_CUT_CORNERS : MAXIMUM_MODEL_CUT_CORNERS))
			tally->frames_over++;
		cut_frame_peak[kind] = 0;
	}
}

static void cut_count_report(unsigned long frame_count)
{
	int kind;

	if (!cut_count_on)
		return;
	for (kind = 0; kind < CUT_KINDS; kind++)
	{
		struct cut_tally *tally = &cut_tally[kind];

		platform_log("GE: cutcount %s, %lu frames: triangles cut %lu at the near plane, %lu at guard planes only, %lu of them wholly off the screen%s; pieces %lu, %lu wholly off the screen%s, %lu lost; most corners in one draw %lu (room %d), frames over the room %lu",
			cut_kind_names[kind], frame_count, tally->near, tally->guard_only, tally->whole_off,
			cut_less ? " (not cut)" : "", tally->pieces, tally->off_screen, cut_less ? " (left out)" : " (drawn)",
			tally->lost, tally->peak,
			kind == CUT_LEVEL || kind == CUT_OTHER ? MAXIMUM_CUT_CORNERS : MAXIMUM_MODEL_CUT_CORNERS, tally->frames_over);
	}
	platform_log("GE: cutcount: %s, guard planes %s (x %.2f, y %.2f), %lu spill blocks taken",
		cut_stream ? "cut pieces streamed past the buffers" : "cutstreamoff: the fixed buffers alone",
		guard_wide ? "at the GE's limit (guardwide)" : "at 6 screens", guard_x, guard_y, cut_spill_blocks);
	memset(cut_tally, 0, sizeof(cut_tally));
	cut_spill_blocks = 0;
}

/* ---------- the level, textured (build 20)

The level's own vertices can't carry texture coordinates to the GE (in the
game's layout they come after the position, and the GE wants them before),
and the CPU reads them (visibility, object lighting), so they stay as they
are. For the GE, each material's vertices are copied once, the first time
it is drawn, into two streams: base texture coordinates with the position,
and lightmap coordinates with the position (20 bytes each).

A lit surface is two passes: the base texture, then the lightmap multiplied
over it (where the depth is exactly the first pass's, so alpha-tested holes
stay holes). Fog: the first pass fades toward the fog colour and the second
toward white, so far away the result is the fog colour and near it is
base x lightmap. */

struct ge_vertex
{
	float u, v, x, y, z;
};

struct level_stream
{
	const void *source;
	struct ge_vertex *base, *lightmap;
	/* build 68: the packed normals (the map's 32-bit form), for the dynamic
	lights' reach; the copies are all the renderer has of a material once
	its bytes have left the cache */
	unsigned int *normals;
	/* build 89: the lightmap vertices' packed incident radiosity, for object
	lighting (object_lights.c via psp_ge_level_vertex_lighting; NULL without
	a lightmap) */
	unsigned int *radiosity;
	int count;
	/* build 68: the frame it was last drawn in, for letting the least used
	go when the copies are over their budget (a map built with its geometry
	outside the tags, psp_geo.c) */
	unsigned int last_frame;
	/* build 58: texture coordinate units per world unit (0: not yet worked out) */
	float uv_per_unit;
	/* build 66: the vertices' bounding box, for skipping the classification
	of a draw wholly inside the view */
	float low[3], high[3];
	/* build 87: the stencil bake's pieces for this material (NULL: none);
	count then includes the vertices the bake added */
	struct bake_wall *bake;
	int original_count;
	/* build 87: "levelcompact": the same vertices at 16 bits (texture
	coordinates times a scale, positions on a lattice; NULL: floats only) */
	struct compact_vertex *compact_base, *compact_lightmap;
	float compact_origin[3], compact_uv_scale[2];
	/* the point the positions of base and lightmap are given from (the world
	minus it); low and high stay in the world */
	float origin[3];
};
#define LEVEL_STREAMS 512

/* build 87: "levelcompact" (launch menu, or the debug menu's "Level
vertices"). Memory traffic is this port's crux (memory_traffic.md): the GE
reads every level vertex from RAM once a pass, 20 bytes each (two float
texture coordinates, three float positions), up to four passes a triangle.
At 16 bits a vertex is 10 bytes:
	- positions on one lattice of 1/256 unit (3.9 mm) for every material,
	  each material's origin a lattice point: the same world point lands on
	  the same lattice point whichever material holds it (no cracks), and the
	  GE's 16-bit position (q / 32768) times the model matrix's 128 plus the
	  origin is exact in floats. The float copies are snapped to the lattice
	  too, so the cut-up triangles, shadows, lights and stencils lie on the
	  very same points;
	- texture coordinates as q / 32768 times a per-material scale (the
	  material's largest coordinate), applied with sceGuTexScale; lightmap
	  coordinates are 16-bit in the map already.
A material wider than the lattice reaches (+-128 units around its origin)
stays in floats */
struct compact_vertex
{
	short u, v, x, y, z;
};
#define COMPACT_STEP (1.0f / 256.0f)
static int level_compact = -1;
static unsigned long compact_streams = 0, compact_refused = 0, compact_draws = 0;
/* the compact draw context of the level draw under way (NULL: floats) */
static const struct level_stream *compact_now = NULL;
static float pass_scale_u = 1.0f, pass_scale_v = 1.0f;
static void world_model(void);

/* the material's own model matrix (lattice to world, camera-relative) and
texture scale for a compact pass */
static void compact_begin(int lightmap)
{
	ScePspFMatrix4 m;
	float k = COMPACT_STEP * 32768.0f;

	gumLoadIdentity(&m);
	m.x.x = m.y.y = m.z.z = k;
	m.w.x = compact_now->compact_origin[0];
	m.w.y = compact_now->compact_origin[1];
	m.w.z = compact_now->compact_origin[2];
	relative(&m);
	sceGumMatrixMode(GU_MODEL);
	sceGumLoadMatrix(&m);
	if (lightmap)
		sceGuTexScale(pass_scale_u * (32768.0f / 32767.0f), pass_scale_v * (32768.0f / 32767.0f));
	else
		sceGuTexScale(pass_scale_u * compact_now->compact_uv_scale[0], pass_scale_v * compact_now->compact_uv_scale[1]);
	compact_draws++;
}

static void compact_end(void)
{
	level_model(compact_now->origin);
	sceGuTexScale(pass_scale_u, pass_scale_v);
}

/* builds a stream's 16-bit copies, snapping its float copies to the lattice;
0 when the material is too wide for the lattice (it stays in floats) */
static int compact_build(struct level_stream *stream)
{
	float low[3], high[3], umax = 1e-6f, vmax = 1e-6f;
	int i, axis;

	for (axis = 0; axis < 3; axis++)
	{
		low[axis] = stream->low[axis];
		high[axis] = stream->high[axis];
		stream->compact_origin[axis] = floorf((low[axis] + high[axis]) * 0.5f / COMPACT_STEP + 0.5f) * COMPACT_STEP;
		if (high[axis] - stream->compact_origin[axis] > 32760.0f * COMPACT_STEP ||
			stream->compact_origin[axis] - low[axis] > 32760.0f * COMPACT_STEP)
		{
			compact_refused++;
			return 0;
		}
	}
	for (i = 0; i < stream->count; i++)
	{
		if (fabsf(stream->base[i].u) > umax) umax = fabsf(stream->base[i].u);
		if (fabsf(stream->base[i].v) > vmax) vmax = fabsf(stream->base[i].v);
	}
	stream->compact_uv_scale[0] = umax * (32768.0f / 32767.0f);
	stream->compact_uv_scale[1] = vmax * (32768.0f / 32767.0f);
	/* build 111: in the copies' arena (16-byte aligned, as memalign gave) */
	stream->compact_base = copy_alloc(stream->count * sizeof(struct compact_vertex), (void **)&stream->compact_base);
	stream->compact_lightmap = stream->lightmap ?
		copy_alloc(stream->count * sizeof(struct compact_vertex), (void **)&stream->compact_lightmap) : NULL;
	if (!stream->compact_base || (stream->lightmap && !stream->compact_lightmap))
	{
		copy_free(stream->compact_base);
		copy_free(stream->compact_lightmap);
		stream->compact_base = stream->compact_lightmap = NULL;
		compact_refused++;
		return 0;
	}
	for (i = 0; i < stream->count; i++)
	{
		struct ge_vertex *b = &stream->base[i];
		struct compact_vertex *c = &stream->compact_base[i];
		float *p = &b->x;
		short q[3];

		for (axis = 0; axis < 3; axis++)
		{
			q[axis] = (short)floorf((p[axis] + stream->origin[axis] - stream->compact_origin[axis]) / COMPACT_STEP + 0.5f);
			/* the float copy on the lattice point itself (exact in floats) */
			p[axis] = stream->compact_origin[axis] + (float)q[axis] * COMPACT_STEP - stream->origin[axis];
		}
		c->x = q[0]; c->y = q[1]; c->z = q[2];
		c->u = (short)floorf(b->u / stream->compact_uv_scale[0] * 32768.0f + 0.5f);
		c->v = (short)floorf(b->v / stream->compact_uv_scale[1] * 32768.0f + 0.5f);
		if (stream->lightmap)
		{
			struct ge_vertex *l = &stream->lightmap[i];
			struct compact_vertex *cl = &stream->compact_lightmap[i];
			float lu = l->u < 0.0f ? 0.0f : l->u > 1.0f ? 1.0f : l->u, lv = l->v < 0.0f ? 0.0f : l->v > 1.0f ? 1.0f : l->v;

			l->x = b->x; l->y = b->y; l->z = b->z;
			cl->x = q[0]; cl->y = q[1]; cl->z = q[2];
			cl->u = (short)floorf(lu * 32767.0f + 0.5f);
			cl->v = (short)floorf(lv * 32767.0f + 0.5f);
		}
	}
	ge_writeback(stream->compact_base, stream->count * sizeof(struct compact_vertex));
	if (stream->compact_lightmap)
		ge_writeback(stream->compact_lightmap, stream->count * sizeof(struct compact_vertex));
	compact_streams++;
	return 1;
}
static struct level_stream level_streams[LEVEL_STREAMS];
static unsigned long stream_bytes = 0;
static unsigned long streams_let_go = 0;
/* build 68: the copies' budget while a geometry pack is open (its blocks come
and go; the copies are what stays) */
#define LEVEL_STREAM_BUDGET (1024 * 1024)

/* ---------- build 111: the copies' arena

b108 run 1 (hardware_b108.md): the copies are let go and rebuilt as the view
moves (level_streams_trim), and each rebuild asked the general heap for large
blocks again (two of 106 KB for a material of 5,439 vertices) while the
game's small, long-lived allocations settled into the holes the last ones
left. After the hunter fight no hole was large enough: 1.4 MB free in 293
pieces, and 5,290 materials went undrawn to the level's end.

Every piece of a copy (the float copies, the normals, the radiosity, the
16-bit copies) now comes from one block, carved out of the heap at the first
copy and kept, so the copies' churn never reaches the general heap. When a
piece doesn't fit, the least recently drawn copies are let go until it does,
never one drawn this frame (the trim's rule: the GE may still be reading
those). If this frame's copies fill the arena, the piece comes from the
general heap, as before b111. What is drawn doesn't change.

Its size is the copies' budget plus 32 KB for the blocks' headers and some
slack against the arena's own fragmentation: what the copies already took
from the heap. Everything else shares what's left. In b108 run 1 that was
tight: at the peak (game time 25,072) the heap held 5,856 KB, the copies
about 980 of it, so the rest needed about 4,876 KB; with a 6,016 KB heap,
32 KB of slack leaves it about 4,960 (128 KB would leave about 4,864).

Blocks lie end to end, each behind a header holding its size and the size of
the block before it, so a freed block merges with free neighbours on both
sides. A request takes the smallest free block that holds it (best fit),
split when what's left is worth keeping. "noarena" (keep word): the general
heap only, as before. */
#define ARENA_BYTES (LEVEL_STREAM_BUDGET + 32 * 1024)
/* "heaproom256": on a map that carves, 256 KB of the arena's room moves to
the level carve, taken whole at the map's open as the carve's floor (the level
carve, below), so the general heap keeps those bytes. The geometry cache pays
for it, 256 KB less when it is over 1 MB (psp_geo.c); the RAM budget census's
replay found those bytes free. Frames in flight (geinflight's buffer) and the
Media Engine's VRAM copies take about 200 KB more of the heap, enough to run
a30 out of it, so with geinflight and a geometry cache over 1 MB it is on by
default; elsewhere the heap has room, and the move would cost the geometry
cache's misses for nothing. "heaproomoff" turns it off, "heaproom256" on with
any geometry cache (with a 1 MB one its bytes come from the textures). A
384 KB size (128 KB more from the textures) cost b30 texture loads and frames
drawn with a texture missing, so it is gone (docs/review_sv.md).
"heaproomN" (N KB, in 128s, 128 to 768): the same move at another size, the
geometry cache paying all of it down to 1 MB, so the textures keep what they
had. In the carve's own steps: the carve is taken 128 KB at a time, so a
size between them takes the rest from the textures, and b30 then draws many
more frames with a texture missing. Smaller vertex formats (PRECONVERT) let the cache hold the same play in
less, and this is how those bytes reach the heap; the replay of the cache at
smaller sizes prices each map (memory_survey_b144.md, section 7, on research/memtrace until it merges) */
#define HEAPROOM_MOST_KB 768
unsigned int psp_heaproom_bytes(void)
{
#ifndef PSP_MULTIPLAYER
	int psp_keep(const char *name);
	int psp_keep_number(const char *prefix, int fallback);
	int kb;

	if (psp_keep("heaproomoff"))
		return 0;
	kb = psp_keep_number("heaproom", 0);
	if (kb > 0)
	{
		kb = (kb + 127) & ~127;
		return (unsigned int)(kb < HEAPROOM_MOST_KB ? kb : HEAPROOM_MOST_KB) * 1024u;
	}
	if (inflight_buffer_bytes() && (psp_keep("geo2") || psp_keep("geo15")))
		return 256 * 1024;
#endif
	return 0; /* the multiplayer build's maps keep their geometry cache in the heap and carve nothing */
}
/* the arena's size, settled once (0: not yet): ARENA_BYTES less the heaproom
share. The arena is taken once and kept for every map, so it is smaller on
every map; the maps that carve take the difference back at their open (one
that doesn't, the main menu among them, has the smaller arena and the heap) */
static unsigned int arena_room_bytes = 0;

static unsigned int arena_size(void)
{
	if (!arena_room_bytes)
		arena_room_bytes = ARENA_BYTES - psp_heaproom_bytes();
	return arena_room_bytes;
}
#define ARENA_ALIGN 16
#define ARENA_SPLIT_LEAST 64
struct arena_block
{
	unsigned int size;     /* bytes, this header included; a multiple of ARENA_ALIGN */
	unsigned int previous; /* the size of the block before it (0: the first) */
	unsigned int used;
	void **owner;          /* the field holding the block's address (arena_compact moves it) */
};
/* where the copies' blocks lie: the arena, and on a map that carves one, the
level carve (below). Both keep their blocks alike, and a copy's pieces may lie
in either */
struct copy_room
{
	char *start;
	unsigned int bytes;
	unsigned long used;
};
static struct copy_room rooms[2];
#define ARENA_ROOM (&rooms[0])
#define CARVE_ROOM (&rooms[1])
static int arena_state = -1; /* -1: not started; 0: off ("noarena", or no room); 1: on */
static unsigned long arena_let_go = 0, arena_from_heap = 0, copy_reserve_refused = 0;

/* The heap's last KB are kept for the engine's own requests: a level copy or
one of this file's speed-up tables (triangle lists, skin plans, spheres)
that would take them is refused instead, and its draw takes the slower path
it has without one. a10 and a30 filled the heap with these, and the engine's
mallocs then failed. "copyreserveN": N KB (256 by default; copyreserve0, as
before). The room is read once a frame (it walks the heap's free lists) and
what this file takes meanwhile is counted off it */
static int heap_reserve_kb = -1;
static unsigned long heap_room_frame = ~0ul, heap_room_kb_now = 0, heap_reserve_refused = 0;

static int heap_reserve_allows(size_t bytes)
{
	extern unsigned long psp_heap_room_kb(void);
	unsigned long kb = (unsigned long)(bytes / 1024) + 1;

	if (heap_reserve_kb < 0)
		heap_reserve_kb = psp_keep_number("copyreserve", 256);
	if (heap_reserve_kb <= 0)
		return 1;
	if (psp_on_me())
		return 0; /* the heap's lock is a system call */
	if (heap_room_frame != show_frame_number)
	{
		heap_room_frame = show_frame_number;
		heap_room_kb_now = psp_heap_room_kb();
	}
	if (heap_room_kb_now < (unsigned long)heap_reserve_kb + kb)
		return 0;
	heap_room_kb_now -= kb;
	return 1;
}

/* a speed-up table's block, or none while the heap is near its reserve.
Only for tables whose absence costs time (bounding spheres, cluster runs):
never for a skinned part's plan, without which its vertices are drawn on one
node (doors sliding whole, hands off the gun) until the buffer is converted
again */
static void *cache_malloc(size_t bytes)
{
	if (!heap_reserve_allows(bytes))
	{
		heap_reserve_refused++;
		return NULL;
	}
	return malloc(bytes);
}
/* pieces the general heap holds now (from_heap counts all ever sent there) */
static unsigned long arena_heap_live = 0;
/* the largest free block at the last frame's end, for the log */
static unsigned int arena_largest = 0, carve_largest = 0;
static int level_stream_let_go_oldest(void);
static void level_stream_let_go(struct level_stream *stream);

/* ---------- the level carve

a10's cryo chamber, entered from the main menu: its view draws about 1.67 MB
of level copies a frame [D, the a10 diagnosis] against the arena's 1,056 KB,
and the general heap, at its end there (23 KB free of 5,604 KB), had no piece
for the rest. The materials left over waited for room the whole time, their
draws skipped: the floor grating and some walls missing [E, hop-menu-a10;
M, a PSP-1000].

On a map whose geometry cache is carved from the texture RAM cache (its
textures stream as drawn anyway), the copies may take a second room from
there too, below the geometry and animation caches' shares, as those carve
theirs. Every texture byte it takes is one the texture cache can't hold
(b30 drew untextured for thousands of frames with 512 KB taken at its open
[E]), so it is taken only when the copies need it: at a
frame's end (the GE done with the textures), when a copy found no room in the
arena nor a piece of the general heap large enough, or materials wait for
room that the rooms can't give, the carve grows downward by whole steps
(LEVEL_CARVE_STEP) to cover it, the textures resident there forgotten as for
any carve. Then copies go to the arena first, the carve next, the general
heap last. A carve left empty for CARVE_IDLE_FRAMES goes back to the texture
cache, and all of it at the map's end. None on the showcase map (its textures
are the point), nor with "noarena" (the general heap alone, as asked) or
"streamsqueezeN" (a test of the arena's own pressure). "levelcarveN": the
most it may take, in KB; "levelcarve0": never, the copies as before.

The block goes back to the system while the PSP sleeps, and the block
headers in the carve with it. The power callback marks the carve lost: it
takes nothing more, nothing writes to it, and at the next frame's end its
copies are let go without reading those headers (their next draws build them
again from the geometry pack). Once the block is lent again (a new
generation from the texture cache) at the same place, the carve starts again
empty. Until then draws may read what the sleep left there, as the texture
and geometry caches' do */
#define LEVEL_CARVE_DEFAULT_KB 768
#define LEVEL_CARVE_STEP (128 * 1024)
#define CARVE_IDLE_FRAMES 300
void *psp_tex_carve(unsigned int bytes);       /* psp_tex.c */
unsigned int psp_tex_carved(void);
void *psp_tex_carve_now(unsigned int *generation);
/* 1: lost to a sleep (the power callback's thread sets it); 2: lost, its
copies already let go */
static volatile int carve_lost = 0;
static int carve_dropping = 0;
static unsigned int carve_generation = 0, carve_given = 0;
static unsigned long carve_pieces = 0, carve_pieces_peak = 0, carve_peak = 0, carve_sleeps = 0;
/* this map may carve (carve_most bytes at most, below carve_below bytes the
other caches carved); what the copies found no room for since the last
frame's end; a frame-end build that failed though the rooms had the bytes */
static int carve_allowed = 0, carve_pack_short = 0;
static unsigned int carve_most = 0, carve_below = 0, carve_want = 0, carve_idle = 0;
static unsigned long carve_steps = 0, carve_returns = 0;
/* "heaproom256": the bytes the carve keeps from the map's open to its end (0:
none), the arena's share it stands in for */
static unsigned int carve_floor = 0;
unsigned int psp_tex_ram_textures(void);
static void carve_release(void);
static int carve_grow(unsigned int deficit);

/* a room that takes blocks now */
static int room_open(const struct copy_room *room)
{
	return room->start && (room != CARVE_ROOM || !carve_lost);
}

/* the room whose range holds pointer, open or not; NULL: the heap or VRAM */
static struct copy_room *room_of(const void *pointer)
{
	const char *p = pointer;
	int k;

	for (k = 0; k < 2; k++)
		if (rooms[k].start && p >= rooms[k].start && p < rooms[k].start + rooms[k].bytes)
			return &rooms[k];
	return NULL;
}

/* one free block, the whole room */
static void room_reset(struct copy_room *room)
{
	struct arena_block *first = (struct arena_block *)room->start;

	first->size = room->bytes;
	first->previous = 0;
	first->used = 0;
	first->owner = NULL;
	room->used = 0;
	if (room == CARVE_ROOM)
		carve_pieces = 0;
}

/* A level material whose copy finds no room, in the arena or on the general
heap, once every copy not drawn so far in the frame has been let go: what the
arena still holds is on screen, and the heap has no piece large enough. Tried
again on its next draw, it would let go the copies not yet drawn in that frame
(which the frame then builds again, reading the geometry pack) and fail again,
every frame. Such a material waits instead: its draws skip it at no cost, and
at a frame's end, when the GE is done and the arena's blocks may move, it is
built if the room free and the copies not drawn that frame hold it (the arena
compacted first if that room is split), or, once a second, if the general
heap has the room. What is drawn otherwise doesn't change. "streamroomoff":
tried again on every draw. "streamsqueezeN" (test): the copies held to N KB
of the arena and given no heap room, the pressure made where the emulator
can show it */
#define STREAM_WAITING 64
#define STREAM_HEAP_RETRY 30
static int stream_room = -1;         /* -1: not read; 1: on; 0: "streamroomoff" */
static int stream_room_building = 0; /* the frame end's build, past the wait */
static int arena_squeeze = 0;        /* "streamsqueezeN": KB */
static struct
{
	const void *vertices, *lightmap_vertices;
	int count;
	/* frames: last drawn; the next try on the heap, and on the arena's room
	after a try there failed */
	unsigned int wanted, heap_after, arena_after;
	unsigned int texture;
} stream_waiting[STREAM_WAITING];
static int stream_waiting_count = 0;
/* the material level_stream_get was asked for (vertices is replaced by the
pack's bytes on the way); texture: its base texture's key as the asking draw
gave it (0: not known), which names the material in the log */
static const void *stream_asked, *stream_asked_lightmap;
static int stream_asked_count;
static unsigned int stream_asked_texture = 0;
static unsigned long stream_room_waits = 0, stream_room_skips = 0, stream_room_built = 0, stream_room_tries = 0,
	stream_room_heap_tries = 0, stream_room_compactions = 0, stream_room_seen = 0;
static unsigned int stream_room_log_frame = 0;
static void *arena_take(struct copy_room *room, unsigned int size, void **owner);
static unsigned int stream_size(const struct level_stream *stream);
static unsigned int stream_room_need(int count, int lightmap, int compact);

static struct arena_block *arena_next(const struct copy_room *room, struct arena_block *block)
{
	char *next = (char *)block + block->size;

	return next < room->start + room->bytes ? (struct arena_block *)next : NULL;
}

static void __attribute__((cold)) arena_start(void)
{
	arena_state = 0;
	stream_room = !psp_keep("streamroomoff");
	if (psp_keep("noarena"))
	{
		platform_log("GE: the copies' arena: off (noarena); copies from the general heap");
		return;
	}
	ARENA_ROOM->start = memalign(ARENA_ALIGN, arena_size());
	if (!ARENA_ROOM->start)
	{
		platform_log("GE: the copies' arena: no room for %u KB; copies from the general heap", arena_room_bytes / 1024);
		return;
	}
	ARENA_ROOM->bytes = arena_room_bytes;
	room_reset(ARENA_ROOM);
	arena_state = 1;
	platform_log("GE: the copies' arena: %u KB", arena_room_bytes / 1024);
	{
		extern int psp_keep_number(const char *prefix, int fallback);

		arena_squeeze = psp_keep_number("streamsqueeze", 0);
	}
	/* held by no copy for good: compaction keeps it at the start */
	if (arena_squeeze > 0 && (unsigned int)arena_squeeze * 1024 < arena_room_bytes)
	{
		arena_take(ARENA_ROOM, arena_room_bytes - (unsigned int)arena_squeeze * 1024, NULL);
		platform_log("GE: streamsqueeze (test): the copies held to %d KB of the arena, none from the general heap",
			arena_squeeze);
	}
}

/* the smallest free block holding size bytes (header included), split if
the rest is worth keeping; NULL: none */
static void *arena_take(struct copy_room *room, unsigned int size, void **owner)
{
	struct arena_block *block, *best = NULL;

	for (block = (struct arena_block *)room->start; block; block = arena_next(room, block))
		if (!block->used && block->size >= size && (!best || block->size < best->size))
			best = block;
	if (!best)
		return NULL;
	if (best->size - size >= ARENA_SPLIT_LEAST)
	{
		struct arena_block *rest = (struct arena_block *)((char *)best + size), *after;

		rest->size = best->size - size;
		rest->previous = size;
		rest->used = 0;
		rest->owner = NULL;
		best->size = size;
		after = arena_next(room, rest);
		if (after)
			after->previous = rest->size;
	}
	best->used = 1;
	best->owner = owner;
	room->used += best->size;
	if (room == CARVE_ROOM)
	{
		if (room->used > carve_peak)
			carve_peak = room->used;
		if (++carve_pieces > carve_pieces_peak)
			carve_pieces_peak = carve_pieces;
	}
	return best + 1;
}

/* the arena first; the carve only for what the arena can't hold */
static void *rooms_take(unsigned int size, void **owner)
{
	void *pointer = size <= ARENA_ROOM->bytes ? arena_take(ARENA_ROOM, size, owner) : NULL;

	if (!pointer && room_open(CARVE_ROOM) && size <= CARVE_ROOM->bytes)
		pointer = arena_take(CARVE_ROOM, size, owner);
	return pointer;
}

static void *copy_alloc(unsigned int bytes, void **owner)
{
	unsigned int size = (bytes + sizeof(struct arena_block) + ARENA_ALIGN - 1) & ~(ARENA_ALIGN - 1);
	void *pointer = NULL;
	int overflow = 0;

	if (arena_state < 0)
		arena_start();
	if (arena_state > 0 && (size <= ARENA_ROOM->bytes || (room_open(CARVE_ROOM) && size <= CARVE_ROOM->bytes)))
	{
		while (!(pointer = rooms_take(size, owner)) && level_stream_let_go_oldest())
			arena_let_go++;
		if (pointer)
			return pointer;
		if (arena_squeeze > 0)
			return NULL;
		/* counted once the heap gives the piece: a refusal counted as a piece
		there kept the migration walking the copies every frame for nothing */
		overflow = 1;
		if (stream_room <= 0)
		{
			arena_from_heap++;
			arena_heap_live++;
		}
	}
	/* a copy the rooms refused doesn't take the heap's reserve
	(heap_reserve_allows): refused, the draw reads the level's own data */
	if (overflow && !heap_reserve_allows(bytes))
	{
		copy_reserve_refused++;
		if (arena_state > 0)
			carve_want += size;
		return NULL;
	}
	/* 16-byte aligned like the arena's blocks (the 16-bit copies had memalign).
	A refusal isn't a failure: the carve may grow for it (below) */
	{
		extern void *psp_heap_try_memalign(size_t alignment, size_t size);

		pointer = psp_heap_try_memalign(ARENA_ALIGN, bytes);
	}
	if (pointer && overflow && stream_room > 0)
	{
		arena_from_heap++;
		arena_heap_live++;
	}
	/* the rooms and the heap all refused it: the carve may grow for it at
	the frame's end (carve_grow) */
	if (!pointer && arena_state > 0)
		carve_want += size;
	return pointer;
}

/* a used block given back to its room, merged with free neighbours */
static void room_give(struct copy_room *room, struct arena_block *block)
{
	struct arena_block *next;

	block->used = 0;
	block->owner = NULL;
	room->used -= block->size;
	next = arena_next(room, block);
	if (next && !next->used)
		block->size += next->size;
	if (block->previous)
	{
		struct arena_block *before = (struct arena_block *)((char *)block - block->previous);

		if (!before->used)
		{
			before->size += block->size;
			block = before;
		}
	}
	next = arena_next(room, block);
	if (next)
		next->previous = block->size;
}

static void copy_free(void *pointer)
{
	struct copy_room *room;

	if (!pointer)
		return;
	room = room_of(pointer);
	if (!room)
	{
		if (arena_state > 0 && arena_heap_live)
			arena_heap_live--;
		free(pointer);
		return;
	}
	/* a lost carve's headers may be the sleep's: it starts again empty */
	if (room == CARVE_ROOM && (carve_lost || carve_dropping))
		return;
	if (room == CARVE_ROOM && carve_pieces)
		carve_pieces--;
	room_give(room, (struct arena_block *)pointer - 1);
}

/* build 111: keeping the arena whole between frames.

The copies drawn in a frame can't move or go while it's drawn, so when the
view brings many new copies at once (a BSP switch, a turn in a big room) they
land in whatever holes the earlier copies left, and the arena can fill up in
pieces too small for the next big copy while plenty is free. b30 replayed on
run 1's route: after the BSP switch into the facility, 89 pieces went to the
general heap with about 960 KB of copies against the arena's 1,056.

At the frame's end the GE is done and no copy is being built, so the arena's
used blocks can slide down to its start (arena_compact): each block's owner
field (the level copy's pointer to it) is repointed, and the GE's arrays are
written back from the CPU's cache. Nothing outside level_streams keeps a
copy's address past the frame: see-through draws are flushed within it, cut
corners live in the display list, the decal cache and the overlays' answers
keep the stream or the indices' identity [D]. Then copies the general heap
holds come back into the arena (arena_migrate), a bounded amount a frame.

Compacting moves up to the arena's 1 MB, so it runs only when the free space
is badly split (its largest block under half of it, at least 64 KB free), at
most every 30 frames, or at the end of a frame that sent pieces to the heap.
"nocompact": neither (the arena as first built) */
#define ARENA_COMPACT_EVERY 30
#define ARENA_MIGRATE_BYTES (128 * 1024)
static int arena_compact_on = -1;
static unsigned int arena_last_compact = 0;
static unsigned long arena_seen_from_heap = 0;
static unsigned long arena_compactions = 0, arena_moved_kb = 0, arena_migrated = 0;

static void __attribute__((cold)) arena_compact(struct copy_room *room)
{
	struct arena_block *block, *next;
	char *to = room->start, *first_moved = NULL, *end = room->start + room->bytes;
	unsigned int previous = 0;
	unsigned long moved = 0;

	for (block = (struct arena_block *)room->start; block; block = next)
	{
		unsigned int size = block->size;

		/* the next block's header lies past this one: moving this one down
		never overwrites it */
		next = arena_next(room, block);
		if (!block->used)
			continue;
		if ((char *)block != to)
		{
			void *was = block + 1;

			memmove(to, block, size);
			if (!first_moved)
				first_moved = to;
			moved += size;
			vm_level_move(was, (struct arena_block *)to + 1);
		}
		block = (struct arena_block *)to;
		block->previous = previous;
		if (block->owner)
			*block->owner = block + 1;
		previous = size;
		to += size;
	}
	if (to < end)
	{
		block = (struct arena_block *)to;
		block->size = (unsigned int)(end - to);
		block->previous = previous;
		block->used = 0;
		block->owner = NULL;
	}
	if (first_moved)
		ge_writeback(first_moved, (unsigned int)(to - first_moved));
	arena_compactions++;
	arena_moved_kb += moved / 1024;
}

/* a piece the general heap holds, moved into the arena (or the carve) if a
free block takes it as it is (no letting go); the GE's arrays written back.
0: no room, 1: nothing to move (none, VRAM, or already in a room), 2: moved */
static int __attribute__((cold)) arena_migrate_piece(void **field, unsigned int bytes, int ge_reads)
{
	char *piece = (char *)*field;
	void *to;

	if (!piece || (((unsigned int)piece & 0x0F000000u) == 0x04000000u) || room_of(piece))
		return 1;
	to = rooms_take((bytes + sizeof(struct arena_block) + ARENA_ALIGN - 1) & ~(ARENA_ALIGN - 1), field);
	if (!to)
		return 0;
	memcpy(to, piece, bytes);
	if (ge_reads)
		ge_writeback(to, bytes);
	*field = to;
	vm_level_move(piece, to);
	free(piece);
	if (arena_heap_live)
		arena_heap_live--;
	return 2;
}

/* the general heap's copies brought into the arena, up to ARENA_MIGRATE_BYTES
a frame; a copy whose pieces don't all fit keeps the rest where they are */
static void __attribute__((cold)) arena_migrate(void)
{
	static const struct { unsigned short offset; unsigned char size, ge_reads; } pieces[6] = {
		{ offsetof(struct level_stream, base), sizeof(struct ge_vertex), 1 },
		{ offsetof(struct level_stream, lightmap), sizeof(struct ge_vertex), 1 },
		{ offsetof(struct level_stream, normals), sizeof(unsigned int), 0 },
		{ offsetof(struct level_stream, radiosity), sizeof(unsigned int), 0 },
		{ offsetof(struct level_stream, compact_base), sizeof(struct compact_vertex), 1 },
		{ offsetof(struct level_stream, compact_lightmap), sizeof(struct compact_vertex), 1 },
	};
	long budget = ARENA_MIGRATE_BYTES;
	int index, k, moved, result;

	for (index = 0; index < LEVEL_STREAMS && budget > 0 && arena_heap_live; index++)
	{
		struct level_stream *stream = &level_streams[index];

		for (moved = k = 0; k < 6; k++)
		{
			result = arena_migrate_piece((void **)((char *)stream + pieces[k].offset),
				(unsigned int)stream->count * pieces[k].size, pieces[k].ge_reads);
			if (!result)
				return;
			moved |= result == 2;
		}
		if (moved)
		{
			arena_migrated++;
			budget -= (long)stream_size(stream);
		}
	}
}

/* at the frame's end, after the trim (the GE has finished the frame). The
arena's housekeeping is marked cold: rare, compiled for size, kept out of the
hot code (build 111's first compaction build crossed a 64 KB heap step) */
/* ---------- build 115: "gereads" (big_levers.md section 0, "Measure first")

What each draw makes the GE read from memory, and where those bytes live:
the vertices (count times the format's size: for an indexed draw, a vertex
fetched for every index; any reuse inside the GE would make it less [U]) and
the indices. By where they live: VRAM, the display list (data written into
it each frame: decals' and cut pieces' corners, kept indices), the level's
copies (the arena above), the geometry cache (the models), the rest of the
volatile partition, and the heap or the program. Each vertex buffer (by its
address) keeps the bytes read from it and its extent; every 300 frames the
report gives the totals a frame by place and by kind of draw, the buffers
that would give most back per KB of VRAM, and the fit curve: the share of the
GE's reads from main RAM that VRAM would take if it held the best buffers,
200 to 1,000 KB of them. The per-frame data in the list can't be held, so it
stays out of the curve */
enum { GR_VRAM, GR_LIST, GR_LEVEL, GR_GEOMETRY, GR_VOLATILE, GR_OTHER, GR_PLACES };
static const char *const gr_place_names[GR_PLACES] = { "VRAM", "display list (this frame's data)", "level copies",
	"geometry cache (models)", "volatile, other", "heap or program" };
struct gr_entry { unsigned int key, read, size; unsigned char place, kind; };
#define GR_SLOTS 2048
static struct gr_entry *gr_table = NULL;
static unsigned long long gr_place_bytes[GR_PLACES], gr_kind_bytes[KINDS], gr_index_bytes = 0;
static unsigned long gr_draws = 0, gr_dropped = 0, gr_frames_mark = 0;
void psp_geo_range(unsigned int *start, unsigned int *end); /* psp_geo.c */
int psp_geo_holds(unsigned int address);

/* a GE vertex's size from its format word (the GE's own layout: weights,
texture coordinates, colour, normal, position, each aligned to its size, the
whole to its largest) */
static int gr_vertex_size(int vtype)
{
	static const unsigned char sizes[4] = { 0, 1, 2, 4 };
	int size = 0, align = 1, t;

	if ((t = (vtype >> 9) & 3) != 0)
	{
		size = (size + sizes[t] - 1) & ~(sizes[t] - 1);
		size += sizes[t] * (((vtype >> 14) & 7) + 1);
		if (sizes[t] > align) align = sizes[t];
	}
	if ((t = vtype & 3) != 0)
	{
		size = (size + sizes[t] - 1) & ~(sizes[t] - 1);
		size += sizes[t] * 2;
		if (sizes[t] > align) align = sizes[t];
	}
	if ((t = (vtype >> 2) & 7) != 0)
	{
		int c = t == 7 ? 4 : 2;

		size = (size + c - 1) & ~(c - 1);
		size += c;
		if (c > align) align = c;
	}
	if ((t = (vtype >> 5) & 3) != 0)
	{
		size = (size + sizes[t] - 1) & ~(sizes[t] - 1);
		size += sizes[t] * 3;
		if (sizes[t] > align) align = sizes[t];
	}
	if ((t = (vtype >> 7) & 3) != 0)
	{
		size = (size + sizes[t] - 1) & ~(sizes[t] - 1);
		size += sizes[t] * 3;
		if (sizes[t] > align) align = sizes[t];
	}
	return (size + align - 1) & ~(align - 1);
}

static int gr_place(unsigned int address)
{
	unsigned int a = address & ~0x40000000u, geo_start = 0, geo_end = 0;

	if ((a & 0x0F000000u) == 0x04000000u)
		return GR_VRAM;
	if (display_list && a >= ((unsigned int)display_list & ~0x40000000u) &&
		a < ((unsigned int)display_list & ~0x40000000u) + (unsigned int)display_list_size)
		return GR_LIST;
	if (room_of((const void *)a))
		return GR_LEVEL;
	psp_geo_range(&geo_start, &geo_end);
	if ((geo_end > geo_start && a >= (geo_start & ~0x40000000u) && a < (geo_end & ~0x40000000u)) || psp_geo_holds(a))
		return GR_GEOMETRY;
	if (a >= 0x08400000u && a < 0x08800000u)
		return GR_VOLATILE;
	return GR_OTHER;
}

static void ge_reads_note(int vtype, int count, const void *indices, const void *vertices)
{
	int vsize = gr_vertex_size(vtype), isize = ((vtype >> 11) & 3) == 1 ? 1 : ((vtype >> 11) & 3) == 2 ? 2 : 0;
	unsigned int bytes, extent, key, h;
	int place;

	if (count <= 0 || !vertices || vsize <= 0)
		return;
	if (!gr_table)
	{
		gr_table = calloc(GR_SLOTS, sizeof(struct gr_entry));
		if (!gr_table)
		{
			ge_reads_on = 0;
			platform_log("GE: gereads: no room for its table; off");
			return;
		}
	}
	gr_draws++;
	bytes = (unsigned int)count * (unsigned int)vsize;
	place = gr_place((unsigned int)vertices);
	gr_place_bytes[place] += bytes;
	gr_kind_bytes[ge_kind_now] += bytes;
	if (indices && isize)
	{
		gr_index_bytes += (unsigned int)count * (unsigned int)isize;
		gr_place_bytes[gr_place((unsigned int)indices)] += (unsigned int)count * (unsigned int)isize;
	}
	/* only buffers that could be held go in the table: this frame's data in
	the list is new every frame (its addresses would fill the table), and
	what's in VRAM is there already */
	if (place == GR_LIST || place == GR_VRAM)
		return;
	/* the buffer's extent: the vertices an index reaches, or the draw's own */
	extent = bytes;
	if (indices && isize)
	{
		unsigned int most = 0;
		int k;

		if (isize == 2)
			for (k = 0; k < count; k++) { if (((const unsigned short *)indices)[k] > most) most = ((const unsigned short *)indices)[k]; }
		else
			for (k = 0; k < count; k++) { if (((const unsigned char *)indices)[k] > most) most = ((const unsigned char *)indices)[k]; }
		extent = (most + 1) * (unsigned int)vsize;
	}
	key = (unsigned int)vertices & ~0x40000000u;
	h = (key >> 4) * 2654435761u;
	for (unsigned int probe = 0; probe < 16; probe++)
	{
		struct gr_entry *e = &gr_table[(h + probe) & (GR_SLOTS - 1)];

		if (e->key == key || !e->key)
		{
			if (!e->key)
			{
				e->key = key;
				e->place = (unsigned char)place;
				e->kind = (unsigned char)ge_kind_now;
			}
			e->read += bytes;
			if (extent > e->size)
				e->size = extent;
			return;
		}
	}
	gr_dropped++;
}

static int gr_by_value(const void *a, const void *b)
{
	const struct gr_entry *x = a, *y = b;
	/* reads per byte, best first (cross-multiplied, no division) */
	unsigned long long l = (unsigned long long)y->read * (x->size ? x->size : 1);
	unsigned long long r = (unsigned long long)x->read * (y->size ? y->size : 1);

	return l > r ? 1 : l < r ? -1 : 0;
}

static void __attribute__((noinline, cold)) ge_reads_report(unsigned long frames_now)
{
	extern long psp_game_time_peek(void);
	unsigned long span = frames_now - gr_frames_mark;
	unsigned long long ram = 0, taken;
	char line[512];
	int k, n = 0, at, step;
	static const unsigned int steps_kb[5] = { 200, 400, 600, 800, 1000 };

	if (ge_reads_on <= 0 || !gr_table || !span)
		return;
	at = snprintf(line, sizeof(line), "gereads: game time %ld, %lu frames, %lu draws; KB a frame by place:", psp_game_time_peek(), span, gr_draws);
	for (k = 0; k < GR_PLACES && at < (int)sizeof(line) - 1; k++)
		at += snprintf(line + at, sizeof(line) - at, " %s %.1f;", gr_place_names[k], gr_place_bytes[k] / 1024.0 / span);
	if (at < (int)sizeof(line) - 1)
		snprintf(line + at, sizeof(line) - at, " (indices %.1f of it)", gr_index_bytes / 1024.0 / span);
	platform_log("%s", line);
	at = snprintf(line, sizeof(line), "gereads: vertex KB a frame by kind:");
	for (k = 0; k < KINDS && at < (int)sizeof(line) - 1; k++)
		if (gr_kind_bytes[k])
			at += snprintf(line + at, sizeof(line) - at, " %s %.1f;", kind_names[k], gr_kind_bytes[k] / 1024.0 / span);
	platform_log("%s", line);
	/* the buffers that could be held: in main RAM, not this frame's data */
	for (k = 0; k < GR_SLOTS; k++)
		if (gr_table[k].key && gr_table[k].place != GR_VRAM && gr_table[k].place != GR_LIST)
		{
			gr_table[n] = gr_table[k];
			ram += gr_table[n].read;
			n++;
		}
	qsort(gr_table, n, sizeof(struct gr_entry), gr_by_value);
	at = snprintf(line, sizeof(line), "gereads: fit curve (%d buffers in main RAM, %.1f KB a frame read from them): VRAM holding the best",
		n, ram / 1024.0 / span);
	for (step = 0; step < 5 && at < (int)sizeof(line) - 1; step++)
	{
		unsigned long long held = 0;

		taken = 0;
		for (k = 0; k < n; k++)
			if (held + gr_table[k].size <= steps_kb[step] * 1024ull)
			{
				held += gr_table[k].size;
				taken += gr_table[k].read;
			}
		at += snprintf(line + at, sizeof(line) - at, " %u KB: %.0f%%;", steps_kb[step], ram ? 100.0 * taken / ram : 0.0);
	}
	platform_log("%s", line);
	/* the same curve for the level's copies alone and the models alone: the
	mixed curve fills every step with model buffers first, so it can't say
	how much VRAM the level's streams would want beside vrammodels (the
	split a "levelvramN" room would need). Each place's share is of its own
	reads */
	{
		static const unsigned char places[2] = { GR_LEVEL, GR_GEOMETRY };
		static const unsigned short place_kb[6] = { 64, 128, 192, 256, 384, 512 };
		int p;

		for (p = 0; p < 2; p++)
		{
			unsigned long long place_read = 0, place_size = 0;
			int place_count = 0;

			for (k = 0; k < n; k++)
				if (gr_table[k].place == places[p])
				{
					place_read += gr_table[k].read;
					place_size += gr_table[k].size;
					place_count++;
				}
			at = snprintf(line, sizeof(line), "gereads: %s alone (%d buffers, %.0f KB, %.1f KB a frame read): VRAM holding the best",
				gr_place_names[places[p]], place_count, place_size / 1024.0, place_read / 1024.0 / span);
			for (step = 0; step < 6 && at < (int)sizeof(line) - 1; step++)
			{
				unsigned long long held = 0;

				taken = 0;
				for (k = 0; k < n; k++)
					if (gr_table[k].place == places[p] && held + gr_table[k].size <= place_kb[step] * 1024ull)
					{
						held += gr_table[k].size;
						taken += gr_table[k].read;
					}
				at += snprintf(line + at, sizeof(line) - at, " %u KB: %.0f%% (%.1f KB a frame);", place_kb[step],
					place_read ? 100.0 * taken / place_read : 0.0, taken / 1024.0 / span);
			}
			platform_log("%s", line);
		}
	}
	at = snprintf(line, sizeof(line), "gereads: best buffers (place, kind, KB, KB read a frame):");
	for (k = 0; k < n && k < 8 && at < (int)sizeof(line) - 1; k++)
		at += snprintf(line + at, sizeof(line) - at, " %s/%s %.1f %.1f;", gr_place_names[gr_table[k].place], kind_names[gr_table[k].kind],
			gr_table[k].size / 1024.0, gr_table[k].read / 1024.0 / span);
	platform_log("%s%s", line, gr_dropped ? " (some buffers past the table's room weren't kept)" : "");
	memset(gr_table, 0, GR_SLOTS * sizeof(struct gr_entry));
	memset(gr_place_bytes, 0, sizeof(gr_place_bytes));
	memset(gr_kind_bytes, 0, sizeof(gr_kind_bytes));
	gr_index_bytes = 0;
	gr_draws = gr_dropped = 0;
	gr_frames_mark = frames_now;
}

/* a room's largest free block (0 for a room not open), and its free bytes */
static unsigned int __attribute__((cold)) room_largest(struct copy_room *room, unsigned int *free_total)
{
	struct arena_block *block;
	unsigned int largest = 0;

	*free_total = 0;
	if (!room_open(room))
		return 0;
	for (block = (struct arena_block *)room->start; block; block = arena_next(room, block))
		if (!block->used)
		{
			*free_total += block->size;
			if (block->size > largest)
				largest = block->size;
		}
	return largest;
}

/* what the open rooms have free */
static unsigned int rooms_free(void)
{
	unsigned int bytes = 0;
	int k;

	for (k = 0; k < 2; k++)
		if (room_open(&rooms[k]))
			bytes += rooms[k].bytes - (unsigned int)rooms[k].used;
	return bytes;
}

/* a copy with a piece in the carve */
static int carve_holds(const struct level_stream *stream)
{
	return room_of(stream->base) == CARVE_ROOM || room_of(stream->lightmap) == CARVE_ROOM ||
		room_of(stream->normals) == CARVE_ROOM || room_of(stream->radiosity) == CARVE_ROOM ||
		room_of(stream->compact_base) == CARVE_ROOM || room_of(stream->compact_lightmap) == CARVE_ROOM;
}

/* the copies with a piece in the carve let go, the carve itself not read
(copy_free leaves its pieces be): their next draws build them again */
static int __attribute__((cold)) carve_let_go_copies(void)
{
	int index, count = 0;

	if (!CARVE_ROOM->start)
		return 0;
	carve_dropping = 1;
	for (index = 0; index < LEVEL_STREAMS; index++)
		if (level_streams[index].base && carve_holds(&level_streams[index]))
		{
			level_stream_let_go(&level_streams[index]);
			count++;
		}
	carve_dropping = 0;
	return count;
}

/* at a frame's end (the GE done, no copy being built): a carve lost to a
sleep lets its copies go, and starts again empty once the block is back */
static void __attribute__((cold)) carve_frame_end(void)
{
	static int dropped = 0;
	unsigned int generation;
	void *at;

	if (!carve_lost || !CARVE_ROOM->start)
		return;
	if (carve_lost == 1)
	{
		carve_lost = 2;
		dropped = carve_let_go_copies();
		carve_sleeps++;
	}
	at = psp_tex_carve_now(&generation);
	if (!at || generation == carve_generation)
		return;
	if (at != CARVE_ROOM->start)
	{
		platform_log("GE: the level carve: the texture RAM cache came back with its carve at %08x, not %08x: none for the rest of the map",
			(unsigned int)at, (unsigned int)CARVE_ROOM->start);
		carve_lost = 0;
		carve_release();
		carve_allowed = 0;
		return;
	}
	carve_generation = generation;
	room_reset(CARVE_ROOM);
	carve_lost = 0;
	platform_log("GE: the level carve back after a sleep: the %d copies it held let go, built again as drawn", dropped);
}

/* the map's tags are loaded and its geometry cache placed (psp_geo.c):
whether this map may carve, and how much. Nothing is taken yet. The copies of
the last map are gone (psp_ge_forget_map) */
void psp_ge_level_carve(int carve_from_textures)
{
	extern int psp_showcase_map;
	int kb = psp_keep_number("levelcarve", LEVEL_CARVE_DEFAULT_KB);

	/* the geometry cache has carved afresh, so the last carve's bytes are
	the texture cache's again whatever happened to them */
	carve_let_go_copies();
	CARVE_ROOM->start = NULL;
	CARVE_ROOM->used = 0;
	carve_lost = 0;
	carve_allowed = carve_pack_short = 0;
	carve_given = carve_want = carve_idle = 0;
	carve_floor = 0;
	carve_largest = 0;
	carve_peak = carve_pieces = carve_pieces_peak = carve_steps = carve_returns = 0;
	if (!carve_from_textures)
		return; /* the geometry cache from the heap: the textures keep the whole block */
	if (kb <= 0 || psp_showcase_map || psp_keep("noarena") || psp_keep_number("streamsqueeze", 0) > 0)
	{
		platform_log("GE: the level carve: never on this map (%s)", kb <= 0 ? "levelcarve0" : psp_showcase_map ?
			"the showcase map" : psp_keep("noarena") ? "noarena" : "streamsqueeze");
		return;
	}
	if (kb > 2048)
		kb = 2048;
	carve_most = (unsigned int)kb * 1024;
	carve_below = psp_tex_carved();
	carve_allowed = 1;
	platform_log("GE: the level carve: up to %d KB from the texture RAM cache, below the %u KB the geometry and animation caches carved, taken %u KB at a time only when the copies have no room in the arena or the heap; the texture RAM cache has %u KB",
		kb, carve_below / 1024, LEVEL_CARVE_STEP / 1024, psp_tex_ram_textures() / 1024);
	/* "heaproom256": the arena's missing share taken now, kept to the map's
	end, on top of what the carve may take as it goes (none with the arena
	off: no room for it in the heap, and the copies go there whole) */
	if (arena_size() < ARENA_BYTES && arena_state != 0)
	{
		unsigned int floor_bytes = ARENA_BYTES - arena_size();

		carve_most += floor_bytes;
		if (carve_grow(floor_bytes))
		{
			carve_floor = carve_given;
			platform_log("GE: heaproom: %u KB of the copies' room in the level carve for this map, the arena keeping %u KB of the heap",
				carve_floor / 1024, arena_room_bytes / 1024);
		}
		else
		{
			carve_most -= floor_bytes;
			platform_log("GE: heaproom: no %u KB for the copies in the level carve on this map: the arena's %u KB and the heap",
				floor_bytes / 1024, arena_room_bytes / 1024);
		}
	}
}

/* the carve given back whole to the texture cache; its copies let go */
static void carve_release(void)
{
	if (!CARVE_ROOM->start)
		return;
	carve_let_go_copies();
	if (carve_given && psp_tex_carved() >= carve_given)
		psp_tex_carve(psp_tex_carved() - carve_given);
	CARVE_ROOM->start = NULL;
	CARVE_ROOM->used = 0;
	carve_given = carve_idle = 0;
	carve_floor = 0;
	carve_largest = 0;
	carve_lost = 0;
	carve_returns++;
}

/* the map is left: the carve given back (the next map's geometry cache
carves afresh) */
static void carve_close(void)
{
	carve_release();
	carve_allowed = 0;
}

/* at a frame's end (the GE done with the textures, no copy being built): the
carve grown downward by whole steps to cover deficit bytes, up to its most;
the textures resident in what it takes are forgotten (psp_tex_carve). 1: grown */
static int __attribute__((cold)) carve_grow(unsigned int deficit)
{
	unsigned int step = (deficit + LEVEL_CARVE_STEP - 1) / LEVEL_CARVE_STEP * LEVEL_CARVE_STEP;
	static unsigned int logged = 0;
	char *at;

	if (!carve_allowed || carve_lost || !deficit)
		return 0;
	if (step > carve_most - carve_given)
		step = carve_most - carve_given;
	if (!step)
	{
		if (!(logged & 1))
			platform_log("GE: the level carve is at its most (%u KB) and the copies want %u KB more (levelcarveN)",
				carve_given / 1024, deficit / 1024);
		logged |= 1;
		return 0;
	}
	/* the carve lies just below the other caches' shares: anything else
	carved since would sit between */
	if (psp_tex_carved() != carve_below + carve_given ||
		!(at = psp_tex_carve(carve_below + carve_given + step)))
	{
		if (!(logged & 2))
			platform_log("GE: the level carve can't grow by %u KB (%u KB carved, %u expected): the copies wait or use the heap",
				step / 1024, psp_tex_carved() / 1024, (carve_below + carve_given) / 1024);
		logged |= 2;
		return 0;
	}
	if (!CARVE_ROOM->start)
	{
		CARVE_ROOM->start = at;
		CARVE_ROOM->bytes = step;
		room_reset(CARVE_ROOM);
		psp_tex_carve_now(&carve_generation);
	}
	else
	{
		/* the new step lies right below the carve: a block of its own, given
		back so it merges with the carve's first block if that is free */
		struct arena_block *first = (struct arena_block *)at, *old = (struct arena_block *)CARVE_ROOM->start;

		old->previous = step;
		first->size = step;
		first->previous = 0;
		first->used = 1;
		first->owner = NULL;
		CARVE_ROOM->start = at;
		CARVE_ROOM->bytes += step;
		CARVE_ROOM->used += step;
		room_give(CARVE_ROOM, first);
	}
	carve_given += step;
	carve_steps++;
	carve_idle = 0;
	if (carve_steps <= 16)
		platform_log("GE: the level carve grew %u KB to %u of %u KB at %08x (the copies wanted %u KB more); the texture RAM cache keeps %u KB",
			step / 1024, carve_given / 1024, carve_most / 1024, (unsigned int)at, deficit / 1024, psp_tex_ram_textures() / 1024);
	return 1;
}

/* at a frame's end, after the arena's housekeeping: what the copies lacked
this frame (pieces no room took, and the materials waiting for more room
than the rooms have, copies not drawn this frame counted as room), the carve
grown for it; a carve left empty given back */
static void __attribute__((cold)) carve_frame_grow(void)
{
	unsigned int deficit = carve_want;
	int k, index;

	carve_want = 0;
	if (!carve_allowed || carve_lost)
	{
		carve_pack_short = 0;
		return;
	}
	if (stream_room > 0 && stream_waiting_count)
	{
		unsigned int need = 0, room = rooms_free();

		for (k = 0; k < stream_waiting_count; k++)
			if (stream_waiting[k].wanted == ge_hot.stream_frame)
				need += stream_room_need(stream_waiting[k].count, stream_waiting[k].lightmap_vertices != NULL, level_compact > 0);
		if (need)
		{
			for (index = 0; index < LEVEL_STREAMS; index++)
			{
				const struct level_stream *stream = &level_streams[index];
				struct copy_room *in = stream->base ? room_of(stream->base) : NULL;

				if (in && room_open(in) && stream->last_frame != ge_hot.stream_frame)
					room += stream_room_need(stream->count, stream->lightmap != NULL, stream->compact_base != NULL);
			}
			if (need > room && need - room > deficit)
				deficit = need - room;
		}
	}
	/* the bytes were there but not in blocks the pieces fit */
	if (carve_pack_short && deficit < LEVEL_CARVE_STEP)
		deficit = LEVEL_CARVE_STEP;
	carve_pack_short = 0;
	if (deficit && carve_grow(deficit))
		for (k = 0; k < stream_waiting_count; k++)
			stream_waiting[k].arena_after = ge_hot.stream_frame;
	if (CARVE_ROOM->start && !carve_lost)
	{
		if (CARVE_ROOM->used || carve_floor)
			carve_idle = 0;
		else if (++carve_idle >= CARVE_IDLE_FRAMES)
		{
			carve_release();
			platform_log("GE: the level carve, empty for %d frames, given back to the texture RAM cache (%u KB there now)",
				CARVE_IDLE_FRAMES, psp_tex_ram_textures() / 1024);
		}
	}
}

/* with the copies' room reports: what the carve holds, and the most it has */
static void __attribute__((cold)) carve_report(void)
{
	int index, copies = 0;

	if (!carve_allowed)
		return;
	if (CARVE_ROOM->start)
		for (index = 0; index < LEVEL_STREAMS; index++)
			if (level_streams[index].base && carve_holds(&level_streams[index]))
				copies++;
	platform_log("GE: the level carve: %u of %u KB taken (%lu steps, given back %lu times), the texture RAM cache keeping %u KB; %d copies with pieces in it now (%lu pieces, %lu KB used, largest free %u KB); peak %lu KB, %lu pieces; lost to a sleep %lu times%s",
		carve_given / 1024, carve_most / 1024, carve_steps, carve_returns, psp_tex_ram_textures() / 1024, copies, carve_pieces,
		CARVE_ROOM->start ? CARVE_ROOM->used / 1024 : 0, carve_largest / 1024, carve_peak / 1024, carve_pieces_peak,
		carve_sleeps, carve_lost ? " (lost now)" : "");
}

/* the power callback's thread, before the PSP sleeps: the carve goes back to
the system with the texture RAM cache */
void psp_ge_level_carve_suspend(void)
{
	if (CARVE_ROOM->start)
		carve_lost = 1;
}

static void __attribute__((cold)) arena_frame_end(void)
{
	unsigned int free_total[2], largest[2];
	int overflowed, k, compacted = 0;

	if (arena_state <= 0)
		return;
	carve_frame_end();
	for (k = 0; k < 2; k++)
		largest[k] = room_largest(&rooms[k], &free_total[k]);
	arena_largest = largest[0];
	carve_largest = largest[1];
	if (arena_compact_on < 0)
		arena_compact_on = !psp_keep("nocompact");
	if (arena_compact_on)
	{
		overflowed = arena_from_heap != arena_seen_from_heap;
		arena_seen_from_heap = arena_from_heap;
		for (k = 0; k < 2; k++)
			if (free_total[k] >= 64 * 1024 && largest[k] < free_total[k] / 2 &&
				(overflowed || ge_hot.stream_frame - arena_last_compact >= ARENA_COMPACT_EVERY))
			{
				show2_settle(SHOW2_COMPACT); /* geshow2: it moves copies the GE may be drawing from */
				arena_compact(&rooms[k]);
				compacted = 1;
			}
		if (compacted)
			arena_last_compact = ge_hot.stream_frame;
		if (arena_heap_live)
			arena_migrate();
	}
	carve_frame_grow();
}

/* level_stream_get's question: 1 when the material waits for room (its draw
skips it). The frame end's own build passes */
static int stream_room_ask(const void *vertices, const void *lightmap_vertices, int count)
{
	int k;

	stream_asked = vertices;
	stream_asked_lightmap = lightmap_vertices;
	stream_asked_count = count;
	if (stream_room_building)
		return 0;
	for (k = 0; k < stream_waiting_count; k++)
		if (stream_waiting[k].vertices == vertices)
		{
			stream_waiting[k].wanted = ge_hot.stream_frame;
			stream_room_skips++;
			return 1;
		}
	return 0;
}

/* the material just asked for found no room: it waits. With the list full it
is tried again on its next draw, as without the list */
static void stream_room_wait(void)
{
	int k;

	for (k = 0; k < stream_waiting_count && stream_waiting[k].vertices != stream_asked; k++)
		;
	if (k == stream_waiting_count)
	{
		if (k == STREAM_WAITING)
			return;
		stream_waiting[k].vertices = stream_asked;
		stream_waiting[k].lightmap_vertices = stream_asked_lightmap;
		stream_waiting[k].count = stream_asked_count;
		stream_waiting[k].heap_after = ge_hot.stream_frame + STREAM_HEAP_RETRY;
		stream_waiting[k].arena_after = ge_hot.stream_frame;
		stream_waiting[k].texture = stream_asked_texture;
		stream_waiting_count++;
		if (++stream_room_waits <= 8)
		{
			extern int psp_tex_size(unsigned int key, int *width, int *height);
			int width = 0, height = 0;

			psp_tex_size(stream_asked_texture, &width, &height);
			platform_log("GE: no room for a material's copy (%d vertices, %u KB; base texture %08x, %dx%d): it waits for room at a frame's end",
				stream_asked_count, stream_room_need(stream_asked_count, stream_asked_lightmap != NULL, level_compact > 0) / 1024,
				stream_asked_texture, width, height);
		}
	}
	stream_waiting[k].wanted = ge_hot.stream_frame;
}

/* what a copy of count vertices takes from the arena, headers included */
static unsigned int stream_room_need(int count, int lightmap, int compact)
{
	unsigned int header = sizeof(struct arena_block) + ARENA_ALIGN - 1, need;

	need = (((unsigned int)count * sizeof(struct ge_vertex) + header) & ~(ARENA_ALIGN - 1)) +
		(((unsigned int)count * sizeof(unsigned int) + header) & ~(ARENA_ALIGN - 1));
	if (lightmap)
		need *= 2;
	if (compact)
		need += (lightmap ? 2 : 1) * (((unsigned int)count * sizeof(struct compact_vertex) + header) & ~(ARENA_ALIGN - 1));
	return need;
}

unsigned int psp_geo_address_of(const void *ptr);
unsigned int psp_geo_generation(const void *ptr);
int psp_geo_open_count(void);

/* build 68: what a material's vertices are known by. In the tags, their
address; in the geometry cache, their address in the pack, since the block
they sit in can be reused for other vertices once they are evicted */
int psp_geo_external(unsigned int address);
void *psp_geo_lock(unsigned int address, unsigned int size);
unsigned int psp_geo_level_stride(void);

static const void *stream_key(const void *vertices)
{
	unsigned int external;

	/* the level's hooks pass the pack address itself for vertices kept
	outside (psp_fixups.py): the copies are what's drawn, and the pack's
	bytes are only asked for to build them */
	if (psp_geo_external((unsigned int)vertices))
		return vertices;
	external = psp_geo_address_of(vertices);
	return external ? (const void *)external : vertices;
}

/* ---------- build 87: the stencil bake (tools/stencilbake.py, <map>.pstn)

A stencil (a light strip on a base wall, the cave's fungus) is its own
see-through surface a few millimetres in front of a wall: the Xbox's 24-bit
depth keeps them apart, the PSP's 16 bits can't, and no nudge was right at
every distance and slant (builds 52-85). The bake cuts each wall triangle
under a stencil along the stencil's outline (a constrained triangulation,
no T-junctions with the triangles beside it). Here:
	- a wall triangle the bake replaced is drawn as its pieces, in every pass
	  (base, lightmap, detail; shadows and lights too), from vertices added
	  to the material's copies;
	- the stencil is drawn on the very same pieces (same positions, same
	  matrices), only where its wall triangle was drawn this frame, with
	  depth EQUAL, as the lightmap and detail passes are: exact, no nudge;
	- its triangles the bake took are left out of the old path; any it
	  couldn't (not on a wall: the teleporter's beam) keep the old nudge.
"Stencils" in the debug menu: baked with EQUAL / baked with GEQUAL and two
depth steps of slack / the old nudge (the bake off) */

struct bake_affected
{
	unsigned short a, b, c, first, count, pad;
};

struct bake_wall
{
	unsigned int key, original, added, affected_count, piece_count;
	const float *added_vertices;          /* 8 words each: x y z u v lu lv, packed normal */
	const struct bake_affected *affected;
	const unsigned short *pieces;         /* 3 each, into the extended copies */
	unsigned char *flags;                 /* per original vertex: in a replaced triangle */
	unsigned short *hash;                 /* replaced triangle + 1, by its triple */
	unsigned int hash_mask;
	unsigned int *drawn_frame;            /* per replaced triangle: the frame it was drawn in */
};

struct bake_layer
{
	unsigned int wall_key, stencil_key, vertex_count, tri_count;
	struct ge_vertex *vertices;           /* the stencil's texture coordinates on the pieces' positions */
	const unsigned short *tris;           /* 4 each: three vertices, the wall's replaced triangle */
	struct bake_wall *wall;
	/* the origin the positions are given from, once placed as the wall's
	copy is (level_layers_follow); unplaced, they are the file's world ones */
	float origin[3];
	int placed;
};

struct bake_stencil
{
	unsigned int key, count;
	const unsigned short *triples;
	unsigned short *hash;
	unsigned int hash_mask;
};

static struct
{
	void *file;
	int wall_count, layer_count, stencil_count;
	struct bake_wall *walls;
	struct bake_layer *layers;
	struct bake_stencil *stencils;
} bake;
/* 0 baked, EQUAL; 1 baked, GEQUAL with STENCIL_SLACK steps; 2 the old nudge */
static int stencil_mode = 0;
#define STENCIL_SLACK 2
static unsigned long bake_substituted = 0, bake_pieces_drawn = 0, bake_layer_draws = 0, bake_layer_triangles = 0,
	bake_old_path_triangles = 0;

static unsigned int bake_triple_hash(unsigned int a, unsigned int b, unsigned int c)
{
	return (a * 73856093u) ^ (b * 19349663u) ^ (c * 83492791u);
}

static void bake_forget(void)
{
	int k;

	for (k = 0; k < bake.wall_count; k++)
	{
		free(bake.walls[k].flags);
		free(bake.walls[k].hash);
		free(bake.walls[k].drawn_frame);
	}
	for (k = 0; k < bake.stencil_count; k++)
		free(bake.stencils[k].hash);
	free(bake.walls);
	free(bake.layers);
	free(bake.stencils);
	free(bake.file);
	memset(&bake, 0, sizeof(bake));
}

static struct bake_wall *bake_wall_of(unsigned int key)
{
	int k;

	if (stencil_mode == 2)
		return NULL;
	for (k = 0; k < bake.wall_count; k++)
		if (bake.walls[k].key == key)
			return &bake.walls[k];
	return NULL;
}

static struct bake_stencil *bake_stencil_of(unsigned int key)
{
	int k;

	if (stencil_mode == 2)
		return NULL;
	for (k = 0; k < bake.stencil_count; k++)
		if (bake.stencils[k].key == key)
			return &bake.stencils[k];
	return NULL;
}

/* The stencil bake is one block (up to ~162 KB on c20), and after a level
change the heap's only free piece that large is its top, whose size follows
what earlier levels left: the load's own small requests could eat it before
the geometry pack opened and asked (the heap ladder runs). So the block is
taken right after the old map is let go, when the top is at its largest,
and if even then there's no room, asked for again once the load has let go
of its temporaries. "stencilsold": asked for only when the pack opens */
static char stencils_next[64];
static void *stencils_held = NULL;
static int stencils_held_size = 0;
static char stencils_pending[64];
static unsigned int stencils_pending_crc = 0;
static int stencils_old = -1;
static int stencils_retrying = 0;

static int stencils_old_way(void)
{
	if (stencils_old < 0)
		stencils_old = psp_keep("stencilsold") != 0;
	return stencils_old;
}

/* the next map, as the loading screen names it (psp_loading.c) */
void psp_ge_stencils_expect(const char *map_name)
{
	const char *slash = map_name ? strrchr(map_name, '\\') : NULL;

	snprintf(stencils_next, sizeof(stencils_next), "%s", slash ? slash + 1 : map_name ? map_name : "");
}

static void bake_forget(void);
void psp_ge_stencils_open(const char *name, unsigned int map_crc);

/* the end of game_dispose_from_old_map (psp_fixups.py): the old map's bake
let go, and the next map's block taken while the heap's top is whole */
void psp_ge_stencils_reserve(void)
{
	char path[128];
	SceUID file;
	int size;

	if (stencils_old_way() || !stencils_next[0])
		return;
	GE_DEFER_DRAIN();
	bake_forget();
	free(stencils_held);
	stencils_held = NULL;
	stencils_held_size = 0;
	snprintf(path, sizeof(path), PSP_MAPS_ROOT "/%s.pstn", stencils_next);
	stencils_next[0] = 0;
	file = sceIoOpen(path, PSP_O_RDONLY, 0);
	if (file < 0)
		return;
	size = sceIoLseek32(file, 0, PSP_SEEK_END);
	sceIoClose(file);
	if (size <= 64)
		return;
	/* a refusal here isn't a failure: the map's own load asks again */
	{
		extern void *psp_heap_try_memalign(size_t alignment, size_t size);

		stencils_held = psp_heap_try_memalign(64, size);
	}
	if (stencils_held)
		stencils_held_size = size;
	platform_log("stencils: %s's %d KB %s as the old map was let go", path, size / 1024,
		stencils_held ? "taken" : "found no room");
}

/* the end of the load (psp_loading.c): a bake that found no room when its
pack opened is asked for again, the load's temporaries let go */
void psp_ge_stencils_retry(void)
{
	char name[64];

	if (!stencils_pending[0])
		return;
	snprintf(name, sizeof(name), "%s", stencils_pending);
	stencils_pending[0] = 0;
	platform_log("stencils: %s asked for again at the load's end", name);
	stencils_retrying = 1;
	psp_ge_stencils_open(name, stencils_pending_crc);
	stencils_retrying = 0;
}

/* the map's .pstn, when its geometry pack opens (psp_geo.c) */
void psp_ge_stencils_open(const char *name, unsigned int map_crc)
{
	GE_DEFER_DRAIN(); /* build 112: gedefer */
	char path[128];
	SceUID file;
	int size, k;
	unsigned int header[16];
	const unsigned char *at, *end;
	static int mode_word = -1;

	/* the debug menu's modes from keep.txt, for A/Bs without hands:
	"stencilsslack" baked with two steps of slack, "stencilsnudge" the bake
	off (the old nudge) */
	if (mode_word < 0)
	{
		mode_word = psp_keep("stencilsnudge") ? 2 : psp_keep("stencilsslack") ? 1 : 0;
		if (mode_word)
			stencil_mode = mode_word;
	}
	bake_forget();
	snprintf(path, sizeof(path), PSP_MAPS_ROOT "/%s.pstn", name);
	file = sceIoOpen(path, PSP_O_RDONLY, 0);
	if (file < 0)
	{
		platform_log("stencils: no %s: the old nudge for every stencil", path);
		return;
	}
	size = sceIoLseek32(file, 0, PSP_SEEK_END);
	sceIoLseek32(file, 0, PSP_SEEK_SET);
	if (size == 64)
	{
		/* the header alone: the bake found no wall under a decal to cut */
		sceIoClose(file);
		platform_log("stencils: %s holds no cut walls (none on this map): the old nudge for every stencil", path);
		return;
	}
	if (stencils_held && stencils_held_size == size)
	{
		bake.file = stencils_held;
		stencils_held = NULL;
		stencils_held_size = 0;
	}
	else
	{
		free(stencils_held);
		stencils_held = NULL;
		stencils_held_size = 0;
		/* the load's end asks once more (below), so only that last ask is
		a failure the overlay should count */
		if (size <= 64)
			bake.file = NULL;
		else if (stencils_retrying || stencils_old_way())
			bake.file = memalign(64, size);
		else
		{
			extern void *psp_heap_try_memalign(size_t alignment, size_t size);

			bake.file = psp_heap_try_memalign(64, size);
		}
		if (!bake.file && size > 64 && !stencils_old_way() && !stencils_retrying)
		{
			/* no room while the load holds its temporaries: once more at its end */
			sceIoClose(file);
			snprintf(stencils_pending, sizeof(stencils_pending), "%s", name);
			stencils_pending_crc = map_crc;
			platform_log("stencils: %s found no room for %d KB while the map loads; asked again at the load's end", path, size / 1024);
			return;
		}
	}
	if (!bake.file || sceIoRead(file, bake.file, size) != size)
	{
		sceIoClose(file);
		platform_log("stencils: %s unreadable (%d bytes)", path, size);
		bake_forget();
		return;
	}
	sceIoClose(file);
	memcpy(header, bake.file, sizeof(header));
	if (header[0] != 0x4E545350 /* PSTN */ || header[1] != 1)
	{
		platform_log("stencils: %s is not a stencil bake", path);
		bake_forget();
		return;
	}
	if (map_crc && header[2] != map_crc)
	{
		platform_log("stencils: %s was baked from another build of the map (crc %08x, map %08x): not used", path,
			header[2], map_crc);
		bake_forget();
		return;
	}
	bake.walls = calloc(header[3] + 1, sizeof(struct bake_wall));
	bake.layers = calloc(header[4] + 1, sizeof(struct bake_layer));
	bake.stencils = calloc(header[5] + 1, sizeof(struct bake_stencil));
	if (!bake.walls || !bake.layers || !bake.stencils)
	{
		bake_forget();
		return;
	}
	at = (const unsigned char *)bake.file + 64;
	end = (const unsigned char *)bake.file + size;
	/* build 89: every record checked against the file's end and its indices
	against their arrays (a damaged or half-copied file is refused whole),
	and stored at the count of good ones: b87-b88 stored at the loop index
	while counting only successes, so a wall whose allocation failed sat
	inside the counted range with NULL tables (review) */
#define BAKE_ROOM(bytes) ((unsigned int)(end - at) >= (unsigned int)(bytes))
	for (k = 0; k < (int)header[3]; k++)
	{
		struct bake_wall *w = &bake.walls[bake.wall_count];
		const unsigned int *h = (const unsigned int *)at;
		unsigned int i, slots, vertex_limit;

		if (!BAKE_ROOM(20) || h[2] > 65535 || h[3] > 65535 || h[4] > 1000000 ||
			!BAKE_ROOM(20 + h[2] * 32 + h[3] * 12 + h[4] * 6))
			goto damaged;
		w->key = h[0]; w->original = h[1]; w->added = h[2]; w->affected_count = h[3]; w->piece_count = h[4];
		at += 20;
		w->added_vertices = (const float *)at;
		at += w->added * 32;
		w->affected = (const struct bake_affected *)at;
		at += w->affected_count * 12;
		w->pieces = (const unsigned short *)at;
		at += w->piece_count * 6;
		at = (const unsigned char *)(((unsigned int)at + 3) & ~3u);
		if (at > end || w->original > 65535)
			goto damaged;
		vertex_limit = w->original + w->added;
		for (i = 0; i < w->affected_count; i++)
			if ((unsigned int)w->affected[i].first + w->affected[i].count > w->piece_count)
				goto damaged;
		for (i = 0; i < w->piece_count * 3; i++)
			if (w->pieces[i] >= vertex_limit)
				goto damaged;
		for (slots = 16; slots < w->affected_count * 2; slots <<= 1)
			;
		w->hash_mask = slots - 1;
		w->hash = calloc(slots, sizeof(unsigned short));
		w->flags = calloc(w->original ? w->original : 1, 1);
		/* written by the replay (on the ME under geinflight): whole lines of
		its own, as psp_tex.c's line_calloc says why */
		{
			size_t bytes = ((w->affected_count ? w->affected_count : 1) * sizeof(unsigned int) + 63) & ~(size_t)63;

			w->drawn_frame = psp_keep("texlinesold") ? calloc(bytes / sizeof(unsigned int), sizeof(unsigned int)) :
				memalign(64, bytes);
			if (w->drawn_frame)
				memset(w->drawn_frame, 0, bytes);
		}
		if (!w->hash || !w->flags || !w->drawn_frame)
		{
			free(w->hash);
			free(w->flags);
			free(w->drawn_frame);
			memset(w, 0, sizeof(*w));
			continue;
		}
		for (i = 0; i < w->affected_count; i++)
		{
			const struct bake_affected *t = &w->affected[i];
			unsigned int slot = bake_triple_hash(t->a, t->b, t->c) & w->hash_mask;

			while (w->hash[slot])
				slot = (slot + 1) & w->hash_mask;
			w->hash[slot] = (unsigned short)(i + 1);
			if (t->a < w->original) w->flags[t->a] = 1;
			if (t->b < w->original) w->flags[t->b] = 1;
			if (t->c < w->original) w->flags[t->c] = 1;
		}
		bake.wall_count++;
	}
	for (k = 0; k < (int)header[4]; k++)
	{
		struct bake_layer *l = &bake.layers[bake.layer_count];
		const unsigned int *h = (const unsigned int *)at;
		unsigned int i;
		int w;

		if (!BAKE_ROOM(16) || h[2] > 65535 || h[3] > 65535 || !BAKE_ROOM(16 + h[2] * 20 + h[3] * 8))
			goto damaged;
		memset(l, 0, sizeof(*l));
		l->wall_key = h[0]; l->stencil_key = h[1]; l->vertex_count = h[2]; l->tri_count = h[3];
		at += 16;
		l->vertices = (struct ge_vertex *)at;
		at += l->vertex_count * 20;
		l->tris = (const unsigned short *)at;
		at += l->tri_count * 8;
		for (i = 0; i < l->tri_count; i++)
			if (l->tris[4 * i] >= l->vertex_count || l->tris[4 * i + 1] >= l->vertex_count ||
				l->tris[4 * i + 2] >= l->vertex_count)
				goto damaged;
		for (w = 0; w < bake.wall_count; w++)
			if (bake.walls[w].key == l->wall_key)
				l->wall = &bake.walls[w];
		if (l->wall)
			bake.layer_count++;
	}
	for (k = 0; k < (int)header[5]; k++)
	{
		struct bake_stencil *st = &bake.stencils[bake.stencil_count];
		const unsigned int *h = (const unsigned int *)at;
		unsigned int i, slots;

		if (!BAKE_ROOM(8) || h[1] > 65535 || !BAKE_ROOM(8 + h[1] * 6))
			goto damaged;
		st->key = h[0]; st->count = h[1];
		at += 8;
		st->triples = (const unsigned short *)at;
		at += st->count * 6;
		at = (const unsigned char *)(((unsigned int)at + 3) & ~3u);
		if (at > end)
			goto damaged;
		for (slots = 16; slots < st->count * 2; slots <<= 1)
			;
		st->hash_mask = slots - 1;
		st->hash = calloc(slots, sizeof(unsigned short));
		if (!st->hash)
			continue;
		for (i = 0; i < st->count; i++)
		{
			unsigned int slot = bake_triple_hash(st->triples[3 * i], st->triples[3 * i + 1], st->triples[3 * i + 2]) & st->hash_mask;

			while (st->hash[slot])
				slot = (slot + 1) & st->hash_mask;
			st->hash[slot] = (unsigned short)(i + 1);
		}
		bake.stencil_count++;
	}
	/* the layers' vertices are read by the GE */
	ge_writeback(bake.file, size);
	platform_log("stencils: %s: %d walls, %d stencil layers, %d stencil materials baked", path, bake.wall_count,
		bake.layer_count, bake.stencil_count);
	return;
damaged:
	platform_log("stencils: %s is damaged (a record runs past the file or names a vertex it doesn't have): not used, the old nudge for every stencil", path);
	bake_forget();
#undef BAKE_ROOM
}

/* which replaced triangle a triple is (-1: none) */
static int bake_find(const struct bake_wall *w, unsigned int a, unsigned int b, unsigned int c)
{
	unsigned int slot;

	if (a >= w->original || b >= w->original || c >= w->original || !w->flags[a] || !w->flags[b] || !w->flags[c])
		return -1;
	slot = bake_triple_hash(a, b, c) & w->hash_mask;
	while (w->hash[slot])
	{
		const struct bake_affected *t = &w->affected[w->hash[slot] - 1];

		if (t->a == a && t->b == b && t->c == c)
			return w->hash[slot] - 1;
		slot = (slot + 1) & w->hash_mask;
	}
	return -1;
}

static void *list_take(int bytes);

/* a draw's triangles with the replaced ones swapped for their pieces (in the
display list's memory); the game's own list when none are in it. mark: the
pieces are being drawn (the stencils on them may be drawn this frame) */
static const unsigned short *bake_substitute(struct level_stream *stream, const unsigned short *indices,
	long *triangle_count, int mark)
{
	struct bake_wall *w = stream->bake;
	long t, extra = 0, found = 0, out = 0;
	unsigned short *list;

	if (!w || stencil_mode == 2)
		return indices;
	for (t = 0; t < *triangle_count; t++)
	{
		int r = bake_find(w, indices[3 * t], indices[3 * t + 1], indices[3 * t + 2]);

		if (r >= 0)
		{
			extra += w->affected[r].count - 1;
			found++;
		}
	}
	if (!found)
		return indices;
	list = (unsigned short *)list_take((int)((*triangle_count + extra) * 3 * sizeof(unsigned short) + 3) & ~3);
	if (!list)
		return indices;
	for (t = 0; t < *triangle_count; t++)
	{
		const unsigned short *tri = indices + 3 * t;
		int r = bake_find(w, tri[0], tri[1], tri[2]);

		if (r < 0)
		{
			list[3 * out] = tri[0];
			list[3 * out + 1] = tri[1];
			list[3 * out + 2] = tri[2];
			out++;
			continue;
		}
		memcpy(list + 3 * out, w->pieces + 3 * w->affected[r].first, w->affected[r].count * 3 * sizeof(unsigned short));
		out += w->affected[r].count;
		if (mark)
		{
			w->drawn_frame[r] = ge_hot.stream_frame;
			bake_pieces_drawn += w->affected[r].count;
		}
	}
	if (mark)
		bake_substituted += found;
	*triangle_count = out;
	return list;
}

/* build 89: what a material's copies take: the float copies, the packed
normals and incident radiosity, the 16-bit copies (the budget's count; the
16-bit ones and the radiosity were left out before) */
static unsigned int stream_size(const struct level_stream *stream)
{
	unsigned int count = (unsigned int)stream->count, size;

	size = count * sizeof(struct ge_vertex) * (stream->lightmap ? 2 : 1) + count * sizeof(unsigned int);
	if (stream->radiosity)
		size += count * sizeof(unsigned int);
	if (stream->compact_base)
		size += count * sizeof(struct compact_vertex);
	if (stream->compact_lightmap)
		size += count * sizeof(struct compact_vertex);
	return size;
}

/* build 112: level copies that work marked as the Media Engine's needed and
couldn't build (gemesim): building reads the geometry cache, which may start
a stick read, and may fall back on the heap, all system calls. The main CPU
builds them at the frame's end (psp_ge_frame_end), so they're there for the
next frame; the draw is skipped meanwhile, as for geometry still on its way.
Build 113: 128 (b112's Test 3 asked for 113 in a frame and 14 didn't fit;
12 bytes each) */
#define STREAMS_WANTED 128
static struct
{
	const void *vertices, *lightmap_vertices;
	int count;
	unsigned int texture;
} streams_wanted[STREAMS_WANTED];
static int streams_wanted_count = 0;
static unsigned long streams_wanted_total = 0, streams_wanted_dropped = 0;

/* a level copy already built, or NULL; changes nothing (with the shadows) */
static struct level_stream *level_stream_find(const void *vertices);
/* "levelpretouch" (psp_ge_draw_level): copies built as their draws were written down, and those without room */
static unsigned long level_pretouches = 0, level_pretouch_missed = 0;

/* a wall's stencil layers given from its copy's origin: they lie on the
wall's pieces, drawn with depth EQUAL, so they take the same snapped
positions from the same origin. Main CPU only, when nothing reads them */
static void level_layers_follow(unsigned int wall_key, const float *origin)
{
	int k;

	if (!level_local)
		return;
	for (k = 0; k < bake.layer_count; k++)
	{
		struct bake_layer *l = &bake.layers[k];
		unsigned int i;

		if (l->wall_key != wall_key || (l->placed && !memcmp(l->origin, origin, sizeof(l->origin))))
			continue;
		for (i = 0; i < l->vertex_count; i++)
		{
			float *p = &l->vertices[i].x;
			int axis;

			for (axis = 0; axis < 3; axis++)
				p[axis] = (l->placed ? p[axis] + l->origin[axis] : level_snap(p[axis])) - origin[axis];
		}
		memcpy(l->origin, origin, sizeof(l->origin));
		l->placed = 1;
		ge_writeback(l->vertices, l->vertex_count * sizeof(struct ge_vertex));
	}
}

/* a copy moved to another origin: exact (lattice points, snapped positions,
within LEVEL_REACH), or left where it is */
static void level_copy_follow(struct level_stream *stream, const float *origin)
{
	float shift[3];
	int i, axis;

	/* a carve lost to a sleep is the system's: drawn from its own origin */
	if (!level_reach(stream->low, stream->high, origin) || (carve_lost && carve_holds(stream)))
	{
		ge_hot.level_copies_left++;
		return;
	}
	for (axis = 0; axis < 3; axis++)
		shift[axis] = stream->origin[axis] - origin[axis];
	for (i = 0; i < stream->count; i++)
	{
		struct ge_vertex *b = &stream->base[i];

		b->x += shift[0]; b->y += shift[1]; b->z += shift[2];
		if (stream->lightmap)
		{
			struct ge_vertex *l = &stream->lightmap[i];

			l->x = b->x; l->y = b->y; l->z = b->z;
		}
	}
	memcpy(stream->origin, origin, sizeof(stream->origin));
	ge_writeback(stream->base, stream->count * sizeof(struct ge_vertex));
	vm_level_note(stream->base, stream->count * sizeof(struct ge_vertex));
	if (stream->lightmap)
	{
		ge_writeback(stream->lightmap, stream->count * sizeof(struct ge_vertex));
		vm_level_note(stream->lightmap, stream->count * sizeof(struct ge_vertex));
	}
	if (stream->bake)
		level_layers_follow(stream->bake->key, origin);
	level_copies_moved++;
	level_vertices_moved += (unsigned long)stream->count;
}

static struct level_stream *level_stream_get(const void *vertices, const void *lightmap_vertices, int count)
{
	const void *key = stream_key(vertices);
	unsigned int slot = ((unsigned int)key >> 5) % LEVEL_STREAMS, tries = 0;
	struct level_stream *stream;
	int index;
	/* the bytes of a rendered vertex, and where its uv is (floats): the tags
	and an older pack keep the Xbox's 32, binormal and tangent included */
	unsigned int level_stride = 32;
	int uv_at;

	while (level_streams[slot].source && level_streams[slot].source != key)
	{
		slot = (slot + 1) % LEVEL_STREAMS;
		if (++tries >= LEVEL_STREAMS)
			return NULL;
	}
	stream = &level_streams[slot];
	if (stream->source && stream->base)
	{
		stream->last_frame = ge_hot.stream_frame;
		return stream;
	}
	if (stream_room > 0 && stream_room_ask(vertices, lightmap_vertices, count))
		return NULL;
	if (PSP_ME_WORK())
	{
		int k;

		for (k = 0; k < streams_wanted_count && streams_wanted[k].vertices != vertices; k++)
			;
		if (k == streams_wanted_count)
		{
			if (k < STREAMS_WANTED)
			{
				streams_wanted[k].vertices = vertices;
				streams_wanted[k].lightmap_vertices = lightmap_vertices;
				streams_wanted[k].count = count;
				streams_wanted[k].texture = stream_asked_texture;
				streams_wanted_count++;
				streams_wanted_total++;
			}
			else
				streams_wanted_dropped++;
		}
		return NULL;
	}
	/* new, or let go when over budget (level_streams_trim): built here */
	if (psp_geo_external((unsigned int)vertices))
	{
		/* the pack's bytes: the material's own vertices (level_stride each)
		and, when it has them, the lightmap's (8 each) right after; one block */
		unsigned int address = (unsigned int)vertices;
		const char *raw;

		level_stride = psp_geo_level_stride();
		raw = psp_geo_lock(address, (unsigned int)count * (level_stride + (lightmap_vertices ? 8u : 0u)));
		if (!raw)
			return NULL;
		vertices = raw;
		if (lightmap_vertices)
			lightmap_vertices = raw + level_stride * count;
	}
	stream->source = key;
	stream->count = count;
	stream->original_count = count;
	stream->last_frame = ge_hot.stream_frame;
	stream->uv_per_unit = 0.0f;
	/* build 87: room for the stencil bake's pieces' vertices after the
	material's own */
	stream->bake = bake_wall_of((unsigned int)key);
	if (stream->bake && (stream->bake->original != (unsigned int)count || count + stream->bake->added > 65535))
		stream->bake = NULL;
	{
		int total = count + (stream->bake ? (int)stream->bake->added : 0);

		stream->base = stream_alloc(total * sizeof(struct ge_vertex), (void **)&stream->base);
		stream->lightmap = lightmap_vertices ? stream_alloc(total * sizeof(struct ge_vertex), (void **)&stream->lightmap) : NULL;
		/* build 111: in the copies' arena too (not VRAM: the CPU reads these) */
		stream->normals = copy_alloc(total * sizeof(unsigned int), (void **)&stream->normals);
		stream->radiosity = lightmap_vertices ? copy_alloc(total * sizeof(unsigned int), (void **)&stream->radiosity) : NULL;
		if (stream->radiosity)
			memset(stream->radiosity, 0, total * sizeof(unsigned int));
	}
	if (!stream->base || (lightmap_vertices && (!stream->lightmap || !stream->radiosity)) || !stream->normals)
	{
		if (stream_room > 0)
			stream_room_wait();
		else
			platform_log("GE: no memory for a material's vertices (%d)", count);
		stream_free(stream->base);
		stream_free(stream->lightmap);
		copy_free(stream->normals);
		copy_free(stream->radiosity);
		stream->base = stream->lightmap = NULL;
		stream->normals = stream->radiosity = NULL;
		return NULL;
	}
	stream->low[0] = stream->low[1] = stream->low[2] = 1e30f;
	stream->high[0] = stream->high[1] = stream->high[2] = -1e30f;
	if (level_local < 0)
		level_local = !psp_keep("levellocaloff");
	uv_at = (int)(level_stride - 8) / 4;
	for (index = 0; index < count; index++)
	{
		const float *vertex = (const float *)((const char *)vertices + level_stride * index);
		struct ge_vertex *b = &stream->base[index];
		int axis;

		if (level_local)
		{
			b->x = level_snap(vertex[0]); b->y = level_snap(vertex[1]); b->z = level_snap(vertex[2]);
		}
		else
		{
			b->x = vertex[0]; b->y = vertex[1]; b->z = vertex[2];
		}
		b->u = vertex[uv_at]; b->v = vertex[uv_at + 1];
		stream->normals[index] = ((const unsigned int *)vertex)[3];
		for (axis = 0; axis < 3; axis++)
		{
			float p = (&b->x)[axis];

			if (p < stream->low[axis]) stream->low[axis] = p;
			if (p > stream->high[axis]) stream->high[axis] = p;
		}
		if (stream->lightmap)
		{
			const short *texcoord = (const short *)((const char *)lightmap_vertices + 8 * index + 4);
			struct ge_vertex *l = &stream->lightmap[index];

			l->u = (texcoord[0] * 2.0f + 1.0f) * (1.0f / 65535.0f);
			l->v = (texcoord[1] * 2.0f + 1.0f) * (1.0f / 65535.0f);
			stream->radiosity[index] = *(const unsigned int *)((const char *)lightmap_vertices + 8 * index);
		}
	}
	if (stream->bake)
	{
		/* build 87: the pieces' vertices (inside the wall, so the box holds) */
		unsigned int added;

		for (added = 0; added < stream->bake->added; added++)
		{
			const float *a = stream->bake->added_vertices + 8 * added;
			struct ge_vertex *b = &stream->base[count + added];

			if (level_local)
			{
				b->x = level_snap(a[0]); b->y = level_snap(a[1]); b->z = level_snap(a[2]);
			}
			else
			{
				b->x = a[0]; b->y = a[1]; b->z = a[2];
			}
			b->u = a[3]; b->v = a[4];
			stream->normals[count + added] = ((const unsigned int *)a)[7];
			if (stream->lightmap)
			{
				struct ge_vertex *l = &stream->lightmap[count + added];

				l->u = a[5]; l->v = a[6];
			}
		}
		count += stream->bake->added;
		stream->count = count;
	}
	/* the positions given from the copy's origin: the level's, or where the
	copy lies too far from it to be exact, the lattice point nearest its
	middle */
	{
		int axis;

		stream->origin[0] = stream->origin[1] = stream->origin[2] = 0.0f;
		if (level_local && level_origin_set && level_reach(stream->low, stream->high, level_origin))
			memcpy(stream->origin, level_origin, sizeof(stream->origin));
		else if (level_local)
			for (axis = 0; axis < 3; axis++)
				stream->origin[axis] = floorf((stream->low[axis] + stream->high[axis]) * (0.5f / LEVEL_ORIGIN_STEP) + 0.5f) *
					LEVEL_ORIGIN_STEP;
		for (index = 0; index < count; index++)
		{
			struct ge_vertex *b = &stream->base[index];

			b->x -= stream->origin[0]; b->y -= stream->origin[1]; b->z -= stream->origin[2];
			if (stream->lightmap)
			{
				struct ge_vertex *l = &stream->lightmap[index];

				l->x = b->x; l->y = b->y; l->z = b->z;
			}
		}
		if (stream->bake)
			level_layers_follow(stream->bake->key, stream->origin);
	}
	if (level_compact < 0)
		level_compact = psp_keep("levelcompact");
	stream->compact_base = stream->compact_lightmap = NULL;
	if (level_compact)
		compact_build(stream);
	ge_writeback(stream->base, count * sizeof(struct ge_vertex));
	vm_level_note(stream->base, count * sizeof(struct ge_vertex));
	if (stream->lightmap)
	{
		ge_writeback(stream->lightmap, count * sizeof(struct ge_vertex));
		vm_level_note(stream->lightmap, count * sizeof(struct ge_vertex));
	}
	stream_bytes += stream_size(stream);
	return stream;
}

/* at the frame's end, after the arena's housekeeping: the materials waiting
for room (stream_room above) built where there is room now. A material not
drawn this frame stops waiting; its next draw tries as any new one does */
static void __attribute__((cold)) stream_room_frame_end(void)
{
	unsigned int room = 0;
	int k, index;

	for (k = 0; k < stream_waiting_count; k++)
		if (stream_waiting[k].wanted != ge_hot.stream_frame)
			stream_waiting[k--] = stream_waiting[--stream_waiting_count];
	if (stream_waiting_count && arena_state > 0)
	{
		/* free, and held by copies not on screen this frame (let go first) */
		room = rooms_free();
		for (index = 0; index < LEVEL_STREAMS; index++)
		{
			const struct level_stream *stream = &level_streams[index];
			struct copy_room *in = stream->base ? room_of(stream->base) : NULL;

			if (in && room_open(in) && stream->last_frame != ge_hot.stream_frame)
				room += stream_room_need(stream->count, stream->lightmap != NULL, stream->compact_base != NULL);
		}
	}
	for (k = 0; k < stream_waiting_count; k++)
	{
		const void *vertices = stream_waiting[k].vertices, *lightmap_vertices = stream_waiting[k].lightmap_vertices;
		int count = stream_waiting[k].count;
		unsigned int need = stream_room_need(count, lightmap_vertices != NULL, level_compact > 0);
		int on_arena = 0;

		if (arena_state > 0 && need <= room && (int)(ge_hot.stream_frame - stream_waiting[k].arena_after) >= 0)
		{
			unsigned int free_total[2], largest[2];
			int r;

			while (rooms_free() < need && level_stream_let_go_oldest())
				arena_let_go++;
			/* the pieces go where they fit, the arena first: unless one block
			already holds the whole copy, each open room is made whole */
			for (r = 0; r < 2; r++)
				largest[r] = room_largest(&rooms[r], &free_total[r]);
			if (largest[0] < need && largest[1] < need)
				for (r = 0; r < 2; r++)
					if (room_open(&rooms[r]))
					{
						arena_compact(&rooms[r]);
						stream_room_compactions++;
					}
			room -= need;
			on_arena = 1;
		}
		else if (!arena_squeeze && (int)(ge_hot.stream_frame - stream_waiting[k].heap_after) >= 0)
		{
			/* asked past the heap's accounting: a refusal here is a question,
			not a failure to report */
			extern void *__real_memalign(size_t alignment, size_t size);
			unsigned int bytes = (unsigned int)count * sizeof(struct ge_vertex);
			void *first = __real_memalign(ARENA_ALIGN, bytes);
			void *second = first && lightmap_vertices ? __real_memalign(ARENA_ALIGN, bytes) : NULL;
			int fits = first && (!lightmap_vertices || second);

			free(second);
			free(first);
			stream_waiting[k].heap_after = ge_hot.stream_frame + STREAM_HEAP_RETRY;
			stream_room_heap_tries++;
			if (!fits)
				continue;
		}
		else
			continue;
		stream_room_tries++;
		stream_room_building = 1;
		stream_asked_texture = stream_waiting[k].texture;
		if (level_stream_get(vertices, lightmap_vertices, count))
		{
			stream_room_built++;
			stream_waiting[k--] = stream_waiting[--stream_waiting_count];
		}
		else if (on_arena)
		{
			stream_waiting[k].arena_after = ge_hot.stream_frame + STREAM_HEAP_RETRY;
			carve_pack_short = 1;
		}
		stream_room_building = 0;
	}
	/* once a second while anything happens; every ten while a material only waits */
	if ((stream_room_waits + stream_room_built + stream_room_tries != stream_room_seen &&
		ge_hot.stream_frame - stream_room_log_frame >= 30) ||
		(stream_waiting_count && ge_hot.stream_frame - stream_room_log_frame >= 300))
	{
		platform_log("GE: level copies short of room: %lu materials have waited, %d waiting now, %lu of their draws skipped; "
			"built at a frame's end %lu of %lu tries (%lu compactions for it), the heap asked %lu times",
			stream_room_waits, stream_waiting_count, stream_room_skips, stream_room_built, stream_room_tries,
			stream_room_compactions, stream_room_heap_tries);
		carve_report();
		stream_room_seen = stream_room_waits + stream_room_built + stream_room_tries;
		stream_room_log_frame = ge_hot.stream_frame;
	}
}

int psp_tex_bind(unsigned int key, int *has_alpha);
int psp_tex_size(unsigned int key, int *width, int *height);
void psp_tex_hud_note(unsigned int key); /* stripcensus */
static unsigned long textured_draws = 0, flat_draws = 0;

/* the level's draws so far, textured and flat (not baked, or still on its
way from the Memory Stick): "transwalk" reads them across a BSP switch
(psp_fixups.py, FIXES_TRANSITIONS) */
void psp_ge_level_draw_counts(unsigned long *textured, unsigned long *flat)
{
	*textured = textured_draws;
	*flat = flat_draws;
}

/* build 58: the texture level chosen by distance ("slope" mode) instead of
the GE's automatic choice. The automatic choice (as PPSSPP models it,
GPU/Software/Rasterizer.cpp) looks only at how s changes across the screen
and t down it, so on ground seen at a slant it depends on how the texture
happens to lie: turn, and the level changes, and the ground and the bullet
holes on it shimmer and change shape (b57; holes on the walls, which
face the eye, stayed still). In slope mode the GE takes the level from each
pixel's depth: log2(2 x w x slope), w its distance. With a surface's texels
per world unit known, slope = texels per unit / (2 x pixels per unit at
distance 1) puts one texel on one pixel of a surface facing the eye, the
Xbox's choice there; at a slant it doesn't blur more as the Xbox's did, but
it no longer swims as the view turns. "mipauto": the automatic choice */
static int mip_slope = -1;

/* build 107: the tangent of half the field of view, worked out when the
field of view changes (the decals asked for it twice a call, 800 calls a
frame in b30's fight: tanf was 0.4% of the beach's time) */
static float fov_tan_half(void)
{
	static float fov = -1.0f, value = 1.0f;

	if (projection_fov != fov)
	{
		fov = projection_fov;
		value = tanf(projection_fov * 0.5f);
	}
	return value;
}

static float pixels_per_unit(void)
{
	return (SCREEN_HEIGHT * 0.5f) / fov_tan_half();
}

#ifdef PSP_DEBUG_TOOLS
/* "sdump" (debug tools): on a screenshot's frame, every level, model and
decal draw whose screen box meets the box of the keep words sboxlN, sboxtN,
sboxrN, sboxbN (pixels; the whole screen when left out), with its map, its
alpha test and whether the map has alpha, and for the level its shader. tdump
lists only the see-through draws; this names what draws anything else that
is on screen. Name lookups walk the tags, so not on the Media Engine */
extern volatile int psp_screenshot_requested;
static int sdump = -1, sdump_limits[4];
static void const *sdump_shader = NULL;

/* the environment shader of the level draw that follows (psp_fixups.py) */
void psp_ge_sdump_shader(void const *shader)
{
	sdump_shader = shader;
}

static int sdump_wanted(void)
{
	if (sdump < 0)
	{
		extern int psp_keep_number(const char *prefix, int fallback);

		sdump = psp_keep("sdump") != 0;
		sdump_limits[0] = psp_keep_number("sboxl", 0);
		sdump_limits[1] = psp_keep_number("sboxt", 0);
		sdump_limits[2] = psp_keep_number("sboxr", SCREEN_WIDTH);
		sdump_limits[3] = psp_keep_number("sboxb", SCREEN_HEIGHT);
	}
	if (sdump && psp_screenshot_requested && !PSP_ME_WORK())
	{
		static int listed = 0;

		/* once a run: a run plays one map */
		if (!listed)
		{
			extern void psp_sdump_shared_bases(void); /* hud_draw.c (psp_fixups.py) */

			listed = 1;
			psp_sdump_shared_bases();
		}
		return 1;
	}
	return 0;
}

/* box: left, top, right, bottom in pixels, from camera-space points. A point
behind the near plane makes the box the whole screen: the draw passes the
eye, and its box can't be worked out from its corners */
static void sdump_box_start(float *box)
{
	box[0] = box[1] = 1e30f;
	box[2] = box[3] = -1e30f;
}

static void sdump_box_add(float *box, const float *c)
{
	float scale = pixels_per_unit(), x, y;

	if (c[2] < NEAR_CLIP)
	{
		box[0] = box[1] = -1e29f;
		box[2] = box[3] = 1e29f;
		return;
	}
	x = SCREEN_WIDTH * 0.5f + c[0] / c[2] * scale;
	y = SCREEN_HEIGHT * 0.5f - c[1] / c[2] * scale;
	if (x < box[0]) box[0] = x;
	if (y < box[1]) box[1] = y;
	if (x > box[2]) box[2] = x;
	if (y > box[3]) box[3] = y;
}

/* 1 when the box meets the screen and the asked-for box; text: the box */
static int sdump_box_text(const float *box, char *text, int size)
{
	int l = box[0] < -9999.0f ? -9999 : (int)box[0], t = box[1] < -9999.0f ? -9999 : (int)box[1];
	int r = box[2] > 9999.0f ? 9999 : (int)box[2], b = box[3] > 9999.0f ? 9999 : (int)box[3];

	if (r < 0 || b < 0 || l >= SCREEN_WIDTH || t >= SCREEN_HEIGHT)
		return 0;
	if (r < sdump_limits[0] || b < sdump_limits[1] || l >= sdump_limits[2] || t >= sdump_limits[3])
		return 0;
	snprintf(text, size, "box %d %d %d %d", l, t, r, b);
	return 1;
}

static char const *sdump_map_name(unsigned int key)
{
	extern char const *psp_bitmap_tag_name(void const *bitmap); /* hud_draw.c (psp_fixups.py) */
	char const *name = key ? psp_bitmap_tag_name((void const *)(key & ~3u)) : NULL;

	return name ? name : "?";
}
#endif

/* texture coordinate units per world unit over a draw's triangles */
static float uv_density(const struct ge_vertex *stream, const unsigned short *indices, long triangle_count)
{
	/* build 102: in single precision (doubles are software on the PSP) */
	float uv_area = 0.0f, world_area = 0.0f;
	long t;

	for (t = 0; t < triangle_count && t < 512; t++)
	{
		const struct ge_vertex *a = &stream[indices[3 * t]], *b = &stream[indices[3 * t + 1]], *c = &stream[indices[3 * t + 2]];
		float e1[3] = { b->x - a->x, b->y - a->y, b->z - a->z }, e2[3] = { c->x - a->x, c->y - a->y, c->z - a->z };
		float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };

		world_area += sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		uv_area += fabsf((b->u - a->u) * (c->v - a->v) - (c->u - a->u) * (b->v - a->v));
	}
	return world_area > 0.0f ? sqrtf(uv_area / world_area) : 0.0f;
}

/* the GE's level mode for a texture of size width x height drawn at
uv_per_unit (times scale) */
static float texture_levels_by_distance(float uv_per_unit, int width, int height, float scale, float bias)
{
	float slope;

	if (mip_slope < 0)
		mip_slope = !psp_keep("mipauto");
	if (!mip_slope || uv_per_unit <= 0.0f || width <= 0 || height <= 0)
	{
		sceGuTexLevelMode(tex_level_mode, bias);
		return 0.0f;
	}
	slope = uv_per_unit * scale * sqrtf((float)width * (float)height) / (2.0f * pixels_per_unit());
	sceGuTexSlope(slope);
	sceGuTexLevelMode(tex_level_mode == GU_TEXTURE_CONST ? GU_TEXTURE_CONST : GU_TEXTURE_SLOPE, bias);
	return tex_level_mode == GU_TEXTURE_CONST ? 0.0f : slope;
}

/* build 64: the slant put back into the level choice. In slope mode the GE
takes the level from each pixel's depth alone, as for a surface facing the
eye; one seen at a slant is squeezed on the screen by 1 / cos(angle) across
the slant, and the Xbox's choice (by how fast the texture changes across
the screen, in either direction) is that much blurrier there. b63's run
B: by distance alone, crisp floors but the ground swam outside and the
cliffs' feet looked aliased. The GE's slope is one value a draw, so each
level draw's triangles are sorted into buckets by their slant (half a level
apart, up to three levels: 1 / cos up to 8), and each bucket is drawn with
its own slope. "noangle": by distance alone (b63 run B) */
#define ANGLE_BUCKETS 7
/* build 66: each slant bucket twice, near and far: a triangle whose nearest
corner is past DETAIL_FAR is drawn in the base and lightmap passes only. The
detail passes fade to their neutral grey by DETAIL_FAR (the fog), so they
change nothing there; skipping them saves the GE the fill and the CPU the
draws. "detailallfar": the detail passes everywhere, as before */
#define SORT_BUCKETS (2 * ANGLE_BUCKETS)
#define ANGLE_TRIANGLES 4096
static int angle_buckets = -1, detail_all_far = -1;
static int angle_bucketed = 0, angle_near_any = 0;
static long angle_first[SORT_BUCKETS], angle_count[SORT_BUCKETS];
static unsigned char angle_bucket_of[ANGLE_TRIANGLES];
static const float angle_factor[ANGLE_BUCKETS] = { 1.0f, 1.41421356f, 2.0f, 2.82842712f, 4.0f, 5.65685425f, 8.0f };
static unsigned long angle_draws = 0, angle_triangles[ANGLE_BUCKETS], angle_us = 0, angle_far_triangles = 0,
	detail_passes_skipped_far = 0, level_draws_inside = 0, level_draws_classified = 0;
static float detail_far_squared = 0.0f;

/* build 87: one triangle's slant bucket (plus ANGLE_BUCKETS when wholly
past the detail fade). The main CPU's loop below and the Media Engine's
measure job (psp_ge_me_angle) both use this, so the two compute the very same
expressions in the same order (no fused multiply-adds: -ffp-contract=off) and
must agree bit for bit. No library calls: the ME runs it */
static inline int angle_bucket_one(const struct ge_vertex *stream, const unsigned short *triangle, float ex, float ey,
	float ez, int far_possible, float far_squared)
{
	const struct ge_vertex *a = &stream[triangle[0]], *p = &stream[triangle[1]], *q = &stream[triangle[2]];
	float e1[3] = { p->x - a->x, p->y - a->y, p->z - a->z }, e2[3] = { q->x - a->x, q->y - a->y, q->z - a->z };
	float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
	float da[3] = { a->x - ex, a->y - ey, a->z - ez };
	float d[3] = { da[0] + e1[0] * (1.0f / 3.0f) + e2[0] * (1.0f / 3.0f), da[1] + e1[1] * (1.0f / 3.0f) + e2[1] * (1.0f / 3.0f),
		da[2] + e1[2] * (1.0f / 3.0f) + e2[2] * (1.0f / 3.0f) };
	float dot = n[0] * d[0] + n[1] * d[1] + n[2] * d[2];
	float lengths = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) * (d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
	int bucket = 0;

	/* 1 / cos^2 against the bucket edges, 2^(b - 1/2) */
	if (lengths > 0.0f)
	{
		float ratio_limit = dot * dot;

		while (bucket < ANGLE_BUCKETS - 1 && lengths > ratio_limit * angle_factor[bucket] * angle_factor[bucket] * 1.41421356f)
			bucket++;
	}
	if (far_possible)
	{
		/* far: every corner past DETAIL_FAR (the nearest corner decides) */
		float sa = da[0] * da[0] + da[1] * da[1] + da[2] * da[2];

		if (sa > far_squared)
		{
			float dp[3] = { p->x - ex, p->y - ey, p->z - ez };
			float sp = dp[0] * dp[0] + dp[1] * dp[1] + dp[2] * dp[2];

			if (sp > far_squared)
			{
				float dq[3] = { q->x - ex, q->y - ey, q->z - ez };

				if (dq[0] * dq[0] + dq[1] * dq[1] + dq[2] * dq[2] > far_squared)
					bucket += ANGLE_BUCKETS;
			}
		}
	}
	return bucket;
}

/* build 87, the like-for-like: the same function on the main CPU over the
same range, its cache emptied of the range first as the ME's is, timed, and
its answers against the ME's */
static unsigned long me_classify_sc_us = 0, me_classify_sc_mismatches = 0;
static unsigned long me_classify_me_us = 0;
/* build 87: the second measure job, the slant buckets (angle_sort's loop:
three vertices fetched per triangle from wherever they lie, the shape of the
renderer's most expensive CPU step on hardware, 3.8 ms a frame in b86) */
#if PSP_DEBUG_TOOLS
static unsigned char me_buckets[ANGLE_TRIANGLES + 64] __attribute__((aligned(64)));
static float me_angle_view[16] __attribute__((aligned(64)));
#endif
static unsigned long me_angle_jobs = 0, me_angle_triangles = 0, me_angle_mismatches = 0, me_angle_failures = 0,
	me_angle_wait_us = 0, me_angle_sc_us = 0;
static unsigned long me_angle_me_us = 0;
static void me_angle_measure(const struct ge_vertex *stream, const unsigned short *indices, long count,
	int far_possible);
static unsigned long angle_loop_us = 0;

/* The slant split (ME phase 1; on by default, "meslantoff"): the back part of a draw's bucket loop goes to the
Media Engine through the job ring while this CPU works the front part, so
the two halves run at once. Both call angle_bucket_one, so the buckets are
the same bits (b90 on the PSP: 0 differing over 4.7 million triangles); the
display list comes out byte for byte the same. A draw is split only from
SLANT_SPLIT_MIN triangles, where the work outweighs a job's hand-over, and
the ME's part is smaller by SLANT_BIAS triangles, since it starts later and
drops its cache first ("meslantminN", "meslantbiasN" set them, to tune on
the PSP). "meslantcheck": this CPU works the ME's part out again, counts
what differs and uses its own. Only where this CPU draws the level (with
gemeon the whole level draw is already the ME's) */
#define SLANT_SPLIT_MIN 96
#define SLANT_BIAS 32
static PSP_MAIN_SIDE int me_slant = -1, me_slant_check = 0, me_slant_min = SLANT_SPLIT_MIN, me_slant_bias = SLANT_BIAS;
static PSP_MAIN_SIDE unsigned long me_slant_draws = 0, me_slant_split = 0, me_slant_triangles = 0, me_slant_mismatches = 0,
	me_slant_fallbacks = 0, me_slant_checked = 0;
/* the ME's part's buckets: written by the ME's job only, read here only
after the wait (whole lines of their own). The ME's part is at most half a
draw */
static unsigned char me_slant_out[ANGLE_TRIANGLES / 2] __attribute__((aligned(64)));

/* the words, read once. Cold, like the other once-only and rare paths
below: the hand-over's code, each draw's, stays fewer cache lines */
static __attribute__((noinline, cold)) void slant_decide(void)
{
	extern int psp_keep_number(const char *prefix, int fallback);

	int psp_me_slant_wanted(void);

	me_slant = psp_me_slant_wanted();
	me_slant_check = psp_keep("meslantcheck");
	me_slant_min = psp_keep_number("meslantmin", SLANT_SPLIT_MIN);
	me_slant_bias = psp_keep_number("meslantbias", SLANT_BIAS);
}

/* the ME's part handed over: returns where this CPU's part ends (count: no
job, this CPU does the whole draw). lowest and highest: the draw's vertex
range. The stream's vertices are in main RAM already (written back when
built or moved, for the GE); the indices may be this frame's list copy */
static __attribute__((noinline)) long slant_hand_over(const struct ge_vertex *stream, const unsigned short *indices, long count, int far_possible,
	unsigned int lowest, unsigned int highest, unsigned int *ticket)
{
	struct psp_me_job job;
	long part, first;
	int k;
	union { float f; unsigned int u; } bits;

	if (PSP_ME_WORK())
		return count;
	if (me_slant < 0)
		slant_decide();
	if (!me_slant)
		return count;
	me_slant_draws++;
	part = (count - me_slant_bias) / 2;
	if (count < me_slant_min || part <= 0 || part > (long)sizeof(me_slant_out) || !psp_me_ring_ready())
		return count;
	first = count - part;
	memset(&job, 0, sizeof(job));
	job.type = ME_JOB_SLANT;
	job.args[0] = (unsigned int)stream;
	job.args[1] = (unsigned int)(indices + 3 * first);
	job.args[2] = (unsigned int)part;
	job.args[3] = (unsigned int)me_slant_out;
	job.args[4] = (unsigned int)far_possible;
	for (k = 0; k < 3; k++)
	{
		bits.f = level_eye[k];
		job.args[5 + k] = bits.u;
	}
	bits.f = detail_far_squared;
	job.args[8] = bits.u;
	job.input[0] = indices + 3 * first;
	job.input_bytes[0] = (unsigned int)part * 3 * sizeof(unsigned short);
	job.input[1] = &stream[lowest];
	job.input_bytes[1] = (highest - lowest + 1) * sizeof(struct ge_vertex);
	job.output = me_slant_out;
	job.output_bytes = ((unsigned int)part + 63) & ~63u;
	psp_me_writeback(job.input[0], job.input_bytes[0]);
	if (!psp_me_push(&job, ticket))
		return count;
	return first;
}

/* the ME's part worked out on this CPU: after a failed wait (done 0), or
with meslantcheck, where the ME's answer (done 1, results) is compared and
this CPU's is used */
static __attribute__((noinline, cold)) void slant_part_here(int done, const unsigned int *results,
	const struct ge_vertex *stream, const unsigned short *indices, long first, long count, int far_possible, long *total)
{
	float ex = level_eye[0], ey = level_eye[1], ez = level_eye[2];
	unsigned long differ = 0;
	long t, mine[SORT_BUCKETS];
	int b;

	memset(mine, 0, sizeof(mine));
	for (t = first; t < count; t++)
	{
		int bucket = angle_bucket_one(stream, indices + 3 * t, ex, ey, ez, far_possible, detail_far_squared);

		if (done && me_slant_out[t - first] != bucket)
			differ++;
		angle_bucket_of[t] = (unsigned char)bucket;
		mine[bucket]++;
	}
	for (b = 0; b < SORT_BUCKETS; b++)
	{
		if (done && (long)((results[b >> 1] >> ((b & 1) * 16)) & 0xFFFFu) != mine[b])
			differ++;
		total[b] += mine[b];
	}
	if (done)
	{
		me_slant_split++;
		me_slant_triangles += (unsigned long)(count - first);
		me_slant_checked += (unsigned long)(count - first);
		me_slant_mismatches += differ;
	}
	else
		me_slant_fallbacks++;
}

/* the ME's part collected into angle_bucket_of and the totals */
static __attribute__((noinline)) void slant_collect(unsigned int ticket, const struct ge_vertex *stream, const unsigned short *indices, long first,
	long count, int far_possible, long *total)
{
	unsigned int results[ME_RING_RESULTS];
	long part = count - first;
	int b, done = psp_me_wait(ticket, results);

	if (!done || me_slant_check)
	{
		slant_part_here(done, results, stream, indices, first, count, far_possible, total);
		return;
	}
	me_slant_split++;
	me_slant_triangles += (unsigned long)part;
	memcpy(angle_bucket_of + first, me_slant_out, (size_t)part);
	for (b = 0; b < SORT_BUCKETS; b++)
		total[b] += (long)((results[b >> 1] >> ((b & 1) * 16)) & 0xFFFFu);
}

/* sorts a draw's triangles by slant; returns the list to draw (the given
one, or a copy in the display list sorted by bucket). lowest and highest:
the draw's vertex range */
static const unsigned short *angle_sort(const struct ge_vertex *stream, const unsigned short *indices, long count,
	int far_possible, unsigned int lowest, unsigned int highest)
{
	long t, total[SORT_BUCKETS];
	int b, used = 0;
	unsigned short *sorted;
	float ex = level_eye[0], ey = level_eye[1], ez = level_eye[2];

	angle_bucketed = 0;
	angle_near_any = 1;
	if (angle_buckets < 0)
	{
		angle_buckets = !psp_keep("noangle");
		detail_all_far = psp_keep("detailallfar");
	}
	if (mip_slope < 0)
		mip_slope = !psp_keep("mipauto");
	if (!angle_buckets || !mip_slope || tex_level_mode == GU_TEXTURE_CONST || count <= 0 || count > ANGLE_TRIANGLES)
		return indices;
	if (detail_all_far)
		far_possible = 0;
	memset(total, 0, sizeof(total));
	{
		unsigned int loop_start = ge_clock(), ticket = 0;
		/* off: one test a draw (me_slant is -1 until the first draw decides) */
		long mine = me_slant ? slant_hand_over(stream, indices, count, far_possible, lowest, highest, &ticket) : count;

		for (t = 0; t < mine; t++)
		{
			int bucket = angle_bucket_one(stream, indices + 3 * t, ex, ey, ez, far_possible, detail_far_squared);

			angle_bucket_of[t] = (unsigned char)bucket;
			total[bucket]++;
		}
		if (mine < count)
			slant_collect(ticket, stream, indices, mine, count, far_possible, total);
		angle_loop_us += ge_clock() - loop_start;
	}
	/* build 87: the ME works out the same buckets ("meclassify", measure only) */
	me_angle_measure(stream, indices, count, far_possible);
	for (b = 0; b < SORT_BUCKETS; b++)
		if (total[b])
			used++;
	sorted = used > 1 ? list_take((int)(count * 3 * sizeof(unsigned short) + 3) & ~3) : NULL;
	if (used > 1 && !sorted)
		return indices;
	{
		long first = 0;

		angle_near_any = 0;
		for (b = 0; b < SORT_BUCKETS; b++)
		{
			angle_first[b] = first;
			angle_count[b] = total[b];
			angle_triangles[b % ANGLE_BUCKETS] += total[b];
			if (b >= ANGLE_BUCKETS)
				angle_far_triangles += total[b];
			else if (total[b])
				angle_near_any = 1;
			first += total[b];
		}
	}
	angle_bucketed = 1;
	angle_draws++;
	if (!sorted)
	{
		/* one bucket: the list as it is, its first triangle at 0 */
		for (b = 0; b < SORT_BUCKETS; b++)
			angle_first[b] = 0;
		return indices;
	}
	{
		long place[SORT_BUCKETS];

		memcpy(place, angle_first, sizeof(place));
		for (t = 0; t < count; t++)
		{
			long at = place[angle_bucket_of[t]]++;

			sorted[3 * at] = indices[3 * t];
			sorted[3 * at + 1] = indices[3 * t + 1];
			sorted[3 * at + 2] = indices[3 * t + 2];
		}
	}
	return sorted;
}

/* one pass over the kept triangles (a list of indices into the stream) and
the cut-up ones (camera-space corners, drawn with no view matrix) */
/* build 66: a draw's cut corners as GE vertices, made once a draw (they were
made again in each of the four passes) */
static struct ge_vertex *cut_vertices_prepared[2];
static int cut_vertices_count = 0;

static void draw_pass(const struct ge_vertex *stream, const unsigned short *indices, int index_count,
	struct cut_store *cut, int cut_count, int lightmap)
{
	if (index_count > 0 && compact_now && (lightmap ? compact_now->compact_lightmap : compact_now->compact_base))
	{
		/* build 87: the 16-bit copies, through the material's own matrix */
		compact_begin(lightmap);
		sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_3D,
			index_count, indices, lightmap ? compact_now->compact_lightmap : compact_now->compact_base);
		compact_end();
	}
	else if (index_count > 0)
		sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
			index_count, indices, stream);
	if (cut_count > 0 && cut_count == cut_vertices_count && cut_vertices_prepared[lightmap ? 1 : 0])
	{
		sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D, cut_count, NULL,
			cut_vertices_prepared[lightmap ? 1 : 0]);
		return;
	}
	if (cut_count > 0)
	{
		struct ge_vertex *corners = list_take(cut_count * sizeof(struct ge_vertex));
		int index;

		if (!corners)
			return;

		/* in the world, through the same matrices as the uncut triangles
		(build 34): drawn from camera space, a cut triangle's corners came out
		a hair off its uncut neighbours' on the PSP, and their shared edges
		opened into see-through seams near the camera. Corners the cut kept
		are the very same floats as the neighbours'; the new ones lie on the
		near or guard planes, well inside what the GE can draw */
		for (index = 0; index < cut_count; index++)
		{
			const float *c = cut_at(cut, index);

			corners[index].u = c[lightmap ? 5 : 3];
			corners[index].v = c[lightmap ? 6 : 4];
			corners[index].x = c[7];
			corners[index].y = c[8];
			corners[index].z = c[9];
		}
		sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_3D, cut_count, NULL, corners);
	}
}

/* a pass of a level draw sorted by angle_sort: each bucket with the slope
times its factor, the cut-up corners (at the eye, level 0 regardless) with
the slope as it is */
static void draw_pass_angled(const struct ge_vertex *stream, const unsigned short *indices, int index_count,
	struct cut_store *cut, int cut_count, int lightmap, float slope, int near_only)
{
	int b, buckets = near_only ? ANGLE_BUCKETS : SORT_BUCKETS;

	if (!angle_bucketed || slope <= 0.0f || index_count <= 0)
	{
		draw_pass(stream, indices, index_count, cut, cut_count, lightmap);
		return;
	}
	if (compact_now && compact_now->compact_base && !lightmap)
		compact_begin(0);
	for (b = 0; b < buckets; b++)
	{
		if (!angle_count[b])
			continue;
		sceGuTexSlope(slope * angle_factor[b % ANGLE_BUCKETS]);
		if (compact_now && compact_now->compact_base && !lightmap)
			sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_16BIT | GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_3D,
				(int)angle_count[b] * 3, indices + 3 * angle_first[b], compact_now->compact_base);
		else
			sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
				(int)angle_count[b] * 3, indices + 3 * angle_first[b], stream);
	}
	if (compact_now && compact_now->compact_base && !lightmap)
		compact_end();
	sceGuTexSlope(slope);
	if (cut_count > 0)
		draw_pass(stream, NULL, 0, cut, cut_count, lightmap);
}


/* one material's visible triangles: vertices are the game's 32-byte level
vertices, lightmap_vertices its 8-byte lightmap ones (or NULL), indices its
16-bit triangle list. base_key and lightmap_key name the textures (0: none).

Each vertex is classified once (build 19): its outside-the-planes bits go
into vertex_codes the first time a triangle uses it, so a vertex shared by
six triangles is fetched and transformed once instead of six times. Only
the corners of triangles that need cutting up are transformed again */
/* the shader's detail maps (build 31): the Xbox combines the base map with
a tiled detail map and a tiled micro detail map, each doubled and multiplied
in. The GE has one texture at a time, so each is a pass of its own, like the
lightmap: the same triangles, the base map's coordinates scaled by the
shader's tiling, blended as 2 x src x dst where pass 1 drew. texpack.py bakes
the maps with an average of 0.5 and folds the real averages into the base
map, so a pass that is skipped, mipped down or faded out changes nothing; it
fades to that neutral grey from DETAIL_NEAR to DETAIL_FAR (or the fog's
far end), where the maps would only shimmer at 272 lines */
/* build 57: 8 to 24 units (was 20 to 60): far off, the detail maps' pattern
only beat against the pixels (the canyon bands; the walls looked better
without them, b54 shots 24-25), and at 272 lines it can't be seen there
anyway. "detailfar": the old range */
/* build 64: 20 to 60 by default (b63's runs A and B: no bands with the
Xbox's faded levels, pack 59). "detailnear": 8 to 24 (b57-b63) */
#define DETAIL_NEAR (detail_far ? 20.0f : 8.0f)
#define DETAIL_FAR (detail_far ? 60.0f : 24.0f)
static int detail_far = -1;
static unsigned int detail_key[2];
static float detail_u[2], detail_v[2];
static unsigned long detail_passes = 0;
static int detail_off = -1;
static float detail_bias = -2.0f;
static int detail_slope = -1;
int psp_keep(const char *name);

/* build 116 (b113 #7): the grey a detail pass fades to, its texels' own
neutral: texpack.py bakes a detail map around 0.5, which a 16-bit (5650)
palette holds as 0x7B7D7B. Builds 31-115 faded to 0x808080, about 4%
brighter a pass, so the terrain and the walls brightened in a band from 20
to 60 units (b113 #7). A 32-bit palette holds 0x808080 itself. Keep word
"detailgrey80": as before */
static unsigned int detail_neutral(unsigned int key)
{
	extern int psp_tex_palette16(unsigned int key, const unsigned short **palette, int *count);
	static int old = -1;
	const unsigned short *palette;
	int count;

	if (old < 0)
		old = psp_keep("detailgrey80");
	return !old && psp_tex_palette16(key, &palette, &count) ? 0xFF7B7D7B : 0xFF808080;
}

void psp_ge_level_detail(unsigned int primary_key, float primary_u, float primary_v,
	unsigned int micro_key, float micro_u, float micro_v)
{
	GE_PRETOUCH_IF_RESIDENT(primary_key, TXPT_LEVEL_DETAIL);
	GE_PRETOUCH_IF_RESIDENT(micro_key, TXPT_LEVEL_DETAIL);
	GE_DEFER(defer_words(DEFER_LEVEL_DETAIL, 6, primary_key, defer_bits(primary_u), defer_bits(primary_v), micro_key,
		defer_bits(micro_u), defer_bits(micro_v), 0, 0)); /* build 112: gedefer */
	detail_key[0] = primary_key; detail_u[0] = primary_u; detail_v[0] = primary_v;
	detail_key[1] = micro_key; detail_u[1] = micro_u; detail_v[1] = micro_v;
}

/* a level shader's self-illumination (psp_fixups.py works out the two colours
its illumination map's red and green weigh, as the Xbox does; texpack.py,
level_glow_looks, bakes a glow map for each under the shader's address | 2 and
| 3). The Xbox adds the colour to the lightmap and the base then multiplies
the frame, so a lit-up area shows its own texture at full brightness: after
the base, lightmap and detail passes the same triangles are drawn again with
the glow map (the base x one channel of the illumination map) times the colour,
added. Added rather than screened: the lit-up part of a level is dark around
it, where the Xbox's clamp at 1 is out of reach. A shader with no base map is
drawn flat, and takes its glow map's own texels. "nolevelglow": left out */
static unsigned int level_glow_key[2], level_glow_color[2];
static unsigned long level_glow_passes = 0;
static int level_glow_off = -1;

static void level_glow_set(unsigned int primary_key, unsigned int primary_color, unsigned int secondary_key,
	unsigned int secondary_color)
{
	level_glow_key[0] = primary_key;
	level_glow_color[0] = primary_color;
	level_glow_key[1] = secondary_key;
	level_glow_color[1] = secondary_color;
}

void psp_ge_level_glow(unsigned int primary_key, float const *primary, unsigned int secondary_key,
	float const *secondary)
{
	unsigned int primary_color = abgr_from_rgb(primary) & 0xFFFFFF;
	unsigned int secondary_color = abgr_from_rgb(secondary) & 0xFFFFFF;

	/* a map the replay will bind */
	if (primary_color && psp_tex_has(primary_key))
		GE_PRETOUCH(primary_key, TXPT_GLOW);
	if (secondary_color && psp_tex_has(secondary_key))
		GE_PRETOUCH(secondary_key, TXPT_GLOW);
	GE_DEFER(defer_words(DEFER_LEVEL_GLOW, 4, primary_key, primary_color, secondary_key, secondary_color, 0, 0, 0, 0));
	level_glow_set(primary_key, primary_color, secondary_key, secondary_color);
}

#define CODE_UNKNOWN 0x80
static unsigned char vertex_codes[65536];
/* build 119's probe: the level's passes on the PSP, by piece (the slant
sort has its own timer, angle_us) */
static unsigned long level_uv_us = 0, level_base_us = 0, level_lightmap_us = 0, level_detail_us = 0, level_probe_draws = 0;
static int box_test = -1;
static unsigned long budget_renderer_us = 0, budget_wait_us = 0;
/* build 87: the same over the heartbeat's own window (benchrun's results
need the window they name; the 300-frame figures lag it) */
static PSP_MAIN_SIDE unsigned long hb_renderer_us = 0, hb_wait_us = 0, hb_frames = 0, entry_snapshot = 0;

/* build 64: the renderer's CPU time and the wait for the GE, us a frame, as
last logged (the heartbeat's budget line) */
void psp_ge_budget(unsigned long *renderer_us, unsigned long *wait_us)
{
	if (hb_frames)
	{
		*renderer_us = hb_renderer_us / hb_frames;
		*wait_us = hb_wait_us / hb_frames;
		hb_renderer_us = hb_wait_us = hb_frames = 0;
		return;
	}
	*renderer_us = budget_renderer_us;
	*wait_us = budget_wait_us;
}
static float cut_corners[MAXIMUM_CUT_CORNERS][CORNER_FLOATS];

static int level_cull = -1;

static void me_classify_measure(const struct level_stream *stream, unsigned int lowest, unsigned int highest);
static unsigned long me_classify_jobs, me_classify_vertices, me_classify_mismatches, me_classify_failures,
	me_classify_wait_us;

/* build 90: this frame's level draws as the base pass drew them (stream,
triangle list, cut-up corners), for the permanent decals (psp_ge_decal),
which are drawn afterwards on these very triangles with depth EQUAL: the
stencil bake's method, without a bake (the game builds these decals as the
map loads). b87's run 2: the scenario's painted symbols on the base
blocks partly sank into them on the PSP (whole in the emulator) */
#define FRAME_LEVEL_DRAWS 96
static struct frame_level_draw
{
	const struct level_stream *stream;
	const unsigned short *indices;
	int kept;
	const struct ge_vertex *cut;
	int cut_count;
} frame_level[FRAME_LEVEL_DRAWS];
static int frame_level_count = 0;
static unsigned int frame_level_stamp = 0;

static void frame_level_record(const struct level_stream *stream, const unsigned short *indices, int kept,
	const struct ge_vertex *cut, int cut_count)
{
	if (frame_level_stamp != ge_hot.stream_frame)
	{
		frame_level_stamp = ge_hot.stream_frame;
		frame_level_count = 0;
	}
	if (frame_level_count >= FRAME_LEVEL_DRAWS)
		return;
	frame_level[frame_level_count].stream = stream;
	frame_level[frame_level_count].indices = indices;
	frame_level[frame_level_count].kept = kept;
	frame_level[frame_level_count].cut = cut;
	frame_level[frame_level_count].cut_count = cut ? cut_count : 0;
	frame_level_count++;
}

/* the planar fog pass over one level draw (psp_ge_fog_submerged): the
planar colour, its alpha the density at each vertex, blended over what the
draw's passes left where they drew (depth equal), in the positions those
passes used (the 16-bit copy or the floats) so the depths agree. The pieces
cut at the near or guard planes are fogged too, in the floats draw_pass drew
them with: a large seabed triangle starting under the eye and running out
(b30's flight, looking out of the Pelican's back) was cut, its pieces left
unfogged, and the bare seabed showed with a hard edge against its fogged
neighbours (B21). "submergedcutold": the pieces left out, as before.
Counted with "oceanfogold" too, so both sides of an A/B say what the pass
would cost */
static int submerged_cut_old = -1;
static unsigned long submerged_cut_vertices = 0;
static void submerged_pass(const struct level_stream *stream, const unsigned short *draw, long kept_count,
	unsigned int lowest, unsigned int highest)
{
	const float *n = submerged_plane;
	float lowest_height = -n[3], d, eye_height;
	unsigned int start, i, count;
	int axis, compact = compact_now != NULL;

	if (submerged_cut_old < 0)
		submerged_cut_old = psp_keep("submergedcutold") != 0;
	if (kept_count <= 0 && (submerged_cut_old || cut_vertices_count <= 0))
		return;
	/* the box's corner farthest below the plane (world positions) */
	for (axis = 0; axis < 3; axis++)
		lowest_height += n[axis] * (n[axis] > 0.0f ? stream->low[axis] : stream->high[axis]);
	if (lowest_height >= 0.0f)
		return;
	/* the plane in the stream's own frame; the eye in fog is psp_ge_fog_planar's */
	d = n[3] - (n[0] * stream->origin[0] + n[1] * stream->origin[1] + n[2] * stream->origin[2]);
	eye_height = n[0] * level_eye[0] + n[1] * level_eye[1] + n[2] * level_eye[2] - d;
	if (eye_height < 0.0f)
		return;
	start = ge_clock();
	/* a draw whose triangles were all cut has no kept range */
	count = kept_count > 0 ? highest - lowest + 1 : 0;
	submerged_draws++;
	submerged_triangles += kept_count;
	submerged_vertices += count;
	if (!submerged_old)
	{
		struct submerged_float { unsigned int color; float x, y, z; } *floats = NULL;
		struct submerged_short { unsigned int color; short x, y, z, pad; } *shorts = NULL;
		void *taken = count ? list_take(count * (compact ? sizeof(*shorts) : sizeof(*floats))) : NULL;

		if (count && !taken)
		{
			submerged_dropped++;
			submerged_us += ge_clock() - start;
			return;
		}
		if (compact)
			shorts = taken;
		else
			floats = taken;
		for (i = 0; i < count; i++)
		{
			const struct ge_vertex *v = &stream->base[lowest + i];
			float depth = d - (n[0] * v->x + n[1] * v->y + n[2] * v->z), density = 0.0f;
			unsigned int alpha = 0;

			if (depth > 0.0f)
			{
				float dx = v->x - level_eye[0], dy = v->y - level_eye[1], dz = v->z - level_eye[2];
				float distance = sqrtf(dx * dx + dy * dy + dz * dz), a, b;

				if (submerged_segment)
					distance *= depth / (depth + eye_height);
				a = depth * submerged_inverse_depth;
				b = distance * submerged_inverse_distance;
				density = submerged_density * (a < 1.0f ? a : 1.0f) * (b < 1.0f ? b : 1.0f);
				alpha = (unsigned int)(density * 255.0f + 0.5f);
			}
			if (compact)
			{
				const struct compact_vertex *c = &compact_now->compact_base[lowest + i];

				shorts[i].color = (alpha << 24) | submerged_color;
				shorts[i].x = c->x;
				shorts[i].y = c->y;
				shorts[i].z = c->z;
				shorts[i].pad = 0;
			}
			else
			{
				floats[i].color = (alpha << 24) | submerged_color;
				floats[i].x = v->x;
				floats[i].y = v->y;
				floats[i].z = v->z;
			}
		}
		sceGuDepthFunc(GU_EQUAL);
		sceGuDepthMask(GU_TRUE);
		sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
		ge_blend(1);
		if (kept_count > 0 && compact)
		{
			compact_begin(0);
			sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_INDEX_16BIT | GU_TRANSFORM_3D,
				(int)kept_count * 3, draw, shorts - lowest);
			compact_end();
		}
		else if (kept_count > 0)
			sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
				(int)kept_count * 3, draw, floats - lowest);
		/* the cut pieces, fogged the same way, at the floats draw_pass drew them at */
		if (!submerged_cut_old && cut_vertices_count > 0 && cut_vertices_prepared[0])
		{
			struct submerged_float *pieces = list_take(cut_vertices_count * (int)sizeof(*pieces));
			int k;

			if (pieces)
			{
				for (k = 0; k < cut_vertices_count; k++)
				{
					const struct ge_vertex *v = &cut_vertices_prepared[0][k];
					float depth = d - (n[0] * v->x + n[1] * v->y + n[2] * v->z), density;
					unsigned int alpha = 0;

					if (depth > 0.0f)
					{
						float dx = v->x - level_eye[0], dy = v->y - level_eye[1], dz = v->z - level_eye[2];
						float distance = sqrtf(dx * dx + dy * dy + dz * dz), a, b;

						if (submerged_segment)
							distance *= depth / (depth + eye_height);
						a = depth * submerged_inverse_depth;
						b = distance * submerged_inverse_distance;
						density = submerged_density * (a < 1.0f ? a : 1.0f) * (b < 1.0f ? b : 1.0f);
						alpha = (unsigned int)(density * 255.0f + 0.5f);
					}
					pieces[k].color = (alpha << 24) | submerged_color;
					pieces[k].x = v->x;
					pieces[k].y = v->y;
					pieces[k].z = v->z;
				}
				sceGumDrawArray(GU_TRIANGLES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, cut_vertices_count, NULL, pieces);
				submerged_cut_vertices += (unsigned long)cut_vertices_count;
			}
			else
				submerged_dropped++;
		}
		ge_blend(0);
		sceGuDepthMask(GU_FALSE);
		sceGuDepthFunc(GU_GEQUAL);
	}
	submerged_us += ge_clock() - start;
}

static void level_glow_pass(const struct level_stream *stream, const unsigned short *draw, long kept_count,
	struct cut_store *pieces, int cut_count, float slope, int textured)
{
	int which, begun = 0;

	if (level_glow_off < 0)
	{
		level_glow_off = psp_keep("nolevelglow");
		if (level_glow_off)
			platform_log("GE: nolevelglow: level self-illumination left out");
	}
	if (level_glow_off)
		return;
	for (which = 0; which < 2; which++)
	{
		if (!level_glow_color[which] || !psp_tex_bind(level_glow_key[which], NULL))
			continue;
		if (!begun)
		{
			/* over the triangles the base pass drew (depth equal), as the
			base's own: its coordinates and its level of detail */
			sceGuEnable(GU_TEXTURE_2D);
			sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
			sceGuTexWrap(GU_REPEAT, GU_REPEAT);
			sceGuDisable(GU_ALPHA_TEST);
			sceGuDepthFunc(GU_EQUAL);
			sceGuDepthMask(GU_TRUE);
			/* added light fades to nothing in the fog */
			sceGuFog(fog_near, fog_far, 0xFF000000);
			sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
			ge_blend(1);
			if (uv_centre)
				uv_shift(1.0f, 1.0f);
			begun = 1;
		}
		sceGuColor(0xFF000000 | level_glow_color[which]);
		draw_pass_angled(stream->base, draw, (int)kept_count * 3, pieces, cut_count, 0, slope, 0);
		level_glow_passes++;
	}
	if (!begun)
		return;
	ge_blend(0);
	if (uv_centre)
		sceGuTexOffset(0.0f, 0.0f);
	sceGuFog(fog_near, fog_far, fog_color);
	sceGuDepthMask(GU_FALSE);
	sceGuDepthFunc(GU_GEQUAL);
	sceGuColor(0xFFFFFFFF);
	if (!textured)
		sceGuDisable(GU_TEXTURE_2D);
}

static void psp_ge_draw_level_body(const void *vertices, const void *lightmap_vertices, int vertex_count,
	const unsigned short *indices, long triangle_count, unsigned int color,
	unsigned int base_key, unsigned int lightmap_key, int alpha_tested)
{
	const unsigned short *draw = indices;
	const unsigned short *const engine_indices = indices;
	unsigned short *kept = NULL;
	long index, kept_count = 0;
	unsigned int lowest = 0xFFFF, highest = 0;
	struct level_stream *stream;
	int cut_count = 0, base_alpha = 0, textured, all_inside, far_possible = 0;
	int fog_pieces = 0, fog_dropped = 0;
	float box_near = 1e30f, box_far = -1e30f;
	unsigned int cut_us = 0;
	float base_slope = 0.0f, detail_pass_slope = 0.0f;
	struct cut_store pieces_store;

	if (!frame_open || triangle_count <= 0 || !vertices || !indices || vertex_count <= 0 || list_full())
		return;
	part_start();
	cut_vertices_count = 0;
	cut_vertices_prepared[0] = cut_vertices_prepared[1] = NULL;
	cut_store_begin(&pieces_store, cut_corners, MAXIMUM_CUT_CORNERS, CUT_LEVEL);
	far_cut_begin();
	stream_asked_texture = base_key;
	stream = level_stream_get(vertices, lightmap_vertices, vertex_count);
	if (!stream)
		return;
	level_frame(stream->origin);
	/* build 87: the stencil bake's pieces in place of the triangles it cut */
	if (stream->bake)
	{
		const unsigned short *swapped = bake_substitute(stream, indices, &triangle_count, 1);

		if (swapped != indices)
		{
			indices = draw = swapped;
			vertex_count = stream->count;
		}
	}
	for (index = 0; index < triangle_count * 3; index++)
	{
		unsigned int vertex = indices[index];

		if (vertex < lowest)
			lowest = vertex;
		if (vertex > highest)
			highest = vertex;
	}
	if (highest >= (unsigned int)vertex_count)
		return;
	part_add(0);
	/* build 66: a draw whose whole vertex box lies inside the view (in front
	of the near plane, within the guard band) needs no classifying: every
	triangle is kept as it is. Outdoors most draws are (the far walls); the
	ground underfoot still gets the loop. The box's corners are tested the
	way the vertices would be. "noboxtest": the loop for every draw */
	if (box_test < 0)
		box_test = !psp_keep("noboxtest");
	all_inside = 0;
	if (box_test)
	{
		int corner;

		all_inside = 1;
		for (corner = 0; corner < 8 && all_inside; corner++)
		{
			float p[3] = { corner & 1 ? stream->high[0] : stream->low[0], corner & 2 ? stream->high[1] : stream->low[1],
				corner & 4 ? stream->high[2] : stream->low[2] }, c[3];

			to_camera(p, c);
			if (outside(c))
				all_inside = 0;
			if (c[2] < box_near)
				box_near = c[2];
			if (c[2] > box_far)
				box_far = c[2];
		}
		/* a box across the far clip (depth is linear, so its corners bound
		every vertex's): its triangles may need cutting there */
		if (all_inside && far_cut_active && box_far > far_cut_at && box_near <= far_cut_at)
		{
			all_inside = 0;
			far_cut_boxes++;
		}
	}
	if (all_inside)
	{
		kept_count = triangle_count;
		level_draws_inside++;
	}
	else
	{
		memset(vertex_codes + lowest, CODE_UNKNOWN, highest - lowest + 1);
		level_draws_classified++;
	}
	/* each triangle against the camera: skipped, kept, or cut up */
	for (index = 0; index < triangle_count && !all_inside; index++)
	{
		const unsigned short *triangle = indices + 3 * index;
		int bits[3], corner, planes, split;

		for (corner = 0; corner < 3; corner++)
		{
			int code = vertex_codes[triangle[corner]];

			if (code & CODE_UNKNOWN)
			{
				float point[3];

				level_to_camera(&stream->base[triangle[corner]].x, point);
				code = outside(point) | far_cut_code(point[2]);
				vertex_codes[triangle[corner]] = (unsigned char)code;
			}
			bits[corner] = code;
		}
		planes = (bits[0] | bits[1] | bits[2]) & CODE_PLANES;
		split = ((bits[0] | bits[1] | bits[2]) & CODE_FAR) == CODE_FAR;
		if (!planes && !split)
		{
			/* safe: kept as it is (copied only once something was dropped) */
			if (kept)
			{
				kept[3 * kept_count] = triangle[0];
				kept[3 * kept_count + 1] = triangle[1];
				kept[3 * kept_count + 2] = triangle[2];
			}
			kept_count++;
			continue;
		}
		if (!kept)
		{
			/* the first triangle left out: from here the kept ones are copied
			into the display list's memory */
			kept = (unsigned short *)list_take((int)(triangle_count * 3 * sizeof(unsigned short) + 3) & ~3);
			if (!kept)
				return;
			memcpy(kept, indices, kept_count * 3 * sizeof(unsigned short));
			draw = kept;
		}
		if (bits[0] & bits[1] & bits[2] & (CODE_PLANES | CODE_PAST))
		{
			culled_this_frame++;
			if (!(bits[0] & bits[1] & bits[2] & CODE_PLANES))
				far_cut_dropped++;
		}
		else
		{
			float corners[3][CORNER_FLOATS] = { { 0.0f } };

			for (corner = 0; corner < 3; corner++)
			{
				const struct ge_vertex *b = &stream->base[triangle[corner]];

				level_to_camera_corner(&b->x, corners[corner]);
				corners[corner][3] = b->u;
				corners[corner][4] = b->v;
				corners[corner][5] = stream->lightmap ? stream->lightmap[triangle[corner]].u : 0.0f;
				corners[corner][6] = stream->lightmap ? stream->lightmap[triangle[corner]].v : 0.0f;
			}
			{
				/* build 47: the cutting timed on its own (taken out of the
				classification's time below) */
				int before = cut_count, dropped = pieces_store.dropped;
				unsigned int cut_start = ge_clock();

				cut_count += split ? clip_at_far(corners, planes, &pieces_store) :
					clip_triangle(corners, planes, &pieces_store);
				/* pieces cut only at the far clip are far: no reason for the detail passes */
				if (!planes)
				{
					fog_pieces += cut_count - before;
					fog_dropped += pieces_store.dropped - dropped;
				}
				cut_us += ge_clock() - cut_start;
			}
		}
	}
	ge_part_us[2] += cut_us;
	ge_part_calls[2]++;
	part_add(1);
	ge_part_us[1] -= cut_us;
	/* build 87: the ME classifies the same range again ("meclassify") */
	if (!all_inside)
		me_classify_measure(stream, lowest, highest);
	if (cut_count > clip_peak)
		clip_peak = cut_count;
	if (kept_count + cut_count == 0 && !pieces_store.dropped)
		return;
	/* build 40: nothing left out, so the GE reads the game's own triangle
	list, which the game wrote this frame into memory the CPU caches. The GE
	draws as soon as a draw is listed, before the frame's end writes the cache
	back, so it read whatever an earlier frame left there: the level's
	triangles came and went with the view (the canyon walls, the same way
	each time the view turned). Out of the cache first, as the meters (build 39) */
	if (!kept)
		ge_writeback(indices, triangle_count * 3 * sizeof(unsigned short));
	/* build 64: sorted by slant (angle_sort; the copy, if one is made, is the
	display list's own). Build 66: with the far triangles (past DETAIL_FAR)
	set apart, when the draw has detail maps and reaches that far */
	if (detail_off < 0)
		detail_off = psp_keep("nodetail");
	if (detail_far < 0)
		detail_far = !psp_keep("detailnear");
	if (!detail_off && (detail_key[0] || detail_key[1]))
	{
		float far = fog_far < DETAIL_FAR ? fog_far : DETAIL_FAR, reach = 0.0f;
		int axis;

		/* the box's farthest corner from the eye: nearer than DETAIL_FAR,
		and no triangle can be far */
		for (axis = 0; axis < 3; axis++)
		{
			float a = fabsf(stream->low[axis] - camera_eye[axis]), b = fabsf(stream->high[axis] - camera_eye[axis]);
			float m = a > b ? a : b;

			reach += m * m;
		}
		detail_far_squared = far * far;
		far_possible = reach > detail_far_squared;
	}
	/* geshow2: the engine's own index list, rewritten by the next frame's
	walk while the GE may still be drawing this one, copied into the list */
	if (show2_copy_indices && draw == engine_indices && kept_count > 0)
	{
		unsigned short *copy = (unsigned short *)list_take((int)(kept_count * 3 * sizeof(unsigned short) + 3) & ~3);

		if (!copy)
			return;
		memcpy(copy, draw, kept_count * 3 * sizeof(unsigned short));
		draw = copy;
		show2_index_copies++;
		show2_index_bytes += kept_count * 3 * sizeof(unsigned short);
	}
	{
		unsigned int angle_start = ge_clock();

		draw = angle_sort(stream->base, draw, kept_count, far_possible, lowest, highest);
		angle_us += ge_clock() - angle_start;
		/* build 89: the pieces cut at the eye are near by definition: with
		every kept triangle past the detail fade, the detail passes were
		skipped for them too (the ground at the feet lost its detail map when
		the rest of a large material's visible triangles were far; review) */
		if (cut_count > fog_pieces || pieces_store.dropped > fog_dropped)
			angle_near_any = 1;
	}
	/* build 66: the cut corners as GE vertices, once for the base passes and
	once for the lightmap's */
	if (cut_count > 0)
	{
		int which;

		for (which = 0; which < (stream->lightmap ? 2 : 1); which++)
		{
			struct ge_vertex *corners = list_take(cut_count * sizeof(struct ge_vertex));

			if (!corners)
				break;
			for (index = 0; index < cut_count; index++)
			{
				const float *c = cut_at(&pieces_store, index);

				corners[index].u = c[which ? 5 : 3];
				corners[index].v = c[which ? 6 : 4];
				corners[index].x = c[7];
				corners[index].y = c[8];
				corners[index].z = c[9];
			}
			cut_vertices_prepared[which] = corners;
		}
		cut_vertices_count = cut_count;
	}
	level_model(stream->origin);

	/* build 35: the back faces culled, as the Xbox does (D3DCULL_CCW for
	every environment pass). Drawn, a hill's far side shares the 16-bit depth
	of its near side at the crest far off, so the multiplying passes (depth
	EQUAL) landed there twice: the bright dots along distant ridges. Fewer
	pixels for the GE too. Keep words: "nocull", "cullflip" */
	if (level_cull < 0)
		level_cull = psp_keep("nocull") ? 0 : psp_keep("cullflip") ? 2 : 1;
	if (level_cull)
	{
		sceGuFrontFace(level_cull == 1 ? LEVEL_FRONT : (LEVEL_FRONT == GU_CW ? GU_CCW : GU_CW));
		sceGuEnable(GU_CULL_FACE);
	}

	textured = psp_tex_bind(base_key, &base_alpha);
	/* build 79: the coordinates of the vertex nearest the eye (of every
	eighth or so of the draw's; a near one is all that's needed, the shift
	only has to be small where the surface is large on the screen) */
	uv_centre_u = uv_centre_v = 0.0f;
	if (uv_centre < 0)
		uv_centre = !psp_keep("uvwide");
	unsigned int probe_mark = probe_clock(); /* build 119's probe */

	if (textured && uv_centre)
	{
		unsigned int step = (highest - lowest) / 96 + 1, i;
		float best = 1e30f;

		for (i = lowest; i <= highest; i += step)
		{
			const struct ge_vertex *v = &stream->base[i];
			float dx = v->x - level_eye[0], dy = v->y - level_eye[1], dz = v->z - level_eye[2];
			float d = dx * dx + dy * dy + dz * dz;

			if (d < best)
			{
				best = d;
				uv_centre_u = v->u;
				uv_centre_v = v->v;
			}
		}
		uv_centred_draws++;
	}
	level_uv_us += probe_clock() - probe_mark;
	level_probe_draws++;
#ifdef PSP_DEBUG_TOOLS
	if (sdump_wanted())
	{
		extern char const *psp_shader_tag_name(void const *shader); /* hud_draw.c (psp_fixups.py) */
		float box[4], c[3];
		char text[64];
		char const *shader = sdump_shader ? (const char *)sdump_shader : NULL;
		char const *shader_name = shader ? psp_shader_tag_name(shader) : NULL;
		long corner;

		sdump_box_start(box);
		for (corner = 0; corner < triangle_count * 3; corner++)
		{
			level_to_camera(&stream->base[indices[corner]].x, c);
			sdump_box_add(box, c);
		}
		if (sdump_box_text(box, text, sizeof(text)))
			platform_log("sdump: level %s shader %s (type %d flags %04x kind %d) map %s alpha tested %d map alpha %d triangles %ld",
				text, shader_name ? shader_name : "?", shader ? *(const short *)(shader + 36) : -1,
				shader ? *(const unsigned short *)(shader + 40) : 0, shader ? *(const short *)(shader + 42) : -1,
				sdump_map_name(base_key), alpha_tested, base_alpha, triangle_count);
	}
#endif
	if (textured)
	{
		int base_width = 0, base_height = 0;

		if (stream->uv_per_unit <= 0.0f)
			stream->uv_per_unit = uv_density(stream->base, indices, triangle_count);
		psp_tex_size(base_key, &base_width, &base_height);
		base_slope = texture_levels_by_distance(stream->uv_per_unit, base_width, base_height, 1.0f, tex_bias);
	}
	if (textured)
	{
		/* build 53: the passes on their own, for finding which one draws
		something (keep word "passes", then select + left cycles; psp_sdl.c) */
		int white_base = psp_pass_mode == 3, skip_lightmap = psp_pass_mode == 1 || psp_pass_mode == 4,
			skip_detail = psp_pass_mode == 1 || psp_pass_mode == 2 || psp_pass_mode == 3;

		/* build 87: the 16-bit copies for this draw's passes, when built */
		compact_now = stream->compact_base ? stream : NULL;
		pass_scale_u = pass_scale_v = 1.0f;
		/* pass 1: the base texture, fading toward the fog */
		probe_mark = probe_clock();
		sceGuEnable(GU_TEXTURE_2D);
		sceGuTexFunc(GU_TFX_REPLACE, base_alpha && alpha_tested ? GU_TCC_RGBA : GU_TCC_RGB);
		sceGuTexWrap(GU_REPEAT, GU_REPEAT);
		if (uv_centre)
			uv_shift(1.0f, 1.0f);
		if (base_alpha && alpha_tested)
		{
			sceGuAlphaFunc(GU_GREATER, 0x7F, 0xFF);
			sceGuEnable(GU_ALPHA_TEST);
		}
		if (white_base)
		{
			sceGuDisable(GU_TEXTURE_2D);
			sceGuColor(0xFFFFFFFF);
		}
		draw_pass_angled(stream->base, draw, (int)kept_count * 3, &pieces_store, cut_count, 0, base_slope, 0);
		if (white_base)
			sceGuEnable(GU_TEXTURE_2D);
		sceGuDisable(GU_ALPHA_TEST);
		if (uv_centre)
			sceGuTexOffset(0.0f, 0.0f);
		textured_draws++;
		level_base_us += probe_clock() - probe_mark;
		probe_mark = probe_clock();
		/* pass 2: the lightmap multiplied in, where pass 1 drew */
		if (!skip_lightmap && stream->lightmap && psp_tex_bind_lightmap(lightmap_key))
		{
			sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
			sceGuTexWrap(GU_CLAMP, GU_CLAMP);
			sceGuDepthFunc(GU_EQUAL);
			sceGuDepthMask(GU_TRUE);
			/* far off the lightmap fades to white, leaving the fog colour
			from pass 1 */
			sceGuFog(fog_near, fog_far, 0xFFFFFFFF);
			/* src x dst, once: the Xbox lightmap pass writes lightmap x
			material colour (final combiner T2 x EF, no shift) and the
			texture pass multiplies the frame by the base (DESTCOLOR, ZERO);
			only the detail maps are doubled (build 35: was 2 x, overbright).
			Dithered: this pass sets the level's brightness (build 117) */
			sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_FIX, 0, 0);
			ge_blend_dithered();
			draw_pass(stream->lightmap, draw, (int)kept_count * 3, &pieces_store, cut_count, 1);
			ge_blend(0);
			sceGuFog(fog_near, fog_far, fog_color);
			sceGuDepthMask(GU_FALSE);
			sceGuDepthFunc(GU_GEQUAL);
		}
		level_lightmap_us += probe_clock() - probe_mark;
		probe_mark = probe_clock();
		/* passes 3 and 4: the detail maps, the same way */
		if (detail_off < 0)
			detail_off = psp_keep("nodetail");
		if (!detail_off && !skip_detail && (detail_key[0] || detail_key[1]) && !angle_near_any)
			detail_passes_skipped_far++;
		if (!detail_off && !skip_detail && (detail_key[0] || detail_key[1]) && angle_near_any)
		{
			int pass;
			float far = fog_far < DETAIL_FAR ? fog_far : DETAIL_FAR;
			float near = fog_near < DETAIL_NEAR ? fog_near : DETAIL_NEAR;

			/* build 56: the detail maps a level and a half blurrier. They
			tile tens of times across a surface, and the GE's choice of level
			(too sharp at a slant, see ge_initialize) left their fine pattern
			beating against the pixels: the canyon walls' bands (b54's
			pass screenshots: present with the detail maps, gone without).
			A detail map's smaller levels fade to its neutral average, so
			blurrier only means less of a pattern that couldn't be seen.
			"detailsharp": as before; "detailbias2": two and a half */
			/* build 64: no extra blur by default (the faded levels do the
			fading); "detailbias15": the b56-b63 1.5, "detailbias2": 2.5 */
			if (detail_bias < -1.0f)
				detail_bias = psp_keep("detailbias15") ? 1.5f : psp_keep("detailbias2") ? 2.5f : 0.0f;
			if (detail_far < 0)
				detail_far = !psp_keep("detailnear");
			sceGuTexLevelMode(tex_level_mode, tex_bias + detail_bias);
			for (pass = 0; pass < 2; pass++)
			{
				int detail_width = 0, detail_height = 0;
				float detail_scale;

				if (!detail_key[pass] || !psp_tex_bind(detail_key[pass], NULL))
					continue;
				psp_tex_size(detail_key[pass], &detail_width, &detail_height);
				detail_scale = sqrtf(fabsf(detail_u[pass] * detail_v[pass]));
				/* build 64: by distance again (b59-b63: the GE's own choice,
				which on the PSP went too blurry at a slant: the bunker floor
				and the grass lost their detail, b63's run A against B),
				with the slant added back per triangle (the angle buckets
				below). "detailauto": the GE's own choice */
				if (detail_slope < 0)
					detail_slope = !psp_keep("detailauto");
				detail_pass_slope = 0.0f;
				if (detail_slope)
					detail_pass_slope = texture_levels_by_distance(stream->uv_per_unit, detail_width, detail_height,
						detail_scale, tex_bias + detail_bias);
				sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
				sceGuTexWrap(GU_REPEAT, GU_REPEAT);
				sceGuTexScale(detail_u[pass], detail_v[pass]);
				pass_scale_u = detail_u[pass];
				pass_scale_v = detail_v[pass];
				if (uv_centre)
					uv_shift(detail_u[pass], detail_v[pass]);
				sceGuDepthFunc(GU_EQUAL);
				sceGuDepthMask(GU_TRUE);
				sceGuFog(near, far > near ? far : near + 1.0f, detail_neutral(detail_key[pass]));
				sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_SRC_COLOR, 0, 0);
				ge_blend(1);
				ge_layer_dither();
				/* build 91: a detail map's smaller levels are faded toward a
				colour other than their top level's average (Bungie's detail
				fade: measured on Blood Gulch, "detail metal sratches" loses
				43% of its brightness by its eighth level, "detail grass" turns
				purple), so two triangles side by side in different slant
				buckets showed a step in brightness along their shared edge
				(b90 run 1, the base floors). The GE's slope mode picks the
				level per pixel by depth, which changes smoothly; the buckets
				only add the slant, per triangle. Base maps keep the buckets
				(their levels keep their average). "slant per triangle (old)"
				in the debug menu restores them here */
				if (detail_by_distance)
				{
					sceGuTexSlope(detail_pass_slope);
					draw_pass(stream->base, draw, (int)kept_count * 3, &pieces_store, cut_count, 0);
				}
				else
					draw_pass_angled(stream->base, draw, (int)kept_count * 3, &pieces_store, cut_count, 0, detail_pass_slope, 1);
				detail_passes++;
			}
			ge_blend(0);
			sceGuTexLevelMode(tex_level_mode, tex_bias);
			sceGuTexScale(1.0f, 1.0f);
			pass_scale_u = pass_scale_v = 1.0f;
			sceGuTexOffset(0.0f, 0.0f);
			sceGuFog(fog_near, fog_far, fog_color);
			sceGuDepthMask(GU_FALSE);
			sceGuDepthFunc(GU_GEQUAL);
		}
		level_detail_us += probe_clock() - probe_mark;
		level_glow_pass(stream, draw, kept_count, &pieces_store, cut_count, base_slope, 1);
		sceGuDisable(GU_TEXTURE_2D);
		sceGuTexLevelMode(tex_level_mode, tex_bias);
		if (submerged_density > 0.0f)
			submerged_pass(stream, draw, kept_count, lowest, highest);
		/* build 90: for the permanent decals (not with the 16-bit copies,
		which go through another vertex path) */
		if (!compact_now && !alpha_tested)
			frame_level_record(stream, draw, (int)kept_count,
				cut_count > 0 && cut_count == cut_vertices_count ? cut_vertices_prepared[0] : NULL, cut_count);
		compact_now = NULL;
	}
	else
	{
		/* not baked, or still on its way from the Memory Stick: flat */
		sceGuColor(color);
		compact_now = stream->compact_base ? stream : NULL;
		pass_scale_u = pass_scale_v = 1.0f;
		draw_pass(stream->base, draw, (int)kept_count * 3, &pieces_store, cut_count, 0);
		level_glow_pass(stream, draw, kept_count, &pieces_store, cut_count, 0.0f, 0);
		compact_now = NULL;
		flat_draws++;
	}
	world_model();
	sceGuDisable(GU_CULL_FACE);
	draws_this_frame++;
	triangles_this_frame += kept_count;
	part_add(3);
}

/* "levelpretouch" (on; levelpretouchoff): a level draw without its copy
gets it built now, as its record is written. The Media Engine's replay
can't build one: it asked for it at the frame's end and left the draw out
of that frame (every level draw of the frame after a BSP switch, B16; a
piece of the ocean or the shore's water gone for a frame, B21). Only a
missing copy settles the frame in flight, so frames whose copies are all
built pay one lookup a draw (geopretouch142 built on every draw and cost
0.8-2.2 ms) */
static int pretouch_me_busy(void);
static void level_pretouch(const void *vertices, const void *lightmap_vertices, int vertex_count, unsigned int key)
{
	static int level_pretouch_on = -1;

	if (!PRETOUCHING() || !vertices || vertex_count <= 0)
		return;
	if (level_pretouch_on < 0)
		level_pretouch_on = !psp_keep("levelpretouchoff");
	if (!level_pretouch_on || level_stream_find(vertices))
		return;
	if (inflight_room_away)
		inflight_room_park();
	if (inflight_pending)
		inflight_sync();
	if (pretouch_me_busy())
		return;
	stream_asked_texture = key;
	level_pretouches++;
	if (!level_stream_get(vertices, lightmap_vertices, vertex_count))
		level_pretouch_missed++;
}

void psp_ge_draw_level(const void *vertices, const void *lightmap_vertices, int vertex_count,
	const unsigned short *indices, long triangle_count, unsigned int color,
	unsigned int base_key, unsigned int lightmap_key, int alpha_tested)
{
	GE_PRETOUCH(base_key, TXPT_LEVEL_BASE);
	GE_PRETOUCH(lightmap_key, TXPT_LIGHTMAP);
	level_pretouch(vertices, lightmap_vertices, vertex_count, base_key);
	GE_DEFER(defer_draw_level(vertices, lightmap_vertices, vertex_count, indices, triangle_count, color, base_key,
		lightmap_key, alpha_tested)); /* build 112: gedefer */
	GE_BENCH_SKIP();
	ge_kind(KIND_LEVEL); /* build 103 */
	unsigned int start = ge_clock();

	ge_category = GE_SKIP_LEVEL;
	psp_ge_draw_level_body(vertices, lightmap_vertices, vertex_count, indices, triangle_count, color, base_key, lightmap_key, alpha_tested);
	ge_category = 0;
	ge_entry_us[0] += ge_clock() - start;
}

/* ---------- objects: a model's parts, each drawn with one node's matrix

A model's vertices (model_vertex_compressed: position, three packed vectors,
16-bit texture coordinates, two node indices and a weight) are only read by
the renderer, so the first time a part is drawn its vertices are rewritten
in place for the GE (build 20): texture coordinate (2 floats), the nodes and
weight where the GE expects a normal (unused, lighting is off), then the
position. Every part is still drawn rigidly with the node of its first
vertex: right for vehicles, scenery and weapons, approximate for anything
that bends */

static const float *model_nodes = NULL;
static int model_node_count = 0;
static float model_u_scale = 1.0f, model_v_scale = 1.0f;
#define CONVERTED_BUFFERS 2048
static const void *converted[CONVERTED_BUFFERS];
/* build 68: the geometry cache's generation of the block when it was
converted; the block can hold other vertices after an eviction */
static unsigned int converted_generation[CONVERTED_BUFFERS];
static unsigned long converted_count = 0, reconverted_count = 0;
static unsigned long multi_node_rigid = 0, transparent_placed = 0;
/* multi_node_rigid by why: see-through, no triangles given, the skinning plan
failed (the log names only the first 12 buffers, which see-through parts
usually take) */
static unsigned long multi_node_rigid_why[3];

/* "model14" (docs/design_model14.md): an opaque rigid part's vertices as
14-byte records: u16 uv (the source's s16 + 32768, undone by the texture
offset), s8 normal with the node in the pad byte the GE skips, s16 position
on a power-of-two step, the part's own, folded into its model matrix. A
step that is a power of two keeps the matrix a uniform scale (normals keep
their direction) and puts shared corners of parts with one step on the same
lattice point. First-person parts, the sky and see-through parts stay in
floats: the first are seen closest, the last are read on the CPU */
static int model14 = -1;
/* the step's exponent + 128 a converted block (0: float records) */
static unsigned char converted_step[CONVERTED_BUFFERS];
/* set by the caller before model_convert: the part must stay in floats */
static int convert_float_only = 0;
/* the step of the block model_convert last answered for (0: floats), and of
the part being drawn */
static float convert_k = 0.0f, part_k14 = 0.0f;
static unsigned long model14_parts = 0, model14_vertices = 0, model14_refused = 0;
/* skinned parts left at 28/32 bytes: what step 3b would take 10 bytes a vertex off */
static unsigned long model14_skinned_parts = 0, model14_skinned_vertices = 0;

static float step_of(unsigned char code)
{
	union { float f; unsigned int u; } k;

	if (!code)
		return 0.0f;
	k.u = (unsigned int)(code - 128 + 127) << 23;
	return k.f;
}

/* a rigid record's position in the part's own space */
static const float *record_point(const void *vertices, int v, float *out)
{
	const short *q;

	if (part_k14 == 0.0f)
		return (const float *)((const char *)vertices + 24 * v + 12);
	q = (const short *)((const char *)vertices + 14 * v + 8);
	out[0] = q[0] * part_k14;
	out[1] = q[1] * part_k14;
	out[2] = q[2] * part_k14;
	return out;
}

/* the part's texture scale; a 14-byte part's u16 coordinates read as
(s + 32768) / 32768, so the offset takes the 32768 back off */
static void model_tex_scale(float su, float sv)
{
	if (part_k14 != 0.0f)
	{
		su *= 32768.0f / 32767.0f;
		sv *= 32768.0f / 32767.0f;
		sceGuTexScale(su, sv);
		sceGuTexOffset(-su, -sv);
	}
	else
		sceGuTexScale(su, sv);
}

/* the model's lighting, from the game (build 22): an ambient colour and up
to two distant lights, each a colour and the direction its light travels */
static unsigned int model_ambient = MODEL_LIGHT;
static int model_light_count = 0;
static unsigned int model_light_color[2];
static ScePspFVector3 model_light_toward[2];
/* build 117: and up to two point lights (the GE's lights 2 and 3), each a
place and a colour already scaled by its reach at the model's centre
(psp_fixups.py, the model's begin) */
static int model_point_count = 0;
static unsigned int model_detail_key; /* psp_ge_model_detail, below */
static unsigned int model_point_color[2];
static ScePspFVector3 model_point_position[2];
static unsigned long model_point_parts = 0;

static unsigned int color_from_rgb(const float *rgb)
{
	int r = (int)(rgb[0] * 255.0f), g = (int)(rgb[1] * 255.0f), b = (int)(rgb[2] * 255.0f);

	r = r < 0 ? 0 : r > 255 ? 255 : r;
	g = g < 0 ? 0 : g > 255 ? 255 : g;
	b = b < 0 ? 0 : b > 255 ? 255 : b;
	return 0xFF000000 | (b << 16) | (g << 8) | r;
}

/* "modelambientN", "modellightN": the models' ambient light and distant
lights times N percent (100 when absent), to find how far the GE's lighting
falls short of the original's in shade (the PC reference draws first-person
weapons and armour lighter there) before a default is chosen. Read without a
log line: this may first run on the Media Engine, which mustn't print */
static float model_ambient_gain = -1.0f, model_light_gain = 1.0f;

static unsigned int color_from_rgb_scaled(const float *rgb, float gain)
{
	float scaled[3];

	if (gain == 1.0f)
		return color_from_rgb(rgb);
	scaled[0] = rgb[0] * gain;
	scaled[1] = rgb[1] * gain;
	scaled[2] = rgb[2] * gain;
	return color_from_rgb(scaled);
}

void psp_ge_model_begin(const float *node_matrices, int node_matrix_count, float u_scale, float v_scale,
	const float *ambient, int light_count, const float *light0, const float *light1)
{
	GE_BENCH_SKIP();
	/* model14's switch, read on the main CPU before any replay needs it, and
	written back: the ME only reads it (a lazy read there would write a line
	the main CPU may also hold) */
	if (model14 < 0 && !psp_on_me())
	{
		model14 = psp_keep("model14");
		sceKernelDcacheWritebackRange(&model14, sizeof(model14));
	}
	if (DEFER_RECORDING() &&
		defer_model_begin(node_matrices, node_matrix_count, u_scale, v_scale, ambient, light_count, light0, light1))
		return;
	int index;

	model_nodes = node_matrices;
	model_node_count = node_matrix_count;
	model_u_scale = u_scale != 0.0f ? u_scale : 1.0f;
	model_v_scale = v_scale != 0.0f ? v_scale : 1.0f;
	if (model_ambient_gain < 0.0f)
	{
		model_ambient_gain = (float)psp_keep_number("modelambient", 100) * 0.01f;
		model_light_gain = (float)psp_keep_number("modellight", 100) * 0.01f;
	}
	model_ambient = ambient ? color_from_rgb_scaled(ambient, model_ambient_gain) : MODEL_LIGHT;
	model_light_count = light_count < 0 ? 0 : light_count > 2 ? 2 : light_count;
	model_point_count = 0; /* build 117: psp_ge_model_point_light follows */
	/* a part's detail map is set by its own call (psp_ge_model_detail), made
	only where a detail pass is wanted: the menu's showcase, or Chief with
	chiefdetail. The draw body no longer checks the showcase, so a key left
	from the menu's last part must not reach the next map's models */
	model_detail_key = 0;
	for (index = 0; index < model_light_count; index++)
	{
		const float *light = index ? light1 : light0;

		model_light_color[index] = color_from_rgb_scaled(light, model_light_gain);
		/* the GE wants the direction toward the light */
		model_light_toward[index].x = -light[3];
		model_light_toward[index].y = -light[4];
		model_light_toward[index].z = -light[5];
	}
}

/* build 117: one of the object's point lights, after psp_ge_model_begin */
void psp_ge_model_point_light(const float *position, unsigned int color)
{
	GE_BENCH_SKIP();
	GE_DEFER(defer_model_point_light(position, color));
	if (model_point_count >= 2 || !position)
		return;
	/* the GE lights after the model matrix, and this file's model matrices
	have ge_eye at the origin (relative), so the light's place is given
	from there too (at replay, after GE_DEFER, the camera is the one the
	model is drawn from) */
	model_point_position[model_point_count].x = position[0] - ge_eye[0];
	model_point_position[model_point_count].y = position[1] - ge_eye[1];
	model_point_position[model_point_count].z = position[2] - ge_eye[2];
	model_point_color[model_point_count] = color;
	model_point_count++;
}

/* build 38: whether the next model is the first-person weapon (its own
projection and depth slice), and whether its bounding sphere comes within
reach of the near plane: the GE draws a triangle that reaches behind the eye
as a wedge across the screen (b37's shot of a rock in the cave), so
those models' triangles are checked one by one (psp_ge_draw_model) */
static void use_projection(int first_person)
{
	sceGumMatrixMode(GU_PROJECTION);
	perspective(projection_fov * (180.0f / 3.14159265f),
		first_person ? first_person_near : projection_near, first_person ? first_person_far : projection_far);
	if (first_person)
		sceGuDepthRange(65535, FIRST_PERSON_DEPTH_FAR);
	else
		sceGuDepthRange(WORLD_DEPTH_NEAR, 0);
	sceGumMatrixMode(GU_MODEL);
}

void psp_ge_model_shape(int first_person, const float *centroid, float radius, float near_fp, float far_fp)
{
	GE_BENCH_SKIP();
	GE_DEFER(defer_model_shape(first_person, centroid, radius, near_fp, far_fp));
	model_sphere_radius = 0.0f; /* build 118: none until this model's is known */
	if (!frame_open)
		return;
	if (near_fp > 0.001f)
		first_person_near = near_fp;
	if (far_fp > first_person_near)
		first_person_far = far_fp;
	first_person = first_person != 0;
	if (first_person)
		first_person_models++;
	if (first_person != model_first_person)
	{
		use_projection(first_person);
		model_first_person = first_person;
	}
	model_near = 0;
	model_pixels = 1.0e9f;
	if (!first_person && centroid && radius > 0.0f)
	{
		float c[3];

		to_camera(centroid, c);
		/* build 118: kept for the model's see-through parts (transparent_cut) */
		model_sphere[0] = c[0];
		model_sphere[1] = c[1];
		model_sphere[2] = c[2];
		model_sphere_radius = radius;
		/* build 107: the sphere's diameter on the screen, for the size floor */
		if (c[2] > radius)
			model_pixels = 2.0f * radius * (SCREEN_HEIGHT * 0.5f) / (fov_tan_half() * c[2]); /* build 114: the remembered tangent (b112 cache model: tanf 14,000 instructions a frame) */
		/* build 42: also near enough that a corner could land past the GE's
		reach to the side (a point at depth d inside the model's sphere is at
		most its radius out; past about 7 d it is off the GE's 2048 pixels) */
		/* build 56: the exact bound instead of build 42's rule of thumb. The
		sphere's nearest depth is c[2] - radius and its widest reach to a
		side |c| + radius; if that reach can pass the guard band at that
		depth, some corner may land past the GE's 2048 pixels and wrap into
		a spike (b54: a warthog beside the player, its wheel arm's triangle
		swept across the screen). Then each triangle is checked, as for
		models at the eye */
		/* build 64: the nearest model to the eye, for nearauto's next frame */
		if (!in_sky)
		{
			float reach = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]) - radius;

			/* build 84: a sphere the eye is inside doesn't count (see
			near_plane_auto). Build 90: only while the player sits in a
			vehicle (the one he is in). On foot against the tank, the eye is
			inside its large sphere too, and b84-b89 then let the near plane
			out to 25 cm and cut the hull open (b87's shot 029): on foot
			such a sphere counts as touching the eye. Build 116: and in a
			first-person seat (psp_player_seated 2: a Pelican's bench) */
			if (reach < 0.0f && player_seated != 1)
				reach = 0.0f;
			if (reach >= 0.0f && reach < near_object_distance)
			{
				near_object_distance = reach;
				near_object_radius = radius;
			}
		}
		{
			float nearest = c[2] - radius;

			model_near = c[2] + radius > 0.0f &&
				(nearest < projection_near * 2.0f + 0.02f ||
				fabsf(c[0]) + radius > guard_x * nearest || fabsf(c[1]) + radius > guard_y * nearest);
		}
		if (model_near)
			near_models++;
	}
}

/* active camouflage (build 30): the Xbox draws a camouflaged model as a
shimmer that bends what is behind it; the GE draws it faint, see-through by
the camouflage's amount, without writing depth */
static float model_camouflage = 0.0f;
/* "camoshimmer": the camouflaged model's own map added at this much of
full (0-255) in place of the faded draw (psp_ge_draw_model_body) */
#define CAMO_SHIMMER_LEVEL 48
static int camo_shimmer = -1;
static unsigned long camo_shimmer_parts = 0;
static unsigned long two_sided_draws = 0;
static int model_cull = -1;
int psp_keep(const char *name);

/* An object whose tag says its transparency self-occludes (Cortana) is
drawn by the Xbox in two goes: every see-through part of it writes its depth
with colour off and both sides drawn, then the parts draw as usual, so only
her nearest surface shows (rasterizer_xbox_transparent_geometry.c, the
modifier effect, type 2). Drawn without it, her hair's and head's back
layers blend over her face in the order they sort, and where her parts
meet that shows as a hard edge. Each such model gets a number its parts
carry (model_occluder; 0 for none). Her model on a level that asks for it
(models.c's cortana hack, the type's second byte here) also sorts after
every other see-through draw, as the Xbox's sort puts her.
"noselfocclude": neither, as before */
static unsigned short model_occluder = 0, model_occluder_last = 0;
static unsigned char model_sorted_last = 0;
static int self_occlude = -1;
static unsigned long self_occlude_parts = 0, self_occlude_runs = 0;

void psp_ge_model_effect(int type, float intensity)
{
	int sorted_last = (type >> 8) & 1;

	GE_BENCH_SKIP();
	GE_DEFER(defer_model_effect(type, intensity));
	type &= 0xFF;
	if (self_occlude < 0)
		self_occlude = !psp_keep("noselfocclude");
	model_occluder = 0;
	model_sorted_last = 0;
	if (self_occlude)
	{
		if (type == 2)
		{
			if (++model_occluder_last == 0)
				model_occluder_last = 1;
			model_occluder = model_occluder_last;
		}
		model_sorted_last = (unsigned char)sorted_last;
	}
	model_camouflage = type == 1 && intensity > 0.0f ? (intensity > 1.0f ? 1.0f : intensity) : 0.0f;
}

/* build 114: "statememo" for lighting: the GE's lighting state as this file
last set it, so a part lit as the GE already is sends nothing. Each part
turns lighting on with the ambient and both lights (about 17 command words
and a dozen pspgu calls), draws, and turns it off for its later passes; the
parts of one object share their lighting. Only this file sets the GE's
lighting: model_lighting, the start-up's "off" (lit_forget(0)), and the
two-sided pass's flipped lights (lit_forget(1)). Forgotten at each frame's
start, and never used in the Media Engine's work */
static int lit_memo = -1, lit_enabled = -1, lit_params = 0, lit_on[2];
static unsigned int lit_ambient, lit_color[2];
static ScePspFVector3 lit_toward[2];
/* build 117: the point lights' (lights 2 and 3) */
static int lit_point_on[2];
static unsigned int lit_point_color[2];
static ScePspFVector3 lit_point_position[2];
/* whether the GE may have light 2 or 3 on, memo or not, so a model without
point lights sends no disables when they're already off (nopointlights
then sends what b116 sent) */
static int lit_points_maybe_on = 0;
static unsigned long lit_sets_sent = 0, lit_sets_memo = 0;

/* "modelspecular" (opt-in while it is priced): a shiny part's highlight from
the GE's own specular lighting. The Xbox adds a part's reflection over its
lit base (psp_ge_model_reflection); its brightest share, the sky or a lamp
in the cube map, shows as a glint that moves as the part turns. Here the
two directional lights are also specular lights of their own colour, the
part's specular material is its reflection's strength (part_specular, 0 for
a part that reflects nothing), and the GE adds the specular term after the
texture (separate specular colour), so the glint stays in shade as the
Xbox's sheen does. Per vertex, in the lighting the GE does anyway: no pass
of its own. "specpowerN": the highlight's exponent (8); "specgainN": the
strength in percent (200: the shader's reflection strength is a mean over
the mask, and the highlight stands for its brightest part) */
int psp_keep_number(const char *prefix, int fallback);
/* the reflection words (reflection_keeps, below): read once on the main
CPU, written back, and from then on only read, by whichever CPU replays the
draws. A cache line of their own, the type's alignment padding the struct
to it. Each was a lazy static (-1 until read) on the lines the main CPU
writes while the ME works (PSP_MAIN_SIDE): a Media Engine that still saw -1
read them again itself and wrote that line under the main CPU's dirty copy,
and on the PSP Chief drew yellow-orange in a run with the visor on,
where the emulator's one cache drew him right */
static struct __attribute__((aligned(64))) reflection_words {
	int read, specular, env_fp, env_near, env_pixels, env_gain, env_min, chief_visor, chief_visor_gain, sheen, sheen_gain;
	float specular_power, specular_gain;
} reflection_words = { 0, 0, 0, 0, 160, 100, 0, 0, 150, 0, 100, 8.0f, 2.0f };
static unsigned int part_specular = 0;
static unsigned long specular_parts = 0;

/* lighting changed outside model_lighting: 0 turned off; 1 the lights'
parameters; 2 everything unknown (a frame's start) */
static void lit_forget(int what)
{
	lit_enabled = what == 0 ? 0 : what == 2 ? -1 : lit_enabled;
	lit_params = 0;
}

static void model_lighting(int on)
{
	int index, sent = 0;

	if (lit_memo < 0)
		lit_memo = psp_keep("statememo") || psp_keep("memolight"); /* memolight: this half alone */
	if (!lit_memo || PSP_ME_WORK())
	{
		lit_enabled = -1;
		lit_params = 0;
	}
	if (!on || (!model_light_count && !model_point_count && model_ambient == 0xFF000000))
	{
		if (lit_enabled != 0)
			sceGuDisable(GU_LIGHTING);
		if (lit_memo && !PSP_ME_WORK())
			lit_enabled = 0;
		return;
	}
	if (lit_enabled != 1)
		sceGuEnable(GU_LIGHTING); /* (each part turns lighting off for its later passes: not counted) */
	if (reflection_words.specular)
	{
		/* the part's own: sceGuColor set every material colour to the
		part's, so this is sent for every part */
		if (!lit_params)
		{
			sceGuLightMode(GU_SEPARATE_SPECULAR_COLOR);
			sceGuSpecular(reflection_words.specular_power);
		}
		sceGuMaterial(GU_SPECULAR, (int)part_specular);
		if (part_specular & 0xFFFFFF)
			specular_parts++;
	}
	if (!lit_params || lit_ambient != model_ambient)
	{
		sceGuAmbient(model_ambient);
		sent = 1;
	}
	for (index = 0; index < 2; index++)
	{
		if (index < model_light_count)
		{
			if (!lit_params || memcmp(&lit_toward[index], &model_light_toward[index], sizeof(ScePspFVector3)) != 0)
			{
				sceGuLight(index, GU_DIRECTIONAL, reflection_words.specular ? GU_DIFFUSE_AND_SPECULAR : GU_DIFFUSE,
					&model_light_toward[index]);
				sent = 1;
			}
			if (!lit_params || lit_color[index] != model_light_color[index])
			{
				sceGuLightColor(index, GU_DIFFUSE, model_light_color[index]);
				if (reflection_words.specular)
					sceGuLightColor(index, GU_SPECULAR, model_light_color[index]);
				sent = 1;
			}
			if (!lit_params || !lit_on[index])
			{
				sceGuEnable(GU_LIGHT0 + index);
				sent = 1;
			}
		}
		else if (!lit_params || lit_on[index])
		{
			sceGuDisable(GU_LIGHT0 + index);
			sent = 1;
		}
	}
	/* build 117: the point lights, as lights 2 and 3: a place and a colour
	already scaled by the reach at the model's centre, so no attenuation;
	the GE shades each vertex by its angle to the light */
	for (index = 0; index < 2; index++)
	{
		if (index < model_point_count)
		{
			if (!lit_params || !lit_point_on[index] ||
				memcmp(&lit_point_position[index], &model_point_position[index], sizeof(ScePspFVector3)) != 0)
			{
				sceGuLight(2 + index, GU_POINTLIGHT, GU_DIFFUSE, &model_point_position[index]);
				sceGuLightAtt(2 + index, 1.0f, 0.0f, 0.0f);
				sent = 1;
			}
			if (!lit_params || !lit_point_on[index] || lit_point_color[index] != model_point_color[index])
			{
				sceGuLightColor(2 + index, GU_DIFFUSE, model_point_color[index]);
				sent = 1;
			}
			if (!lit_params || !lit_point_on[index])
			{
				sceGuEnable(GU_LIGHT2 + index);
				sent = 1;
			}
		}
		else if (lit_points_maybe_on && (!lit_params || lit_point_on[index]))
		{
			sceGuDisable(GU_LIGHT2 + index);
			sent = 1;
		}
	}
	lit_points_maybe_on = model_point_count > 0;
	if (on && model_point_count)
		model_point_parts++;
	if (sent)
		lit_sets_sent++;
	else
		lit_sets_memo++;
	if (!lit_memo || PSP_ME_WORK())
		return;
	lit_enabled = 1;
	lit_params = 1;
	lit_ambient = model_ambient;
	for (index = 0; index < 2; index++)
	{
		lit_on[index] = index < model_light_count;
		if (lit_on[index])
		{
			lit_toward[index] = model_light_toward[index];
			lit_color[index] = model_light_color[index];
		}
		lit_point_on[index] = index < model_point_count;
		if (lit_point_on[index])
		{
			lit_point_position[index] = model_point_position[index];
			lit_point_color[index] = model_point_color[index];
		}
	}
}

/* Halo's packed normal: 11, 11 and 10 bits, signed (rasterizer_geometry.c's
uncompress_int32_to_real_vector3d) */
static void unpack_normal(unsigned int packed, float *normal)
{
	float value = (float)(int)(packed << 21);

	normal[0] = (value * (1.0f / 1048576.0f) + 1.0f) * (1.0f / 2047.0f);
	packed >>= 11;
	value = (float)(int)(packed << 21);
	normal[1] = (value * (1.0f / 1048576.0f) + 1.0f) * (1.0f / 2047.0f);
	packed >>= 11;
	value = (float)(int)(packed << 22);
	normal[2] = (value * (1.0f / 2097152.0f) + 1.0f) * (1.0f / 1023.0f);
}

/* the reverse, for uncompressed vertices, so both kinds share one path */
static unsigned int pack_normal(const float *n)
{
	int i = (int)(n[0] * 1023.0f) & 0x7FF, j = (int)(n[1] * 1023.0f) & 0x7FF, k = (int)(n[2] * 511.0f) & 0x3FF;

	return (unsigned int)i | ((unsigned int)j << 11) | ((unsigned int)k << 22);
}

/* ---------- skinning (build 23)

A Halo model part may hang its vertices on several nodes (a cyborg's body
on 16, the first-person arms on 36), each vertex on one node or blended
between two. The GE skins with up to 8 bone matrices a draw, each vertex
weighting them, so a part is split into batches of triangles that touch at
most 8 nodes. The vertices aren't copied per batch: every node of the part
gets one of 8 slots, with any two nodes that share a triangle on different
slots (a colouring of the skeleton's neighbours, which needs few), and each
vertex's weights are written once, by slot, in place of its Xbox data. A
batch then is any set of triangles whose nodes sit on distinct slots, with
those nodes' matrices loaded into the slots.

Record: the weights (one byte each, 128 = all), the texture coordinate, an
8-bit normal, the position; 28 or 32 bytes, within the Xbox's 32. Parts on
one node keep the 24-byte rigid record. */

#define SKIN_SLOTS 8
#define SKIN_NODES 128

static void halo_matrix(const float *node, ScePspFMatrix4 *model);
static void node_matrix(const void *vertices, const unsigned short *indices, ScePspFMatrix4 *model);

struct skin_batch
{
	unsigned char node[SKIN_SLOTS];
	unsigned short *indices;
	int index_count;
	/* build 118: a sphere round the batch's vertices in the part's own
	space (before bending), for the near path's fast test (batch_sphere);
	worked out the first time the batch is drawn near the eye, radius < 0
	until then */
	float center[3], radius;
	/* build 119: its triangles in runs of NEAR_CLUSTER_TRIANGLES, each with
	a sphere like the batch's (centre, radius), for the corner loop's run
	test (near_cluster_verdict); made the first time the loop runs on the
	batch, -1 until then, 0 when there was no room */
	float (*clusters)[4];
	int cluster_count;
};

struct skin_part
{
	int weights, stride, batch_count;
	struct skin_batch *batches;
	int bytes; /* build 112: the one allocation's size, for the live count */
};

static struct skin_part *skinned[CONVERTED_BUFFERS];
/* each node's slot in the part being planned */
static signed char skin_slot[SKIN_NODES];
static unsigned long skinned_parts = 0, skin_batches = 0, skin_failures = 0, skin_bytes = 0;
/* build 112: the plans' bytes on the heap now (skin_bytes counts all ever
made), and the highest that reached */
static unsigned long skin_live_bytes = 0, skin_live_peak = 0;

static void skin_free(struct skin_part *part)
{
	if (!part)
		return;
	skin_live_bytes -= (unsigned long)part->bytes;
	{
		int b;

		/* build 119: the runs' spheres (near_clusters_make) */
		for (b = 0; b < part->batch_count; b++)
			if (part->batches[b].clusters)
			{
				near_cluster_bytes -= (unsigned long)part->batches[b].cluster_count * sizeof(float[4]);
				free(part->batches[b].clusters);
			}
	}
	free(part);
}

/* a vertex's nodes and the first's weight, from either kind of vertex */
static void vertex_nodes(const void *vertices, int index, int uncompressed, int *n0, int *n1, float *w0)
{
	if (uncompressed)
	{
		const unsigned char *source = (const unsigned char *)vertices + 68 * index;
		short node_indices[2];
		float weights[2];

		memcpy(node_indices, source + 56, 4);
		memcpy(weights, source + 60, 8);
		*n0 = node_indices[0];
		*n1 = node_indices[1];
		*w0 = weights[0];
	}
	else
	{
		const unsigned char *source = (const unsigned char *)vertices + 32 * index;
		short weight;

		memcpy(&weight, source + 30, 2);
		*n0 = source[28] / 3;
		*n1 = source[29] / 3;
		*w0 = weight * (1.0f / 32767.0f);
	}
	if (*n0 < 0 || *n0 >= SKIN_NODES)
		*n0 = 0;
	if (*n1 < 0 || *n1 >= SKIN_NODES || *n1 == *n0 || !(*w0 < 0.995f))
	{
		*n1 = -1;
		*w0 = 1.0f;
	}
}

/* the part's triangles as a list (strips unrolled, keeping each triangle's
winding; strips' joining triangles dropped) into a new array */
static unsigned short *triangle_list(const unsigned short *indices, int index_count, int strip, int *triangle_count)
{
	unsigned short *list = malloc((strip ? (index_count - 2) : index_count / 3) * 3 * sizeof(unsigned short) + 4);
	int count = 0, i;

	if (!list)
		return NULL;
	if (strip)
	{
		for (i = 0; i + 2 < index_count; i++)
		{
			unsigned short a = indices[i], b = indices[i + 1], c = indices[i + 2];

			if (a == b || b == c || a == c)
				continue;
			if (i & 1) { unsigned short t = a; a = b; b = t; }
			list[3 * count] = a; list[3 * count + 1] = b; list[3 * count + 2] = c;
			count++;
		}
	}
	else
	{
		memcpy(list, indices, (index_count / 3) * 3 * sizeof(unsigned short));
		count = index_count / 3;
	}
	*triangle_count = count;
	return list;
}

/* works out the slots and batches; NULL leaves the part rigid */
static struct skin_part *skin_plan(const void *vertices, int count, int uncompressed, const unsigned short *indices,
	int index_count, int strip, signed char *node0, signed char *node1, unsigned char *weight0)
{
	static unsigned char conflict[SKIN_NODES][SKIN_NODES / 8];
	signed char *slot_of = skin_slot;
	static int degree[SKIN_NODES], slot_use[SKIN_SLOTS];
	unsigned short *list;
	int triangle_count, used[SKIN_NODES] = { 0 }, node_count = 0, i, t, slots = 0;
	struct skin_part *part;
	struct skin_batch batches[64];
	int batch_count = 0, batch_triangles[64];
	unsigned char *batch_of;

	for (i = 0; i < count; i++)
	{
		int n0, n1;
		float w0;

		vertex_nodes(vertices, i, uncompressed, &n0, &n1, &w0);
		node0[i] = (signed char)n0;
		node1[i] = (signed char)n1;
		weight0[i] = (unsigned char)(w0 * 128.0f + 0.5f);
		if (!used[n0]++) node_count++;
		if (n1 >= 0 && !used[n1]++) node_count++;
	}
	if (node_count <= 1 || !indices || index_count < 3)
		return NULL;
	list = triangle_list(indices, index_count, strip, &triangle_count);
	if (!list || !triangle_count)
	{
		free(list);
		return NULL;
	}
	/* the nodes that share a triangle */
	memset(conflict, 0, sizeof(conflict));
	memset(degree, 0, sizeof(degree));
	for (t = 0; t < triangle_count; t++)
	{
		int nodes[6], n = 0, a, b;

		for (a = 0; a < 3; a++)
		{
			int v = list[3 * t + a];

			if (v >= count)
			{
				free(list);
				return NULL;
			}
			nodes[n++] = node0[v];
			if (node1[v] >= 0)
				nodes[n++] = node1[v];
		}
		for (a = 0; a < n; a++)
			for (b = 0; b < n; b++)
				if (nodes[a] != nodes[b] && !(conflict[nodes[a]][nodes[b] >> 3] & (1 << (nodes[b] & 7))))
				{
					conflict[nodes[a]][nodes[b] >> 3] |= 1 << (nodes[b] & 7);
					degree[nodes[a]]++;
				}
	}
	/* slots: the most connected nodes first, each on the least used slot its
	neighbours leave free */
	memset(slot_of, -1, SKIN_NODES);
	memset(slot_use, 0, sizeof(slot_use));
	for (i = 0; i < node_count; i++)
	{
		int best = -1, node, other, slot;

		for (node = 0; node < SKIN_NODES; node++)
			if (used[node] && slot_of[node] < 0 && (best < 0 || degree[node] > degree[best]))
				best = node;
		{
			int taken = 0, choice = -1;

			for (other = 0; other < SKIN_NODES; other++)
				if (slot_of[other] >= 0 && (conflict[best][other >> 3] & (1 << (other & 7))))
					taken |= 1 << slot_of[other];
			for (slot = 0; slot < SKIN_SLOTS; slot++)
				if (!(taken & (1 << slot)) && (choice < 0 || slot_use[slot] < slot_use[choice]))
					choice = slot;
			if (choice < 0)
			{
				/* more than 8 nodes share triangles with one another */
				free(list);
				skin_failures++;
				return NULL;
			}
			slot_of[best] = (signed char)choice;
			slot_use[choice]++;
			if (choice + 1 > slots)
				slots = choice + 1;
		}
	}
	/* batches: each triangle into the first whose slots agree with its nodes */
	batch_of = malloc(triangle_count);
	if (!batch_of)
	{
		free(list);
		return NULL;
	}
	for (t = 0; t < triangle_count; t++)
	{
		int nodes[6], n = 0, a, b;

		for (a = 0; a < 3; a++)
		{
			int v = list[3 * t + a];

			nodes[n++] = node0[v];
			if (node1[v] >= 0)
				nodes[n++] = node1[v];
		}
		for (b = 0; b < batch_count; b++)
		{
			for (a = 0; a < n; a++)
			{
				unsigned char held = batches[b].node[(int)slot_of[nodes[a]]];

				if (held != 0xFF && held != nodes[a])
					break;
			}
			if (a == n)
				break;
		}
		if (b == batch_count)
		{
			if (batch_count == 64)
			{
				free(batch_of);
				free(list);
				skin_failures++;
				return NULL;
			}
			memset(batches[b].node, 0xFF, SKIN_SLOTS);
			batch_triangles[b] = 0;
			batch_count++;
		}
		for (a = 0; a < n; a++)
			batches[b].node[(int)slot_of[nodes[a]]] = (unsigned char)nodes[a];
		batch_of[t] = (unsigned char)b;
		batch_triangles[b]++;
	}
	/* one allocation: the part, its batches, their indices */
	{
		int bytes = sizeof(struct skin_part) + batch_count * sizeof(struct skin_batch) + triangle_count * 3 * 2;
		unsigned short *out;
		int b;

		part = malloc(bytes);
		if (!part)
		{
			free(batch_of);
			free(list);
			return NULL;
		}
		skin_bytes += bytes;
		part->bytes = bytes;
		skin_live_bytes += (unsigned long)bytes;
		if (skin_live_bytes > skin_live_peak)
			skin_live_peak = skin_live_bytes;
		part->weights = slots;
		part->stride = (slots <= 4 ? 4 : 8) + 8 + 4 + 12;
		part->batch_count = batch_count;
		part->batches = (struct skin_batch *)(part + 1);
		out = (unsigned short *)(part->batches + batch_count);
		for (b = 0; b < batch_count; b++)
		{
			part->batches[b] = batches[b];
			part->batches[b].indices = out;
			part->batches[b].index_count = 0;
			part->batches[b].radius = -1.0f; /* build 118: its sphere not yet worked out */
			part->batches[b].clusters = NULL; /* build 119: nor its runs' */
			part->batches[b].cluster_count = -1;
			for (t = 0; t < triangle_count; t++)
				if (batch_of[t] == b)
				{
					out[0] = list[3 * t]; out[1] = list[3 * t + 1]; out[2] = list[3 * t + 2];
					out += 3;
					part->batches[b].index_count += 3;
				}
			ge_writeback(part->batches[b].indices, part->batches[b].index_count * 2);
		}
	}
	free(batch_of);
	free(list);
	skinned_parts++;
	skin_batches += batch_count;
	return part;
}

/* build 93: the table forgets what is gone. An entry whose geometry-cache
block has since been loaded with something else (its generation is no longer
any resident block's) or let go is dead: nothing there is converted any
more. When the table is three-quarters full the live entries are kept and
put back (open addressing can't simply delete), and the dead ones' skinning
plans freed. b68-b92 forgot only at a map change, so a long campaign session
churning the cache filled all 2,048 slots and model parts stopped drawing
(code review b89). Entries in the tags (generation 0) are always live */
static int converted_used = 0;
static unsigned long converted_sweeps = 0, converted_forgotten = 0;
/* build 112: sweep sooner. The dead entries' skinning plans stay on the heap
until a sweep: in b108 run 1 the plans made reached 1,160 KB by game time
20,000, and one sweep forgot 1,426 dead entries, so the heap carried up to
about a megabyte of plans nothing would use again while the level's copies
looked for room. The next sweep now comes 256 entries after what the last
one kept, from a quarter of the table up to the three quarters it always
used; a scene whose live parts pass the mark doesn't sweep on every part,
because the mark moves past what was kept. A sweep's cost grows with the
entries it checks, so smaller sweeps cost less each. "sweeplate": at three
quarters only, as before */
#define CONVERTED_SWEEP_STEP 256
static int converted_sweep_mark = CONVERTED_BUFFERS / 4;
static PSP_MAIN_SIDE int converted_sweep_late = -1;
unsigned int psp_geo_generation(const void *ptr);

static void converted_sweep(void)
{
	static const void *keep_address[CONVERTED_BUFFERS];
	static unsigned int keep_generation[CONVERTED_BUFFERS];
	static struct skin_part *keep_skin[CONVERTED_BUFFERS];
	int index, kept = 0;

	for (index = 0; index < CONVERTED_BUFFERS; index++)
	{
		if (!converted[index])
			continue;
		if (psp_geo_generation(converted[index]) == converted_generation[index])
		{
			keep_address[kept] = converted[index];
			keep_generation[kept] = converted_generation[index];
			keep_skin[kept] = skinned[index];
			kept++;
		}
		else
		{
			skin_free(skinned[index]);
			converted_forgotten++;
		}
	}
	memset(converted, 0, sizeof(converted));
	memset(converted_generation, 0, sizeof(converted_generation));
	memset(skinned, 0, sizeof(skinned));
	for (index = 0; index < kept; index++)
	{
		unsigned int slot = ((unsigned int)keep_address[index] >> 4) % CONVERTED_BUFFERS;

		while (converted[slot])
			slot = (slot + 1) % CONVERTED_BUFFERS;
		converted[slot] = keep_address[index];
		converted_generation[slot] = keep_generation[index];
		skinned[slot] = keep_skin[index];
	}
	converted_used = kept;
	converted_sweeps++;
}

/* build 112: model buffers that work marked as the Media Engine's needed
converted (gemesim): converting plans a bending part's skinning on the heap
and sweeps the table, which frees plans, all behind the heap's lock. The
main CPU converts them at the frame's end (psp_ge_frame_end), so they're
ready next frame; the part isn't drawn meanwhile, as for geometry still on
its way */
#define CONVERTS_WANTED 64
static struct
{
	void *vertices;
	const unsigned short *indices;
	int count, uncompressed, index_count, strip, skin, float_only;
} converts_wanted[CONVERTS_WANTED];
static int converts_wanted_count = 0;
static unsigned long converts_wanted_total = 0, converts_wanted_dropped = 0;

/* a converted block's step (0: float records, or not converted) */
static float converted_k(const void *vertices)
{
	unsigned int slot = ((unsigned int)vertices >> 4) % CONVERTED_BUFFERS, tries = 0;

	while (converted[slot] && converted[slot] != vertices && ++tries < CONVERTED_BUFFERS)
		slot = (slot + 1) % CONVERTED_BUFFERS;
	return converted[slot] == vertices ? step_of(converted_step[slot]) : 0.0f;
}

static void convert_want(void *vertices, int count, int uncompressed, const unsigned short *indices, int index_count,
	int strip, int skin)
{
	int k;

	for (k = 0; k < converts_wanted_count && converts_wanted[k].vertices != vertices; k++)
		;
	if (k < converts_wanted_count)
		return;
	if (k >= CONVERTS_WANTED)
	{
		converts_wanted_dropped++;
		return;
	}
	converts_wanted[k].vertices = vertices;
	converts_wanted[k].indices = indices;
	converts_wanted[k].count = count;
	converts_wanted[k].uncompressed = uncompressed;
	converts_wanted[k].index_count = index_count;
	converts_wanted[k].strip = strip;
	converts_wanted[k].skin = skin;
	converts_wanted[k].float_only = convert_float_only;
	converts_wanted_count++;
	converts_wanted_total++;
}

/* ---------- build 116: "vrammodelsN", the model buffers' VRAM copies

big_levers.md section 0, the VRAM swap's second step. b115 [E, gereads]: the
models are 69-88% of what the GE reads from main RAM, and a few small buffers
carry most of it (an 11 KB buffer read 586 KB a frame); ~600 KB of VRAM takes
82-92% of those reads. b115 test 3 [M]: texels from main RAM cost the GE
~1.5 ms a frame, so the textures can give that VRAM up. With the word, the top
N KB of VRAM (psp_tex.c, psp_tex_vram_models) hold copies of the converted
model buffers the GE reads most per byte, and the draw gate (sceGuDrawArray,
above) hands the GE the copy instead. The same bytes: the same pixels.

- A buffer is known from its conversion (model_convert: its address, size and
  the geometry cache's generation of its block). A copy is good while the
  generation holds; the gate drops it when the cache has read something else
  into the block.
- Copies are made at the frame's end, the most-read per byte first, at most
  128 KB a frame; reads halve every 32 frames, so the set follows the view.
  Room a dropped copy used isn't reused for two frames (a list the GE is
  still drawing may read it).
- Never from the Media Engine's side (it can't see VRAM, and the tables are
  the main CPU's): the gate leaves its draws alone.
Off by default: an A/B (a cinematic's GE time, gebusy by kind). */
#define VM_ENTRIES 1024
#define VM_HASH 2048
#define VM_NONE 0xFFFFFFFFu
#define VM_COPY_BUDGET (128 * 1024)
#define VM_QUARANTINE_FRAMES 2
struct vm_entry
{
	unsigned int source, generation, bytes, at, reads, frame, released;
	unsigned int held; /* the copy's own size, kept while it is in quarantine: bytes may already name a new buffer */
	unsigned int level; /* a level stream: valid until released, no geometry-cache generation */
};
static struct vm_entry *vm = NULL;
static unsigned short *vm_hash = NULL, *vm_order = NULL;
static int vm_count = 0;
static unsigned int vm_base = 0, vm_size = 0, vm_frame = 0;
static unsigned long vm_draws = 0, vm_redirects = 0, vm_copied = 0, vm_dropped = 0, vm_full = 0;

/* "mevram": the copies for the Media Engine's draws too (the GE layer
carried out on the ME: gemeon, gemeoverlap, frames in flight). Without it
the ME's side hands the GE every buffer in main RAM, so with the GE layer
on the ME the GE reads all its geometry over the bus the tick misses on
(the overlap runs: draws from a copy 0 of 0). The ME can't touch VRAM, but
it needn't: it only puts the copy's address in the list, and the GE reads
it. The table is the main CPU's, written only between the ME's runs and
written back before each, so on the ME it is read-only: a copy is used when
it is good, nothing is released or moved. The reads the ranking needs go in
a tally only the ME's side writes (an entry's index; the last two words its
draws and redirects), folded in at the frame's end once the ME is done. The
same bytes: the same pixels */
static PSP_MAIN_SIDE int vm_me_on = 0, vm_me_dry = 0;
static PSP_MAIN_SIDE unsigned int *vm_me_tally = NULL;
/* "mevramdry" (a test, off): the ME's reads go in the tally and the
ranking as with mevram, so the frame's end uploads and evicts the same, but
the ME's draws keep reading main RAM. Set against mevram it tells the
upload work's cost to the emulator's speed apart from what the copies draw */
#define VM_TALLY_WORDS (VM_ENTRIES + 16) /* the counters on a line of their own */

/* the entry for a buffer's address, or NULL (adding it when asked) */
static struct vm_entry *vm_find(unsigned int source, int add)
{
	unsigned int h = (source >> 4) & (VM_HASH - 1), probe;

	for (probe = 0; probe < VM_HASH; probe++)
	{
		unsigned short slot = vm_hash[(h + probe) & (VM_HASH - 1)];

		if (!slot)
		{
			if (!add || vm_count >= VM_ENTRIES)
				return NULL;
			vm[vm_count].source = source;
			vm[vm_count].at = VM_NONE;
			vm[vm_count].released = 0;
			vm[vm_count].reads = 0;
			vm[vm_count].level = 0;
			vm_hash[(h + probe) & (VM_HASH - 1)] = (unsigned short)(++vm_count);
			return &vm[vm_count - 1];
		}
		if (vm[slot - 1].source == source)
			return &vm[slot - 1];
	}
	return NULL;
}

/* at a new map or BSP (and the first time): the room psp_tex.c gave, and
empty tables */
static void vm_reset(void)
{
	extern unsigned int psp_tex_vram_models(unsigned int *start);

	vm_size = psp_tex_vram_models(&vm_base);
	vm_on = 0;
	if (!vm_size)
		return;
	if (!vm)
		vm = malloc(VM_ENTRIES * sizeof(struct vm_entry));
	if (!vm_hash)
		vm_hash = malloc(VM_HASH * sizeof(unsigned short));
	if (!vm_order)
		vm_order = malloc(VM_ENTRIES * sizeof(unsigned short));
	if (!vm || !vm_hash || !vm_order)
	{
		platform_log("GE: vrammodels: no room for its tables; off");
		return;
	}
	memset(vm_hash, 0, VM_HASH * sizeof(unsigned short));
	vm_count = 0;
	vm_on = 1;
	vm_me_dry = psp_keep("mevramdry");
	if (psp_keep("mevram") || vm_me_dry)
	{
		if (!vm_me_tally)
			vm_me_tally = memalign(64, VM_TALLY_WORDS * sizeof(unsigned int));
		vm_me_on = vm_me_tally != NULL;
		if (vm_me_tally)
			memset(vm_me_tally, 0, VM_TALLY_WORDS * sizeof(unsigned int));
		else
			platform_log("GE: mevram: no room for its tally; off");
	}
}

/* the Media Engine's side of the gate: the copy's address when it has a
good one, the table left as it is */
static const void * __attribute__((noinline)) vm_redirect_me(const void *vertices, int count)
{
	struct vm_entry *e = vm_find((unsigned int)vertices, 0);

	if (!e)
		return vertices;
	vm_me_tally[VM_ENTRIES]++;
	if (!e->level && psp_geo_generation(vertices) != e->generation)
		return vertices;
	vm_me_tally[e - vm] += (unsigned int)count;
	if (e->at == VM_NONE || e->released)
		return vertices;
	vm_me_tally[VM_ENTRIES + 1]++;
	if (vm_me_dry)
		return vertices;
	return (const void *)(vm_base + e->at);
}

/* at the frame's end, the ME done: what its draws read, as the main CPU's
own draws would have counted it */
static void vm_me_fold(void)
{
	int k;

	for (k = 0; k < vm_count; k++)
		if (vm_me_tally[k])
		{
			vm[k].reads += vm_me_tally[k];
			vm[k].frame = vm_frame;
			vm_me_tally[k] = 0;
		}
	vm_draws += vm_me_tally[VM_ENTRIES];
	vm_redirects += vm_me_tally[VM_ENTRIES + 1];
	vm_me_tally[VM_ENTRIES] = vm_me_tally[VM_ENTRIES + 1] = 0;
}

/* a copy let go: its room comes back after the quarantine */
static void vm_release(struct vm_entry *e)
{
	if (e->at != VM_NONE && !e->released)
	{
		e->released = vm_frame + VM_QUARANTINE_FRAMES + 1;
		vm_dropped++;
	}
}

/* model_convert's buffer, converted: its size and its block's generation */
static void vm_note(void *vertices, unsigned int bytes)
{
	struct vm_entry *e;

	if (vm_on <= 0 || PSP_ME_WORK())
		return;
	e = vm_find((unsigned int)vertices, 1);
	if (!e)
		return;
	if (e->at != VM_NONE && (e->level || e->bytes != bytes || e->generation != psp_geo_generation(vertices)))
		vm_release(e);
	e->bytes = bytes;
	e->generation = psp_geo_generation(vertices);
	e->level = 0;
}

/* "nolevelvram": the level's streams kept out of the copies' room. On, they
share it with the model buffers, ranked by the same reads a byte: the level's
copies are the largest geometry the GE still reads from main RAM (600-760 KB
a frame on a30, b30, c20, d20 [E, levelvram1]), and on c20 and d20 the
models leave 226-365 KB of the room idle. A stream is valid until it is
written again (its origin moved), freed, or moved by the arena; each of
those releases its copy (the quarantine covers a list the GE is still
drawing), so a stale copy is never drawn. The same bytes: the same pixels */
static PSP_MAIN_SIDE int vm_level_on = -1;

/* a level stream just written (built, or its origin moved): its copy, if
any, no longer holds its bytes */
static void vm_level_note(void *vertices, unsigned int bytes)
{
	struct vm_entry *e;

	if (vm_level_on < 0)
		vm_level_on = !psp_keep("nolevelvram");
	if (vm_on <= 0 || !vm_level_on || !vertices || PSP_ME_WORK() ||
		(((unsigned int)vertices & 0x0F000000u) == 0x04000000u))
		return;
	e = vm_find((unsigned int)vertices, 1);
	if (!e)
		return;
	vm_release(e);
	e->bytes = bytes;
	e->generation = 0;
	e->level = 1;
}

/* a level stream let go: its copy goes with it */
static void vm_level_forget(void *vertices)
{
	struct vm_entry *e;

	if (vm_on <= 0 || !vertices)
		return;
	e = vm_find((unsigned int)vertices, 0);
	if (!e || !e->level)
		return;
	vm_release(e);
	e->reads = 0;
	e->bytes = 0;
}

/* a level stream the arena moved (its bytes unchanged): known again at its
new place, copied there afresh */
static void vm_level_move(void *from, void *to)
{
	struct vm_entry *e;
	unsigned int bytes;

	if (vm_on <= 0 || !from)
		return;
	e = vm_find((unsigned int)from, 0);
	if (!e || !e->level || !e->bytes)
		return;
	bytes = e->bytes;
	vm_level_forget(from);
	vm_level_note(to, bytes);
}

/* the gate: the copy's address for a buffer that has a good one */
static const void *vm_redirect(const void *vertices, int count)
{
	struct vm_entry *e;

	if (PSP_ME_WORK() || !vertices)
		return vm_me_on > 0 && vertices ? vm_redirect_me(vertices, count) : vertices;
	e = vm_find((unsigned int)vertices, 0);
	if (!e)
		return vertices;
	vm_draws++;
	if (!e->level && psp_geo_generation(vertices) != e->generation)
	{
		/* the cache has read other bytes into the block: model_convert
		notes it afresh when it converts them */
		vm_release(e);
		e->reads = 0;
		return vertices;
	}
	e->reads += (unsigned int)count;
	e->frame = vm_frame;
	if (e->at == VM_NONE || e->released)
		return vertices;
	vm_redirects++;
	return (const void *)(vm_base + e->at);
}

/* the first gap of the given size in the copies' room, or VM_NONE; copies
whose quarantine is over give their room back first */
static unsigned int vm_place(unsigned int bytes)
{
	unsigned short *order = vm_order;
	int n = 0, k, j;
	unsigned int at = 0;

	for (k = 0; k < vm_count; k++)
	{
		struct vm_entry *e = &vm[k];

		if (e->at == VM_NONE)
			continue;
		if (e->released && vm_frame >= e->released)
		{
			/* no lookup hands out a released copy (both redirects test
			released first), so its last read is in the release's frame at
			the latest, though later lookups still move e->frame */
			unsigned int last = e->released - VM_QUARANTINE_FRAMES - 1;

			psp_ge_show3_reuse(SHOW3_VM, (int)(e->frame - last) < 0 ? e->frame : last);
			e->at = VM_NONE;
			e->released = 0;
			continue;
		}
		/* insertion by place: a few hundred copies at most */
		for (j = n; j > 0 && vm[order[j - 1]].at > e->at; j--)
			order[j] = order[j - 1];
		order[j] = (unsigned short)k;
		n++;
	}
	for (k = 0; k < n; k++)
	{
		struct vm_entry *e = &vm[order[k]];

		if (e->at >= at + bytes)
			return at;
		at = (e->at + e->held + 15) & ~15u;
	}
	return at + bytes <= vm_size ? at : VM_NONE;
}

/* the frame's end: the best buffers without a copy get one, room allowing;
when there's none, the least-read copy not drawn lately makes way */
static void vm_frame_end(void)
{
	unsigned int budget = VM_COPY_BUDGET;
	int k, round;

	if (vm_on <= 0)
		return;
	if (vm_me_on > 0)
		vm_me_fold();
	vm_frame++;
	if ((vm_frame & 31) == 0)
		for (k = 0; k < vm_count; k++)
			vm[k].reads >>= 1;
	for (round = 0; round < 16 && budget; round++)
	{
		struct vm_entry *best = NULL, *worst = NULL;
		unsigned int at;

		/* the most reads a byte among the buffers drawn this frame or the
		last without a copy (cross-multiplied, no division) */
		for (k = 0; k < vm_count; k++)
		{
			struct vm_entry *e = &vm[k];

			if (e->at != VM_NONE || !e->reads || !e->bytes || e->bytes > vm_size || e->bytes > budget ||
				vm_frame - e->frame > 2)
				continue;
			if (!best || (unsigned long long)e->reads * best->bytes > (unsigned long long)best->reads * e->bytes)
				best = e;
		}
		if (!best)
			break;
		at = vm_place(best->bytes);
		if (at == VM_NONE)
		{
			/* the least read a byte of the copies not drawn these 30 frames,
			if it's worth less than the newcomer */
			for (k = 0; k < vm_count; k++)
			{
				struct vm_entry *e = &vm[k];

				if (e->at == VM_NONE || e->released || vm_frame - e->frame < 30)
					continue;
				if (!worst || (unsigned long long)e->reads * worst->bytes < (unsigned long long)worst->reads * e->bytes)
					worst = e;
			}
			if (!worst || (unsigned long long)worst->reads * best->bytes >= (unsigned long long)best->reads * worst->bytes)
			{
				vm_full++;
				break;
			}
			vm_release(worst);
			break; /* its room comes back after the quarantine */
		}
		memcpy((void *)((vm_base + at) | 0x40000000), (const void *)best->source, best->bytes);
		best->at = at;
		best->held = best->bytes;
		best->released = 0;
		budget -= best->bytes;
		vm_copied += best->bytes;
	}
}

/* geshow3check: a frame handed over, with each kind's frame counter as it
records (its last uses are stamped with these) */
static void show3_stamp_out(void)
{
	extern unsigned int psp_tex_frame_now(void), psp_geo_frame_now(void);
	unsigned int *stamp = show3_out_stamp[show3_out++];

	stamp[SHOW3_TEX] = stamp[SHOW3_BIND] = psp_tex_frame_now();
	stamp[SHOW3_GEO] = psp_geo_frame_now();
	stamp[SHOW3_LEVEL] = ge_hot.stream_frame;
	stamp[SHOW3_VM] = vm_frame;
	stamp[SHOW3_LIST] = 0;
}

static void vm_report(void)
{
	unsigned int resident = 0, bytes = 0;
	int k;

	if (vm_on <= 0)
		return;
	for (k = 0; k < vm_count; k++)
		if (vm[k].at != VM_NONE && !vm[k].released)
		{
			resident++;
			bytes += vm[k].bytes;
		}
	platform_log("GE: vrammodels: %u buffers in %u of %u KB of VRAM; draws from a copy %lu of %lu, %lu KB copied, %lu copies let go, %lu times full (last 300 frames; %d buffers known)",
		resident, bytes / 1024, vm_size / 1024, vm_redirects, vm_draws, vm_copied / 1024, vm_dropped, vm_full, vm_count);
	vm_draws = vm_redirects = vm_copied = vm_dropped = vm_full = 0;
}

/* rewrites a model vertex buffer for the GE, once; 0 if there's no room to
remember it. Parts on several nodes are planned for skinning when their
triangles are given (see-through parts aren't: they are queued with one
matrix) */
struct skin_batch;
static void batch_sphere(struct skin_batch *b, const struct skin_part *skin, const void *vertices);
static PSP_MAIN_SIDE int batch_sphere_old = -1;

static int model_convert(void *vertices, int count, int uncompressed, const unsigned short *indices, int index_count,
	int strip, struct skin_part **skin)
{
	unsigned int slot = ((unsigned int)vertices >> 4) % CONVERTED_BUFFERS, tries = 0;
	int index;
	struct skin_part *part = NULL;
	signed char *node0 = NULL, *node1 = NULL;
	unsigned char *weight0 = NULL;

	if (skin)
		*skin = NULL;
	convert_k = 0.0f;
	/* read on the main CPU (psp_ge_model_begin); the ME only reads it */
	if (model14 < 0 && !psp_on_me())
		model14 = psp_keep("model14");
	if (converted_sweep_late < 0)
		converted_sweep_late = psp_keep("sweeplate");
	if (converted_used >= (converted_sweep_late ? CONVERTED_BUFFERS * 3 / 4 : converted_sweep_mark) &&
		!PSP_ME_WORK()) /* build 112: the sweep frees plans: the main CPU's */
	{
		converted_sweep();
		/* build 112: the next mark, past what this sweep kept */
		converted_sweep_mark = converted_used + CONVERTED_SWEEP_STEP;
		if (converted_sweep_mark < CONVERTED_BUFFERS / 4)
			converted_sweep_mark = CONVERTED_BUFFERS / 4;
		if (converted_sweep_mark > CONVERTED_BUFFERS * 3 / 4)
			converted_sweep_mark = CONVERTED_BUFFERS * 3 / 4;
		slot = ((unsigned int)vertices >> 4) % CONVERTED_BUFFERS;
	}
	while (converted[slot] && converted[slot] != vertices)
	{
		slot = (slot + 1) % CONVERTED_BUFFERS;
		if (++tries >= CONVERTED_BUFFERS)
		{
			/* build 89: said once. Entries are only forgotten at a map
			change, and every address a model's block is read to adds one:
			a long session churning the geometry cache (a campaign map) fills
			it, and model parts then stop drawing (review; the real fix,
			keeping this beside the cache's own block, is for the campaign
			work) */
			static int said = 0;

			if (!said)
			{
				said = 1;
				platform_log("GE: the converted-buffer table is FULL (%d addresses since the map loaded): model parts that need converting won't draw until the next map",
					CONVERTED_BUFFERS);
			}
			return 0;
		}
	}
	if (converted[slot])
	{
		unsigned int generation = psp_geo_generation(vertices);

		if (generation == converted_generation[slot])
		{
			if (converted_step[slot] && !skin)
			{
				/* see-through parts read float records on the CPU */
				model14_refused++;
				return 0;
			}
			if (skin)
				*skin = skinned[slot];
			convert_k = step_of(converted_step[slot]);
			return 1;
		}
		if (PSP_ME_WORK())
		{
			convert_want(vertices, count, uncompressed, indices, index_count, strip, skin != NULL);
			return 0;
		}
		/* the cache has read other vertices (or these again) into the block
		since: converted afresh, the old plan let go */
		skin_free(skinned[slot]);
		skinned[slot] = NULL;
		converted_generation[slot] = generation;
		reconverted_count++;
	}
	else
	{
		if (PSP_ME_WORK())
		{
			convert_want(vertices, count, uncompressed, indices, index_count, strip, skin != NULL);
			return 0;
		}
		converted_generation[slot] = psp_geo_generation(vertices);
	}
	if (skin && indices)
	{
		node0 = malloc(count * 3);
		if (node0)
		{
			node1 = node0 + count;
			weight0 = (unsigned char *)(node1 + count);
			part = skin_plan(vertices, count, uncompressed, indices, index_count, strip, node0, node1, weight0);
		}
	}
	if (!converted[slot])
		converted_used++;
	converted[slot] = vertices;
	skinned[slot] = part;
	if (!part)
	{
		/* build 42: a buffer whose vertices hang from several nodes but is
		drawn on one (as see-through parts are, or when its plan failed):
		its far vertices are drawn where the first node puts them */
		int first = -1, nodes = 0, why = !skin ? 1 : !indices ? 2 : 3;

		for (index = 0; index < count; index++)
		{
			int n0, n1;
			float w0;

			vertex_nodes(vertices, index, uncompressed, &n0, &n1, &w0);
			if (first < 0)
				first = n0;
			if (n0 != first || n1 >= 0)
			{
				nodes = 1;
				break;
			}
		}
		if (nodes)
		{
			multi_node_rigid++;
			multi_node_rigid_why[why - 1]++;
			if (multi_node_rigid <= 12)
				platform_log("GE: a buffer of %d vertices on several nodes converted rigid (%s)", count,
					why == 1 ? "see-through" : why == 2 ? "no triangles given" : "its skinning plan failed");
		}
	}
	if (skin)
		*skin = part;
	converted_count++;
	converted_step[slot] = 0;
	if (model14 > 0 && part && !uncompressed && !convert_float_only && count > 0)
	{
		model14_skinned_parts++;
		model14_skinned_vertices += (unsigned long)count;
	}
	if (model14 > 0 && skin && !part && !uncompressed && !convert_float_only && count > 0)
	{
		/* the smallest power of two that keeps every coordinate in 16 bits */
		float largest = 0.0f, k = 1.0f / 16777216.0f;
		int e = -24;

		for (index = 0; index < count; index++)
		{
			const float *p = (const float *)((const unsigned char *)vertices + 32 * index);
			int axis;

			for (axis = 0; axis < 3; axis++)
			{
				float a = p[axis] < 0.0f ? -p[axis] : p[axis];

				if (a > largest)
					largest = a;
			}
		}
		while (largest > 32767.0f * k && e < 24)
		{
			k *= 2.0f;
			e++;
		}
		if (largest <= 32767.0f * k)
		{
			converted_step[slot] = (unsigned char)(e + 128);
			convert_k = k;
			model14_parts++;
			model14_vertices += (unsigned long)count;
		}
	}
	for (index = 0; index < count; index++)
	{
		unsigned char *vertex = (unsigned char *)vertices + 32 * index;
		float position[3], uv[2];
		unsigned int nodes, packed_normal = 0;
		short texcoord[2] = { 0, 0 };

		if (uncompressed)
		{
			/* 68-byte vertices (the sky, and models too big for 16 bits): packed
			down into the same records, in place; record i ends before vertex i
			starts, and vertices before it are already read */
			const unsigned char *source = (const unsigned char *)vertices + 68 * index;
			short node_indices[2];

			memcpy(position, source, 12);
			memcpy(uv, source + 48, 8);
			memcpy(node_indices, source + 56, 4);
			{
				float n[3];

				memcpy(n, source + 12, 12);
				packed_normal = pack_normal(n);
			}
			nodes = (unsigned int)(node_indices[0] >= 0 && node_indices[0] < 85 ? node_indices[0] * 3 : 0);
		}
		else
		{
			memcpy(position, vertex, 12);
			memcpy(&packed_normal, vertex + 12, 4);
			memcpy(texcoord, vertex + 24, 4);
			memcpy(&nodes, vertex + 28, 4);
			uv[0] = texcoord[0] * (1.0f / 32767.0f);
			uv[1] = texcoord[1] * (1.0f / 32767.0f);
		}
		{
			float normal[3];
			int k;

			unpack_normal(packed_normal, normal);
			if (part)
			{
				/* skinned: weights by slot, then as the rigid record */
				unsigned char *record = (unsigned char *)vertices + part->stride * index;
				int uv_at = part->stride - 24;

				memset(record, 0, uv_at);
				if (node1[index] >= 0)
				{
					record[(int)skin_slot[(int)node0[index]]] = weight0[index];
					record[(int)skin_slot[(int)node1[index]]] = (unsigned char)(128 - weight0[index]);
				}
				else
					record[(int)skin_slot[(int)node0[index]]] = 128;
				memcpy(record + uv_at, uv, 8);
				for (k = 0; k < 3; k++)
					record[uv_at + 8 + k] = (unsigned char)(signed char)(normal[k] * 127.0f);
				record[uv_at + 11] = 0;
				memcpy(record + uv_at + 12, position, 12);
			}
			else if (convert_k != 0.0f)
			{
				/* the 14-byte record; record i ends before vertex i + 1 starts */
				unsigned char *record = (unsigned char *)vertices + 14 * index;
				unsigned short u16[2];
				short q[3];
				float inverse = 1.0f / convert_k;

				u16[0] = (unsigned short)(texcoord[0] + 32768);
				u16[1] = (unsigned short)(texcoord[1] + 32768);
				for (k = 0; k < 3; k++)
				{
					float v = position[k] * inverse;
					int r = (int)(v < 0.0f ? v - 0.5f : v + 0.5f);

					q[k] = (short)(r < -32767 ? -32767 : r > 32767 ? 32767 : r);
				}
				memcpy(record, u16, 4);
				for (k = 0; k < 3; k++)
					record[4 + k] = (unsigned char)(signed char)(normal[k] * 127.0f);
				record[7] = (unsigned char)(nodes & 0xFF);
				memcpy(record + 8, q, 6);
			}
			else
			{
				/* 24 bytes: texture coordinate, an 8-bit normal and the first
				node (in the padding byte the GE skips), position (build 22) */
				unsigned char *record = (unsigned char *)vertices + 24 * index;

				memcpy(record, uv, 8);
				for (k = 0; k < 3; k++)
					record[8 + k] = (unsigned char)(signed char)(normal[k] * 127.0f);
				record[11] = (unsigned char)(nodes & 0xFF);
				memcpy(record + 12, position, 12);
			}
		}
	}
	/* each batch's sphere now, on the main CPU, from the records just
	written: the plan is a plain heap block, and the Media Engine's replay,
	which draws the batch near the eye, must write none of it (a line it
	writes back carries a neighbouring chunk's header as it was).
	"batchsphereold": worked out the first time it is drawn near the eye */
	if (batch_sphere_old < 0)
		batch_sphere_old = psp_keep("batchsphereold") != 0;
	if (part && !batch_sphere_old)
	{
		int b;

		for (b = 0; b < part->batch_count; b++)
			batch_sphere(&part->batches[b], part, vertices);
	}
	free(node0);
	{
		int stride = part ? part->stride : convert_k != 0.0f ? 14 : 24;

		ge_writeback(vertices, count * stride);
		vm_note(vertices, (unsigned int)(count * stride)); /* vrammodels: a copy may go to VRAM */
	}
	return 1;
}

/* the change colour (build 31): on the Xbox the lit base map is multiplied
by white blended toward the object's change colour by the multipurpose
map's alpha. Here that is a second pass over the same triangles: the
multipurpose map with a palette worked out per entry (white toward the
colour by the entry's alpha), multiplied into what the first pass drew */
int psp_tex_palette(unsigned int key, const unsigned int **palette, int *count);
int psp_tex_palette16(unsigned int key, const unsigned short **palette, int *count);
static unsigned int change_key = 0, change_color = 0xFFFFFFFF, glow_color = 0, glow_key = 0;
static unsigned long change_passes = 0, glow_passes = 0, glow_mapped = 0;
/* change passes left out because the multipurpose map wasn't resident when
the pass ran (its part drawn without the change colour that frame) */
static unsigned long change_unbound = 0;
static int in_sky;

struct skin_part;
static void model_geometry(void *vertices, const unsigned short *indices, long index_count, int strip,
	struct skin_part *skin, ScePspFMatrix4 *model);

/* An alpha-tested part (leaves, grass, fences) keeps only texels over half
alpha, so its edges are hard and a far plant whose mips thin out shimmers as
it moves. "foliagetwopass" (keep.txt, off) draws such a part a second time:
blended by its alpha, only where it is nearer than what is there (GREATER:
pass 1's own texels are equal, so they are left alone) and without writing
depth, so the texels pass 1 threw away fade the edge in. It costs a second
model_geometry for every alpha-tested part, and the faded edge is drawn in
object order, so something drawn later behind it covers it. A look change
for an A/B */
static int foliage_two_pass = -1;
static unsigned long foliage_second_passes = 0;

static void foliage_soft_pass(void *vertices, const unsigned short *indices, long index_count, int strip,
	struct skin_part *skin, ScePspFMatrix4 *model)
{
	sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
	sceGuDepthFunc(GU_GREATER);
	sceGuDepthMask(GU_TRUE);
	ge_blend(1);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	model_geometry(vertices, indices, index_count, strip, skin, model);
	ge_blend(0);
	sceGuDepthMask(GU_FALSE);
	sceGuDepthFunc(GU_GEQUAL);
	sceGuAlphaFunc(GU_GREATER, 0x7F, 0xFF);
	foliage_second_passes++;
}

/* a rigid part's triangles cut at the near plane or the guard band, as GE
vertices in the part's space (drawn by model_geometry after the part's own
list, in every pass). Their floats are staged in the scratchpad: written and
read back for every model part cut at the near plane, and the emulator's
cache model counted 0.8 M misses on them over b30's beach fight when they
were in main memory */
static PSP_MAIN_SIDE float (*model_cut)[CORNER_FLOATS];

void *psp_scratch_take(unsigned int bytes, const char *what); /* psp_scratch.c */
/* at start, with keep.txt read: the cutting's words, and the scratchpad's buffer */
static void cut_take(void)
{
	cut_stream = !psp_keep("cutstreamoff");
	cut_less = cut_stream && !psp_keep("cutlessoff");
	guard_wide = cut_stream && psp_keep("guardwide");
	{
		extern int psp_keep_number(const char *prefix, int fallback);

		cut_less_mask = psp_keep_number("cutlessmask", -1);
		if (cut_less_mask != -1)
			platform_log("GE: cutlessmask%d: off-screen cuts left out for those kinds only", cut_less_mask);
	}
	cut_count_on = psp_keep("cutcount");
	{
		extern int psp_keep_number(const char *prefix, int fallback);

		far_cut = !psp_keep("farcutoff");
		if (!far_cut)
			platform_log("GE: farcutoff: level triangles across the far clip drawn whole, as before");
	}
	if (!cut_stream)
		platform_log("GE: cutstreamoff: cut pieces past the fixed buffers are lost, pieces off the screen drawn");
	if (!cut_less)
		platform_log("GE: cutlessoff: triangles and pieces wholly off the screen cut and drawn, as before b130");
	if (guard_wide)
		platform_log("GE: guardwide: the guard planes at %d pixels from the screen's centre", (int)GUARD_PIXELS);
	model_cut = psp_scratch_take(sizeof(float[MAXIMUM_MODEL_CUT_CORNERS][CORNER_FLOATS]), "the models' cut corners");
}
static int model_cut_count = 0;
static void *model_cut_vertices = NULL;
static int model_drop = -1;

static unsigned int abgr_from_rgb(float const *rgb)
{
	int r = (int)(rgb[0] * 255.0f + 0.5f), g = (int)(rgb[1] * 255.0f + 0.5f), b = (int)(rgb[2] * 255.0f + 0.5f);

	return 0xFF000000 | (PSP_CLAMP_255(b) << 16) | (PSP_CLAMP_255(g) << 8) | PSP_CLAMP_255(r);
}

/* the model part's multipurpose map and the two colours it masks: the change
colour (by its blue) and the self-illumination (by its green; build 31) */
/* the replay binds the multipurpose map for a change colour that isn't
white, or for a glow without its glow map; the glow map for a glow
(model_lighting's passes): a part with neither binds nothing of these */
static void multipurpose_pretouch(unsigned int multipurpose_key, unsigned int glow_map_key, float const *change,
	float const *glow)
{
	unsigned int glow_rgb = abgr_from_rgb(glow) & 0xFFFFFF;
	int mapped = glow_rgb && glow_map_key && psp_tex_has(glow_map_key);
	int wanted_change = abgr_from_rgb(change) != 0xFFFFFFFF || (glow_rgb && !mapped);

	if (pretouch_all || wanted_change)
		GE_PRETOUCH(multipurpose_key, TXPT_CHANGE);
	else
		GE_PRETOUCH_PASSED(multipurpose_key, TXPT_CHANGE);
	if (pretouch_all || mapped)
		GE_PRETOUCH(glow_map_key, TXPT_GLOW);
	else
		GE_PRETOUCH_PASSED(glow_map_key, TXPT_GLOW);
}

void psp_ge_model_multipurpose(unsigned int multipurpose_key, unsigned int glow_map_key, float const *change,
	float const *glow)
{
	GE_BENCH_SKIP();
	if (PRETOUCHING())
		multipurpose_pretouch(multipurpose_key, glow_map_key, change, glow);
	GE_DEFER(defer_model_multipurpose(multipurpose_key, glow_map_key, change, glow));
	change_key = multipurpose_key;
	glow_key = glow_map_key;
	change_color = abgr_from_rgb(change);
	glow_color = abgr_from_rgb(glow) & 0xFFFFFF;
}

/* ---------- "rso": a still object's records sent again

A model's records (its begin, shape and effect, and each part's multipurpose
and draw) are made from render_model's inputs alone: what the camera changes
is worked out as they are carried out (to_camera, the near plane, the size
floor, the fog), except the level of detail, which is in the key. So an
object drawn with the same inputs as before sends the records it sent then,
and render_model's walk is skipped. The key (those inputs, byte for byte) is
built in models.c (psp_fixups.py, FIXES_RSO). What the walk does besides
writing records is done again here: each texture the records bind is
pretouched, and each part's geometry is locked again, which must give the
addresses the records hold (else the walk runs). A model whose walk reads the
time (a glow), draws a see-through part, takes the Xbox path for a part
(psp_rso_volatile) or writes any other kind of record isn't kept; nor, in
models.c, one with point lights, an effect, or a debug view on. A part's
reflection faded by the camera's distance is kept with its fade's inputs and
answer, and the fade is worked out again before a send: the records go only
if every fraction comes out the same, bit for bit.

The kept records are in one block of free user RAM outside the heap, never
the heap: "rsokbN" KB (RSO_KB by default), halved until RSO_LEAVES_FREE stays
free there. An entry is a header, the key, each faded reflection's cutoff,
falloff and fraction, each part's two buffers and the records. Room is made
only when an entry doesn't fit (rso_make_room): the entries sent stay, so
the room goes to what is sent again rather than to the first come. The key isn't built in a struct: models.c compares
the inputs in place with the kept key, field by field, only the bytes the
walk reads, and writes the key straight into the entry once a walk is kept.
An object whose inputs changed RSO_CHANGES times running without a send (a
moving or flickering object) rests for RSO_REST frames: a header-only entry,
walked and not kept, so the room goes to objects that are sent again.
"rsocheck": the walk runs anyway and its records are
compared with the kept ones word by word (the begin's node pointer only as
set or not: the replay reads only that) */
#define RSO_KB 48
#define RSO_LEAVES_FREE (80 * 1024)
#define RSO_SLOTS 512
#define RSO_MAX_PARTS 32
#define RSO_CHANGES 2
#define RSO_REST 64
#define RSO_MAX_FADES 8
enum { RSO_WORDS, RSO_OBJECT, RSO_USED, RSO_KEY_WORDS, RSO_FADES, RSO_PARTS, RSO_RECORD_WORDS, RSO_RECORDS, RSO_CHANGED,
	RSO_REST_UNTIL, RSO_SENT, RSO_HEADER };
/* an entry: the header, the key, each camera-faded reflection's cutoff,
falloff and fraction, each part's two buffers, the records */
#define RSO_FADE_AT(entry) ((entry) + RSO_HEADER + (entry)[RSO_KEY_WORDS])
#define RSO_BUFFERS_AT(entry) (RSO_FADE_AT(entry) + 3 * (entry)[RSO_FADES])
#define RSO_RECORDS_AT(entry) (RSO_BUFFERS_AT(entry) + 2 * (entry)[RSO_PARTS])
struct vertex_buffer;
struct triangle_buffer;
void *psp_vertex_lock(const struct vertex_buffer *buffer);
void *psp_index_lock(const struct triangle_buffer *buffer);
/* read by the fixups: whether to offer objects, every type or scenery only,
the object offered (NONE, -1, none), and the walk's marks */
PSP_MAIN_SIDE int psp_rso_on = 0, psp_rso_all = 0;
PSP_MAIN_SIDE long psp_rso_object = -1;
PSP_MAIN_SIDE int psp_rso_volatile = 0, psp_rso_capturing = 0;
/* models.c: the key's field where a compare found the inputs changed (0 the
header, 1 regions, 2 ambient, 3 distant lights, 4 reflection tint, 5 centroid,
6 radius, 7 change colours, 8 node matrices, 9 the length; 10, here: a
camera-faded reflection's fraction) */
PSP_MAIN_SIDE int psp_rso_field = 0;
static PSP_MAIN_SIDE int rso_check = 0, rso_room_tried = 0;
static PSP_MAIN_SIDE unsigned int *rso_pool = NULL, rso_pool_words = 0, rso_end = 0, rso_entries = 0;
static PSP_MAIN_SIDE unsigned int rso_slot_entry[RSO_SLOTS]; /* an entry's offset + 1; 0 none */
static PSP_MAIN_SIDE long rso_slot_object[RSO_SLOTS];
static PSP_MAIN_SIDE unsigned long rso_compacted = 0; /* the frame last counted */
/* the walk being kept */
static PSP_MAIN_SIDE long rso_cap_object;
static PSP_MAIN_SIDE unsigned int rso_found, rso_cap_changes, rso_cap_start, rso_cap_parts, rso_cap_checked, rso_cap_fades;
static PSP_MAIN_SIDE float rso_cap_fade[RSO_MAX_FADES * 3];
static PSP_MAIN_SIDE unsigned long rso_last_compact = 0;
static PSP_MAIN_SIDE unsigned long rso_cap_resets, rso_cap_records;
static PSP_MAIN_SIDE defer_word *rso_cap_base;
static PSP_MAIN_SIDE const void *rso_cap_buffers[RSO_MAX_PARTS * 2];
/* since the last log line */
static PSP_MAIN_SIDE unsigned long rso_frames, rso_asked_count, rso_sent, rso_sent_words, rso_kept, rso_new_key,
	rso_no_room, rso_volatile_count[5], rso_broken, rso_kinds, rso_moved, rso_compared, rso_differ, rso_differ_words,
	rso_resting, rso_kind_count[32], rso_field_count[11], rso_compactions;
static PSP_MAIN_SIDE int rso_differ_kind = -1, rso_differ_word = -1;

static unsigned int rso_slot(long object_index)
{
	unsigned int slot = ((unsigned int)object_index * 2654435761u) >> 23;

	while (rso_slot_entry[slot] && rso_slot_object[slot] != object_index)
		slot = (slot + 1) & (RSO_SLOTS - 1);
	return slot;
}

/* at a new map or BSP: the buffers the entries name are gone */
static void rso_forget(void)
{
	memset(rso_slot_entry, 0, sizeof(rso_slot_entry));
	rso_end = rso_entries = 0;
	psp_rso_capturing = 0;
}

static void rso_log(void)
{
	float frames = (float)rso_frames;
	int kind, top_kind = 0;

	for (kind = 1; kind < 32; kind++)
		if (rso_kind_count[kind] > rso_kind_count[top_kind])
			top_kind = kind;
	platform_log("GE: rso: a frame (%lu frames): offered %.1f, sent again %.1f (%.0f words), walked and kept %.1f; "
		"not kept: inputs changed %.1f, a glow that follows the time %.1f, the Xbox path %.1f, a see-through part %.1f, "
		"too many parts %.1f, too many camera-faded reflections %.1f, other records %.1f (most: kind %d, %.1f), "
		"the buffer started over %.1f, no room %.1f; geometry moved %.1f; resting (inputs that keep changing) %.1f; "
		"%u entries in %u of %u KB, made room %lu times",
		rso_frames, rso_asked_count / frames, rso_sent / frames, rso_sent_words / frames, rso_kept / frames,
		rso_new_key / frames, rso_volatile_count[0] / frames, rso_volatile_count[1] / frames,
		rso_volatile_count[2] / frames, rso_volatile_count[3] / frames, rso_volatile_count[4] / frames, rso_kinds / frames,
		top_kind, rso_kind_count[top_kind] / frames, rso_broken / frames, rso_no_room / frames,
		rso_moved / frames, rso_resting / frames, rso_entries, (unsigned int)(rso_end * 4 / 1024),
		(unsigned int)(rso_pool_words * 4 / 1024), rso_compactions);
	platform_log("GE: rso: inputs changed, by the key's first field that differs, a frame: header %.1f, regions %.1f, "
		"ambient %.1f, distant lights %.1f, reflection tint %.1f, centroid %.1f, radius %.1f, "
		"change colours %.1f, node matrices %.1f, length %.1f, a reflection's fade %.1f",
		rso_field_count[0] / frames, rso_field_count[1] / frames, rso_field_count[2] / frames, rso_field_count[3] / frames,
		rso_field_count[4] / frames, rso_field_count[5] / frames, rso_field_count[6] / frames,
		rso_field_count[7] / frames, rso_field_count[8] / frames, rso_field_count[9] / frames, rso_field_count[10] / frames);
	if (rso_check)
		platform_log("GE: rsocheck: %lu walks compared with the kept records, %lu differ (%lu words; the first in a record of kind %d, word %d)",
			rso_compared, rso_differ, rso_differ_words, rso_differ_kind, rso_differ_word);
	rso_frames = rso_asked_count = rso_sent = rso_sent_words = rso_kept = rso_new_key = rso_no_room =
		rso_broken = rso_kinds = rso_moved = rso_resting = rso_compactions = 0;
	memset(rso_volatile_count, 0, sizeof(rso_volatile_count));
	memset(rso_kind_count, 0, sizeof(rso_kind_count));
	memset(rso_field_count, 0, sizeof(rso_field_count));
}

/* room made when an entry doesn't fit, at most once a frame: entries unused
for two frames, and entries kept but never sent though their object has
been drawn since, are dropped and the rest moved down. An entry is never stale by age (a send locks
its geometry again and checks the addresses, and its key is compared whole),
so one that stays is sent again whenever its object is drawn as before; and
the ones sent stay ahead of new walks, which are kept only into the room
left (keep the most sent, not the first come). Offsets into the pool held
across the call (rso_found, rso_cap_checked) are void after it, so it runs
only where none is held */
static void rso_make_room(void)
{
	unsigned int from = 0, to = 0;

	if (rso_last_compact == rso_stamp)
		return;
	rso_last_compact = rso_stamp;
	rso_compactions++;
	memset(rso_slot_entry, 0, sizeof(rso_slot_entry));
	rso_entries = 0;
	while (from < rso_end)
	{
		unsigned int words = rso_pool[from + RSO_WORDS];
		unsigned int used = rso_pool[from + RSO_USED];

		if (rso_stamp - used <= 2 && (rso_pool[from + RSO_SENT] || rso_stamp - used <= 1 || !rso_pool[from + RSO_KEY_WORDS]))
		{
			unsigned int slot;

			if (to != from)
				memmove(rso_pool + to, rso_pool + from, words * sizeof(unsigned int));
			slot = rso_slot((long)rso_pool[to + RSO_OBJECT]);
			rso_slot_entry[slot] = to + 1;
			rso_slot_object[slot] = (long)rso_pool[to + RSO_OBJECT];
			rso_entries++;
			to += words;
		}
		from += words;
	}
	rso_end = to;
}

/* words at the pool's end for an entry, room made once if need be: its offset,
or -1 if it doesn't fit */
static int rso_take(unsigned int words)
{
	unsigned int at;

	if (rso_end + words > rso_pool_words || rso_entries >= RSO_SLOTS / 2)
		rso_make_room();
	if (rso_end + words > rso_pool_words || rso_entries >= RSO_SLOTS / 2)
		return -1;
	at = rso_end;
	rso_end += words;
	rso_entries++;
	return (int)at;
}

/* the block outside the heap, once */
static int rso_room(void)
{
	extern int psp_keep_number(const char *prefix, int fallback);
	SceUID block = -1;
	int kb;

	if (rso_pool)
		return 1;
	if (rso_room_tried)
		return 0;
	rso_room_tried = 1;
	kb = psp_keep_number("rsokb", RSO_KB);
	kb = kb < 8 ? 8 : kb > 256 ? 256 : kb;
	while (kb >= 8 && ((int)sceKernelTotalFreeMemSize() < kb * 1024 + 64 + RSO_LEAVES_FREE ||
		(int)sceKernelMaxFreeMemSize() < kb * 1024 + 64))
		kb /= 2;
	if (kb >= 8)
		block = sceKernelAllocPartitionMemory(2, "halo kept records", PSP_SMEM_High, kb * 1024 + 64, NULL);
	if (block < 0)
	{
		platform_log("GE: rso: no room outside the heap for the kept records (%d KB free there): off",
			(int)(sceKernelTotalFreeMemSize() / 1024));
		psp_rso_on = 0;
		return 0;
	}
	rso_pool = (unsigned int *)(((unsigned int)sceKernelGetBlockHeadAddr(block) + 63) & ~63u);
	rso_pool_words = (unsigned int)kb * 1024 / sizeof(unsigned int);
	platform_log("GE: rso%s: still objects' records sent again (%s), %d KB of free user RAM outside the heap; then %d KB free there",
		rso_check ? "check: walked anyway, and compared" : "", psp_rso_all ? "every type" : "scenery", kb,
		(int)(sceKernelTotalFreeMemSize() / 1024));
	return 1;
}

/* a kept entry's records into the buffer, after its geometry is locked again
where they point and its textures are pretouched; 0 (nothing written) if a
part's geometry moved or the records don't fit */
static int rso_send(unsigned int *entry)
{
	unsigned int parts = entry[RSO_PARTS], words = entry[RSO_RECORD_WORDS];
	const void *const *buffers = (const void *const *)RSO_BUFFERS_AT(entry);
	defer_word *record = (defer_word *)RSO_RECORDS_AT(entry), *at, *end = record + words;
	unsigned int part = 0;

	if (words > DEFER_LARGEST)
		return 0;
	for (at = record; at < end; at += at->u >> 8)
		if ((at->u & 0xFF) == DEFER_DRAW_MODEL)
		{
			if (part >= parts || psp_vertex_lock((const struct vertex_buffer *)buffers[2 * part]) != at[1].p ||
				psp_index_lock((const struct triangle_buffer *)buffers[2 * part + 1]) != at[4].p)
			{
				rso_moved++;
				return 0;
			}
			part++;
		}
	if (PRETOUCHING())
		for (at = record; at < end; at += at->u >> 8)
		{
			if ((at->u & 0xFF) == DEFER_MODEL_MULTIPURPOSE)
				multipurpose_pretouch(at[1].u, at[2].u, &at[4].f, &at[8].f);
			else if ((at->u & 0xFF) == DEFER_DRAW_MODEL)
				pretouch(at[8].u, TXPT_MODEL_BASE, 0);
		}
	/* room, as defer_take makes it */
	if (inflight_room_away)
		inflight_room_park();
	if (inflight_pending)
		inflight_sync();
	if (defer_used + words > defer_limit)
	{
		defer_drains_full++;
		if (inflight_on)
			inflight_part_words += defer_used;
		defer_drain();
	}
	if (!defer_used && inflight_room)
		(void)psp_tex_inflight_room(&inflight_room_generation);
	if (chunk_frame && !chunk_stopped && defer_used - chunk_published >= chunk_words)
		chunk_piece();
	memcpy(defer_base + defer_used, record, words * sizeof(defer_word));
	defer_used += words;
	defer_records += entry[RSO_RECORDS];
	rso_sent++;
	rso_sent_words += words;
	return 1;
}

/* models.c, for an object it can key: 0 if no records can be kept now;
else 1, with *kept_words the words of the object's kept key and its address
returned in *kept (NULL if none), for models.c to compare in place */
int psp_rso_find(long object_index, const unsigned int **kept, unsigned int *kept_words)
{
	unsigned int slot;

	*kept = NULL;
	*kept_words = 0;
	psp_rso_capturing = 0;
	rso_found = 0;
	if (!DEFER_RECORDING() || defer_paused || me_overlap || !rso_room())
		return 0;
	if (rso_compacted != rso_stamp)
	{
		rso_compacted = rso_stamp;
		if (++rso_frames >= 600)
			rso_log();
	}
	rso_asked_count++;
	slot = rso_slot(object_index);
	if (rso_slot_entry[slot])
	{
		unsigned int *entry = rso_pool + rso_slot_entry[slot] - 1;

		rso_found = rso_slot_entry[slot];
		if (entry[RSO_KEY_WORDS])
		{
			*kept = entry + RSO_HEADER;
			*kept_words = entry[RSO_KEY_WORDS];
		}
	}
	return 1;
}

/* a resting entry: the header alone, in place of the object's kept one */
static void rso_rest(long object_index, unsigned int *entry)
{
	unsigned int slot = rso_slot(object_index);
	int at;

	/* the slot keeps the old entry until the new one is in: clearing it
	would break the probe of every object placed past it */
	entry[RSO_USED] = 0; /* dropped when room is next made */
	if ((at = rso_take(RSO_HEADER)) < 0)
		return;
	entry = rso_pool + at;
	memset(entry, 0, RSO_HEADER * sizeof(unsigned int));
	entry[RSO_WORDS] = RSO_HEADER;
	entry[RSO_OBJECT] = (unsigned int)object_index;
	entry[RSO_USED] = rso_stamp;
	entry[RSO_REST_UNTIL] = rso_stamp + RSO_REST;
	slot = rso_slot(object_index);
	rso_slot_entry[slot] = (unsigned int)at + 1;
	rso_slot_object[slot] = object_index;
}

/* the reflection's fade by the camera's distance (rasterizer.c's model
draw); while a walk is being kept, its inputs and answer are noted, for
psp_rso_reuse to work out again before the walk's records are sent */
static float rso_fraction(float distance, float cutoff, float falloff)
{
	float fraction = (distance - cutoff) / (falloff - cutoff);

	return fraction < 0.0f ? 0.0f : fraction > 1.0f ? 1.0f : fraction;
}

float psp_rso_fade(float distance, float cutoff, float falloff)
{
	float fraction = rso_fraction(distance, cutoff, falloff);

	if (psp_rso_capturing)
	{
		if (rso_cap_fades < RSO_MAX_FADES)
		{
			rso_cap_fade[3 * rso_cap_fades] = cutoff;
			rso_cap_fade[3 * rso_cap_fades + 1] = falloff;
			rso_cap_fade[3 * rso_cap_fades + 2] = fraction;
			rso_cap_fades++;
		}
		else
			psp_rso_volatile |= 16;
	}
	return fraction;
}

/* whether each of a kept entry's faded reflections works out to the fraction
its records hold, at the model's distance now (bit for bit) */
static int rso_fades_same(const unsigned int *entry, const float *centroid)
{
	extern float psp_model_distance_along_view(const float *centroid);
	const float *fade = (const float *)RSO_FADE_AT(entry);
	unsigned int count = entry[RSO_FADES], i;
	float distance;

	if (!count)
		return 1;
	distance = psp_model_distance_along_view(centroid);
	for (i = 0; i < count; i++, fade += 3)
	{
		union { float f; unsigned int u; } now, kept;

		now.f = rso_fraction(distance, fade[0], fade[1]);
		kept.f = fade[2];
		if (now.u != kept.u)
			return 0;
	}
	return 1;
}

/* models.c, after psp_rso_find said 1, with whether the inputs equal the kept
key and the model's centroid (for the faded reflections): 1 if the kept
records were sent (render_model's walk is skipped); 0 if the walk that
follows is to be kept (psp_rso_capture_end); -1 if it is walked and not kept
(the object rests) */
int psp_rso_reuse(long object_index, int same, const float *centroid)
{
	unsigned int *entry = rso_found ? rso_pool + rso_found - 1 : NULL;

	rso_cap_checked = 0;
	rso_cap_changes = 0;
	if (entry && same && !rso_fades_same(entry, centroid))
	{
		same = 0;
		psp_rso_field = 10;
	}
	if (entry && !entry[RSO_KEY_WORDS])
	{
		if ((int)(entry[RSO_REST_UNTIL] - rso_stamp) > 0)
		{
			entry[RSO_USED] = rso_stamp;
			rso_resting++;
			return -1;
		}
	}
	else if (entry && same)
	{
		if (rso_check)
			rso_cap_checked = rso_found;
		else if (rso_send(entry))
		{
			entry[RSO_USED] = rso_stamp;
			entry[RSO_CHANGED] = 0;
			if (entry[RSO_SENT] < 0xFFFF)
				entry[RSO_SENT]++;
			return 1;
		}
	}
	else if (entry)
	{
		rso_new_key++;
		rso_field_count[(unsigned int)psp_rso_field < 11 ? psp_rso_field : 9]++;
		rso_cap_changes = entry[RSO_CHANGED] + 1;
		if (rso_cap_changes >= RSO_CHANGES)
		{
			rso_rest(object_index, entry);
			rso_resting++;
			return -1;
		}
	}
	psp_rso_volatile = 0;
	psp_rso_capturing = 1;
	rso_cap_object = object_index;
	rso_cap_parts = 0;
	rso_cap_fades = 0;
	rso_cap_resets = defer_resets;
	rso_cap_records = defer_records;
	rso_cap_start = defer_used;
	rso_cap_base = defer_base;
	return 0;
}

/* psp_model_draw, as a kept walk's part is drawn: the buffers it locked */
void psp_rso_geometry(const void *vertex_buffer, const void *triangle_buffer)
{
	if (!psp_rso_capturing)
		return;
	if (rso_cap_parts >= RSO_MAX_PARTS)
	{
		psp_rso_volatile |= 8;
		return;
	}
	rso_cap_buffers[2 * rso_cap_parts] = vertex_buffer;
	rso_cap_buffers[2 * rso_cap_parts + 1] = triangle_buffer;
	rso_cap_parts++;
}

/* rsocheck: the walk's records (and its fades and buffers) against the kept
ones */
static void rso_compare(const unsigned int *kept, const defer_word *record, unsigned int words, unsigned int parts)
{
	const defer_word *old = (const defer_word *)RSO_RECORDS_AT(kept), *at;
	unsigned long differ = 0;

	rso_compared++;
	if (kept[RSO_RECORD_WORDS] != words || kept[RSO_PARTS] != parts || kept[RSO_FADES] != rso_cap_fades ||
		memcmp(RSO_FADE_AT(kept), rso_cap_fade, 3 * rso_cap_fades * sizeof(float)) ||
		memcmp(RSO_BUFFERS_AT(kept), rso_cap_buffers, 2 * parts * sizeof(void *)))
		differ = words + 1;
	else
		for (at = record; at < record + words; at += at->u >> 8)
		{
			unsigned int count = at->u >> 8, i, offset = (unsigned int)(at - record);

			for (i = 0; i < count; i++)
			{
				unsigned int a = at[i].u, b = old[offset + i].u;

				if ((at->u & 0xFF) == DEFER_MODEL_BEGIN && i == 1)
				{
					a = a != 0;
					b = b != 0;
				}
				if (a != b)
				{
					differ++;
					if (rso_differ_kind < 0)
					{
						rso_differ_kind = (int)(at->u & 0xFF);
						rso_differ_word = (int)i;
					}
				}
			}
		}
	if (differ)
	{
		rso_differ++;
		rso_differ_words += differ;
	}
}

/* models.c, after the walk: its records kept, if they are all a model's own
and nothing came between them; the address for models.c to write the key's
key_words words at, or NULL if the walk isn't kept */
unsigned int *psp_rso_capture_end(unsigned int key_words)
{
	defer_word *record, *at, *end;
	unsigned int words, parts = 0, total, slot, *entry = NULL;
	unsigned long records = 0;

	if (!psp_rso_capturing)
		return NULL;
	psp_rso_capturing = 0;
	if (psp_rso_volatile)
	{
		unsigned int reason;

		/* the first reason only, so the counts add up to the objects not kept */
		for (reason = 0; reason < 4 && !(psp_rso_volatile & (1 << reason)); reason++)
			;
		rso_volatile_count[reason]++;
		return NULL;
	}
	if (!DEFER_RECORDING() || defer_paused || defer_resets != rso_cap_resets || defer_base != rso_cap_base ||
		defer_used <= rso_cap_start)
	{
		rso_broken++;
		return NULL;
	}
	record = defer_base + rso_cap_start;
	end = defer_base + defer_used;
	words = defer_used - rso_cap_start;
	for (at = record; at < end; at += at->u >> 8)
	{
		int kind = (int)(at->u & 0xFF);

		records++;
		if (!(at->u >> 8))
		{
			rso_broken++;
			return NULL;
		}
		if (kind == DEFER_DRAW_MODEL)
			parts++;
		else if (kind == DEFER_MODEL_MULTIPURPOSE ? !at[3].u || !at[7].u :
			kind != DEFER_MODEL_BEGIN && kind != DEFER_MODEL_SHAPE && kind != DEFER_MODEL_EFFECT &&
			kind != DEFER_MODEL_REFLECTION)
		{
			rso_kinds++;
			rso_kind_count[kind & 31]++;
			return NULL;
		}
	}
	if (at != end || records != defer_records - rso_cap_records || parts != rso_cap_parts)
	{
		rso_broken++;
		return NULL;
	}
	if (rso_cap_checked)
		rso_compare(rso_pool + rso_cap_checked - 1, record, words, parts);
	total = RSO_HEADER + key_words + 3 * rso_cap_fades + 2 * parts + words;
	slot = rso_slot(rso_cap_object);
	if (rso_slot_entry[slot])
	{
		entry = rso_pool + rso_slot_entry[slot] - 1;
		if (entry[RSO_WORDS] != total)
		{
			entry[RSO_USED] = 0; /* dropped when room is next made */
			entry = NULL;
		}
	}
	if (!entry)
	{
		int offset = rso_take(total);

		if (offset < 0)
		{
			rso_no_room++;
			return NULL;
		}
		entry = rso_pool + offset;
		slot = rso_slot(rso_cap_object);
		rso_slot_entry[slot] = (unsigned int)offset + 1;
		rso_slot_object[slot] = rso_cap_object;
	}
	entry[RSO_WORDS] = total;
	entry[RSO_OBJECT] = (unsigned int)rso_cap_object;
	entry[RSO_USED] = rso_stamp;
	entry[RSO_KEY_WORDS] = key_words;
	entry[RSO_FADES] = rso_cap_fades;
	entry[RSO_PARTS] = parts;
	entry[RSO_RECORD_WORDS] = words;
	entry[RSO_RECORDS] = (unsigned int)records;
	entry[RSO_CHANGED] = rso_cap_changes;
	entry[RSO_REST_UNTIL] = 0;
	entry[RSO_SENT] = 0;
	memcpy(RSO_FADE_AT(entry), rso_cap_fade, 3 * rso_cap_fades * sizeof(float));
	memcpy(RSO_BUFFERS_AT(entry), rso_cap_buffers, 2 * parts * sizeof(void *));
	memcpy(RSO_RECORDS_AT(entry), record, words * sizeof(defer_word));
	rso_kept++;
	return entry + RSO_HEADER;
}

/* each window's start, as it is written down: the keep words asked once.
The stamp that ages the entries moves at each frame's end (psp_ge_frame_end),
not here: a frame begins several windows, so a per-window stamp aged every
entry out before the next frame could send it again */
static void rso_window(void)
{
	if (rso_asked < 0)
	{
		rso_check = psp_keep("rsocheck") != 0;
		psp_rso_all = psp_keep("rsoall") != 0;
#ifdef PSP_MULTIPLAYER
		rso_asked = rso_check || psp_rso_all || psp_keep("rso");
#else
		/* on by default in the campaign: exact (play and both seeds as
		without it on four maps, its check counting no difference) and a
		gain on every map in the cache model; "rsooff" turns it off */
		rso_asked = rso_check || psp_rso_all || !psp_keep("rsooff");
#endif
		psp_rso_on = rso_asked;
	}
}

/* the part's detail map on the main menu: its key (0 none) and its scale on
the part's texture coordinates (psp_fixups.py: soso shaders, ui.map only) */
static unsigned int model_detail_key = 0;
static float model_detail_u = 1.0f, model_detail_v = 1.0f;

void psp_ge_model_detail(unsigned int detail_key, float u_scale, float v_scale)
{
	GE_BENCH_SKIP();
	GE_PRETOUCH(detail_key, TXPT_MODEL_DETAIL);
	GE_DEFER(defer_model_detail(detail_key, u_scale, v_scale));
	model_detail_key = detail_key;
	model_detail_u = u_scale;
	model_detail_v = v_scale;
}

/* A model shader's reflection: on the Xbox its cube map, times the
multipurpose map's red (the specular mask) and the tint and brightness the
vertex program gives each vertex, is added over the lit base (the model
pixel shader's last stage, rasterizer_xbox_models.c), so a shiny part keeps
its sheen in shade, where the base x the lighting goes dark. The GE has no
cube maps and one texture a pass. A pass of its own for every shiny part
costs too much, so the reflection's average is folded into the part's
ambient light: the base x (lighting + reflection / the base's average)
matches base x lighting + reflection on average, at no cost. The shader's
side (brightness, tints, the distance fade, the lighting's reflection tint)
comes from psp_fixups.py; the cube map's average from the pack (an 8 x 8
patch of it under its key, texpack.py), else REFLECTION_CUBE_DEFAULT.
"noreflection": none, as before */
#define REFLECTION_CUBE_DEFAULT 0.35f
static unsigned int reflection_cube_key = 0;
static float reflection_rgb[3] = { 0.0f, 0.0f, 0.0f };
/* which of Chief's parts the part is: 1 his visor, 2 his armour, 0 none
(psp_fixups.py, by the shader's name) */
static int reflection_chief = 0;
static int reflection_off = -1;
static unsigned long reflection_parts = 0, reflection_cubes_known = 0;

static void reflection_keeps(void);
/* Chief's visor gets the reflection pass wherever he is drawn at least 24
pixels across, at chiefvisorgainN percent (150: beside the PC's c40 close-ups, the nearest in brightness): a restoration, one of the
campaign's defaults (psp_platform.c; "nochiefvisor" turns it off). On the
Xbox the visor's gold is its cube map's, added over a near-black base:
folded into the lighting it can't show, since the GE's ambient stops at 1
and the base x 1 stays dark */

void psp_ge_model_reflection(unsigned int cube_key, float const *rgb)
{
	GE_BENCH_SKIP();
	reflection_keeps();
	if (reflection_words.chief_visor > 0 && rgb && (int)rgb[3] == 1)
	{
		static int logged = 0;
		int width = 0, height = 0;

		if (!logged)
		{
			logged = 1;
			psp_tex_size(cube_key, &width, &height);
			platform_log("GE: chiefvisor: the visor's reflection %.2f %.2f %.2f, its map %08x %dx%d (a sphere map is 32x32)",
				rgb[0], rgb[1], rgb[2], cube_key, width, height);
		}
	}
	/* a reflection pass binds the part's sphere map in the replay */
	if (reflection_words.env_fp > 0 || reflection_words.env_near > 0 ||
		(reflection_words.chief_visor > 0 && rgb && (int)rgb[3] == 1))
		GE_PRETOUCH(cube_key, TXPT_MODEL_DETAIL);
	GE_DEFER(defer_model_reflection(cube_key, rgb));
	reflection_cube_key = cube_key;
	reflection_rgb[0] = rgb ? rgb[0] : 0.0f;
	reflection_rgb[1] = rgb ? rgb[1] : 0.0f;
	reflection_rgb[2] = rgb ? rgb[2] : 0.0f;
	reflection_chief = rgb ? (int)rgb[3] : 0;
}

int psp_tex_average_known(unsigned int key, unsigned int *abgr);

/* the part's ambient with its reflection folded in, or the model's own */
static unsigned int ambient_with_reflection(unsigned int base_key)
{
	unsigned int base, mask_abgr, cube_abgr;
	float cube[3] = { REFLECTION_CUBE_DEFAULT, REFLECTION_CUBE_DEFAULT, REFLECTION_CUBE_DEFAULT }, mask = 0.5f;
	float ambient[3];
	int channel;

	if (reflection_off < 0)
	{
		reflection_off = psp_keep("noreflection");
		if (reflection_off)
			platform_log("GE: noreflection: model reflections not folded into their ambient light");
	}
	if (reflection_off || reflection_rgb[0] + reflection_rgb[1] + reflection_rgb[2] <= 0.004f ||
		!psp_tex_average_known(base_key, &base))
		return model_ambient;
	if (reflection_cube_key && psp_tex_average_known(reflection_cube_key, &cube_abgr))
	{
		for (channel = 0; channel < 3; channel++)
			cube[channel] = (float)((cube_abgr >> (8 * channel)) & 0xFF) * (1.0f / 255.0f);
		reflection_cubes_known++;
	}
	if (change_key && psp_tex_average_known(change_key, &mask_abgr))
		mask = (float)(mask_abgr & 0xFF) * (1.0f / 255.0f);
	for (channel = 0; channel < 3; channel++)
	{
		float b = (float)((base >> (8 * channel)) & 0xFF) * (1.0f / 255.0f);

		ambient[channel] = (float)((model_ambient >> (8 * channel)) & 0xFF) * (1.0f / 255.0f) +
			reflection_rgb[channel] * cube[channel] * mask / (b > 0.08f ? b : 0.08f);
	}
	reflection_parts++;
	return color_from_rgb(ambient);
}

/* a part's reflection strength (0-1 a channel): the shader's brightness and
tint times the multipurpose map's mask on average, as folded into the
ambient above; 0 when the part reflects nothing or "noreflection" */
static int reflection_strength(float *rgb)
{
	unsigned int mask_abgr;
	float mask = 0.5f;
	int channel;

	if (reflection_off > 0 || reflection_rgb[0] + reflection_rgb[1] + reflection_rgb[2] <= 0.004f)
		return 0;
	/* a part without a multipurpose map reflects over its whole surface (a
	visor, glass), and so does Chief's visor: its multipurpose map's average
	isn't its reflection mask (the pack keeps the map for its change colour) */
	if (!change_key || reflection_chief == 1)
		mask = 1.0f;
	else if (psp_tex_average_known(change_key, &mask_abgr))
		mask = (float)(mask_abgr & 0xFF) * (1.0f / 255.0f);
	for (channel = 0; channel < 3; channel++)
	{
		rgb[channel] = reflection_rgb[channel] * mask;
		rgb[channel] = rgb[channel] > 1.0f ? 1.0f : rgb[channel];
	}
	return 1;
}

/* "envfp", "envnear" (opt-in while they are priced): a shiny part's
reflection drawn as the Xbox draws it, added over the lit base, for the
first-person weapon (envfp) and for models at least envpixelsN pixels
across on the screen (envnear, 160: Chief in a cutscene, a vehicle beside
the player), instead of folded into the ambient. The bake turns the
part's cube map into a sphere map (texpack.py, sphere_map); the GE's
environment mapping picks its texel from the normal's two components along
the camera's right and up (lights 2 and 3 carry them; lighting is off for
this pass), and the pass adds it times the part's strength where pass 1
drew. The mask is the part's average (one texture a pass, and the frame
has no alpha to hold the mask in): a part shiny in places shines evenly.
A pass more per such part. "envgainN": the strength in percent (100).
"envminN": only parts whose strongest channel reflects at least N percent
(a visor or glass, not the armour around it, whose mask the pass can't
apply) */
static unsigned long env_passes = 0, env_unbound = 0;
/* the visor's parts reaching the draw: all, textured and lit, shiny, and how
many pixels across the last one was */
static unsigned long visor_drawn = 0, visor_textured = 0, visor_shiny = 0;
static float visor_pixels = 0.0f;

/* the reflection words, read on the main CPU at its first reflection call,
before any record that needs them is replayed (reflection_words, above) */
static void reflection_keeps(void)
{
	if (reflection_words.read || psp_on_me())
		return;
	reflection_words.specular = psp_keep("modelspecular");
	reflection_words.specular_power = (float)psp_keep_number("specpower", 8);
	reflection_words.specular_gain = (float)psp_keep_number("specgain", 200) * (1.0f / 100.0f);
	if (reflection_words.specular)
		platform_log("GE: modelspecular: shiny parts' highlights from the GE's specular lighting, power %.0f",
			reflection_words.specular_power);
	reflection_words.env_near = psp_keep("envnear");
	reflection_words.env_pixels = psp_keep_number("envpixels", 160);
	reflection_words.env_gain = psp_keep_number("envgain", 100);
	reflection_words.env_min = psp_keep_number("envmin", 0);
	reflection_words.chief_visor = psp_keep("chiefvisor");
	reflection_words.chief_visor_gain = psp_keep_number("chiefvisorgain", 150);
	reflection_words.sheen = psp_keep("chiefsheen");
	reflection_words.sheen_gain = psp_keep_number("sheengain", 70);
	if (reflection_words.sheen)
		platform_log("GE: chiefsheen: Chief's armour gets the Xbox's reflection added over it by its mask, gain %d%%",
			reflection_words.sheen_gain);
	if (reflection_words.chief_visor)
		platform_log("GE: chiefvisor: Chief's visor drawn with its reflection pass, gain %d%%",
			reflection_words.chief_visor_gain);
	reflection_words.env_fp = psp_keep("envfp");
	if (reflection_words.env_fp || reflection_words.env_near)
		platform_log("GE: reflections drawn as a pass: %s%s (at %d pixels across), gain %d%%, parts reflecting at least %d%%",
			reflection_words.env_fp ? "first person " : "", reflection_words.env_near ? "near models" : "",
			reflection_words.env_pixels, reflection_words.env_gain, reflection_words.env_min);
	reflection_words.read = 1;
	sceKernelDcacheWritebackRange(&reflection_words, sizeof(reflection_words));
}

static int env_part(const float *strength)
{
	int width, height;
	float most = strength[0] > strength[1] ? strength[0] : strength[1];

	if (reflection_words.chief_visor && reflection_chief == 1 && model_pixels >= 24.0f)
		return reflection_cube_key && psp_tex_size(reflection_cube_key, &width, &height) && width == 32 && height == 32;
	most = most > strength[2] ? most : strength[2];
	if (most * 100.0f < (float)reflection_words.env_min)
		return 0;
	if (!(model_first_person ? reflection_words.env_fp :
		reflection_words.env_near && model_pixels >= (float)reflection_words.env_pixels))
		return 0;
	return reflection_cube_key && psp_tex_size(reflection_cube_key, &width, &height) && width == 32 && height == 32;
}

static int reflection_env_pass(void *vertices, const unsigned short *indices, long index_count, int strip,
	struct skin_part *skin, ScePspFMatrix4 *model, const float *strength)
{
	ScePspFVector3 across, upward;
	float gain = (float)(reflection_chief == 1 && reflection_words.chief_visor ? reflection_words.chief_visor_gain :
		reflection_words.env_gain) * (1.0f / 100.0f);
	unsigned int color;

	if (!psp_tex_bind(reflection_cube_key, NULL))
		return 0;
	color = 0xFF000000u | ((unsigned int)PSP_CLAMP_255((int)(strength[2] * gain * 255.0f)) << 16) |
		((unsigned int)PSP_CLAMP_255((int)(strength[1] * gain * 255.0f)) << 8) |
		(unsigned int)PSP_CLAMP_255((int)(strength[0] * gain * 255.0f));
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
	sceGuColor(color);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuTexScale(1.0f, 1.0f);
	sceGuTexOffset(0.0f, 0.0f);
	/* u from the normal along the camera's right, v along its down (the
	sphere map's first row is up): the GE takes (1 + n . light) / 2 */
	across.x = camera_right[0];
	across.y = camera_right[1];
	across.z = camera_right[2];
	upward.x = -camera_up[0];
	upward.y = -camera_up[1];
	upward.z = -camera_up[2];
	sceGuLight(2, GU_DIRECTIONAL, GU_DIFFUSE, &across);
	sceGuLight(3, GU_DIRECTIONAL, GU_DIFFUSE, &upward);
	lit_forget(1);
	sceGuTexMapMode(GU_ENVIRONMENT_MAP, 2, 3);
	sceGuDepthFunc(GU_EQUAL);
	sceGuDepthMask(GU_TRUE);
	/* added light fades to nothing in the fog */
	sceGuFog(fog_near, fog_far, 0xFF000000);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
	ge_blend(1);
	model_geometry(vertices, indices, index_count, strip, skin, model);
	ge_blend(0);
	sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
	sceGuTexWrap(GU_REPEAT, GU_REPEAT);
	sceGuFog(fog_near, fog_far, fog_color);
	sceGuDepthMask(GU_FALSE);
	sceGuDepthFunc(GU_GEQUAL);
	sceGuColor(0xFFFFFFFF);
	return 1;
}

/* the change colour multiplied in by the multipurpose map's mask, with the
GE's blend texture function: fragment white x (1 - mask) + colour x mask,
the colour as the environment colour. The mask is a CLUT the bake made from
the map's palette (its blue: texpack.py, change_mask) and placed after the
map's levels: the Media Engine replays this pass and can't read a texture
in VRAM (B25: Chief and the crew drawn white or red on the PSP), so nothing
here reads the texture; the GE loads the mask from wherever the map lives.
Until b144 the mask was worked out from the palette into a 40-slot cache */
int psp_tex_has(unsigned int key);

/* the self-illumination with the pack's glow map (the base x the
multipurpose map's green), times the colour, added: on the Xbox the colour
by the mask is added to the lighting, which then multiplies the base map, so
a lit-up area shows its own texture at full brightness (texpack.py,
glow_looks). The mask alone, added as it was, drew the main menu's ring as a
white band and every other self-illuminated part in its flat colour.
A map not yet in memory leaves the part unlit this frame rather than
flashing the flat colour. A part with a glow colour and no glow map in the
pack isn't lit either (glow_unmapped: none on b30, a10 or c20; the mask
alone, as before b119, would read the multipurpose map's palette) */
static unsigned long glow_unmapped = 0;

static int glow_add = -1, glow_no_clamp = -1;
static unsigned int glow_base_key = 0; /* the part's base, for the clamp: psp_ge_draw_model_body */
extern int psp_showcase_map;
static unsigned long glow_clamped = 0;
/* out of line and cold: only the main menu runs it, and glow_map_pass runs
~700 times a frame on b30, where its size in the instruction cache is what
a dormant block would cost (b133's nwbatch) */
static void __attribute__((noinline, cold)) glow_clamp_pass(void *vertices, const unsigned short *indices,
	long index_count, int strip, struct skin_part *skin, ScePspFMatrix4 *model)
{
	if (!psp_tex_bind(glow_base_key, NULL))
		return;
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
	sceGuFog(fog_near, fog_far, fog_color);
	sceGuBlendFunc(GU_MIN, GU_FIX, GU_FIX, 0, 0);
	model_geometry(vertices, indices, index_count, strip, skin, model);
	glow_clamped++;
}
static int glow_map_pass(void *vertices, const unsigned short *indices, long index_count, int strip,
	struct skin_part *skin, ScePspFMatrix4 *model)
{
	int clamped;

	if (!psp_tex_bind(glow_key, NULL))
		return 0;
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
	sceGuColor(0xFF000000 | glow_color);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDepthFunc(GU_EQUAL);
	sceGuDepthMask(GU_TRUE);
	/* added light fades to nothing in the fog */
	sceGuFog(fog_near, fog_far, 0xFF000000);
	/* a screen blend, glow + frame x (1 - glow): the Xbox clamps lighting
	plus self-illumination at 1 before the base multiplies it, so a lit part
	never goes past its own base; added, the ring's bright clouds clipped to
	white. Screened, the glow's share shrinks as the frame nears white, at
	no cost (one pass either way). Not the clamp exactly: where both are
	strong it still ends above the base. Keep word "glowadd": added */
	if (glow_add < 0)
	{
		glow_add = psp_keep("glowadd");
		if (glow_add)
			platform_log("GE: glowadd: the glow maps added, not screened");
	}
	/* on the showcase map, the clamp itself: the glow added, then the
	part's base drawn over it with GU_MIN, min(base x lighting + glow,
	base), as the Xbox caps a lit part at its own base. The base is fogged
	as pass 1 was, so the clamp holds under fog too. A pass more per
	self-illuminated part, which only the main menu may spend (b30 draws
	~700 glow passes a frame). Against ChupathingyCE's frames, the ring was
	far dimmer than either the add or the screen (the desktop build's frames),
	but on the PSP the clamp shows the base's aliasing where the ring is seen
	edge on, which the screen hides under the glow (the emulator: twice
	the shimmer). On by default since the ring's inner textures looked
	too bright on the PSP (B15): the clamp is the original's look, the
	shimmer its price. "glownoclamp": the screen alone, as before */
	if (glow_no_clamp < 0)
		glow_no_clamp = psp_keep("glownoclamp") != 0;
	clamped = psp_showcase_map && !glow_no_clamp && glow_base_key;
	if (glow_add || clamped)
		sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
	else
		sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_COLOR, 0xFFFFFF, 0);
	ge_blend(1);
	model_geometry(vertices, indices, index_count, strip, skin, model);
	if (clamped)
		glow_clamp_pass(vertices, indices, index_count, strip, skin, model);
	ge_blend(0);
	sceGuFog(fog_near, fog_far, fog_color);
	sceGuDepthMask(GU_FALSE);
	sceGuDepthFunc(GU_GEQUAL);
	sceGuColor(0xFFFFFFFF);
	glow_mapped++;
	return 1;
}
/* the model's detail map, as the Xbox draws it on a model shader without a
detail mask: the base x 2 x the detail. The pack's base for such a shader
already holds the detail's average (texpack.py, model_looks), and the detail
map is packed scaled to an average of a half (neutral_detail) or with the
Xbox's own levels, which fade toward it: drawn over the frame with the GE's
2 x source x frame blend it adds only the pattern, and changes nothing where
its levels have faded out. Last, after the self-illumination and its clamp:
the Xbox multiplies base x detail by lighting plus glow, the same product.
Main menu only (the outer ring's panel lines, detail scale 10, read from
the tags); every other map keeps the folded average alone, at no cost.
"nomodeldetail": the folded average alone there too. "modeldetailbiasN": the
detail's level N quarter levels coarser (the GE picks a model's level
itself, and the map repeats ten times across the part) */
static void __attribute__((noinline, cold)) model_detail_pass(void *vertices, const unsigned short *indices,
	long index_count, int strip, struct skin_part *skin, ScePspFMatrix4 *model)
{
	static int off = -1, bias = 0, pull = 0, logs = 0;
	static unsigned int logged[3];
	int bound;

	if (off < 0)
	{
		off = psp_keep("nomodeldetail");
		bias = psp_keep_number("modeldetailbias", 0);
		/* tested nearer-or-equal with its depth pulled toward the eye, not
		equal: far out on the ring the equal test missed by a rounding on some
		frames and not others, and the band flickered. "detailpullN" another
		pull, "detailequal" the equal test */
		pull = psp_keep("detailequal") ? 0 : psp_keep_number("detailpull", 64);
	}
	if (off)
		return;
	bound = psp_tex_bind(model_detail_key, NULL);
	/* each detail map's first draw, logged: drawn, or not in memory yet
	(still streaming in, or not in the pack) */
	if (model_detail_key != logged[0] && model_detail_key != logged[1] && model_detail_key != logged[2])
	{
		logged[logs++ % 3] = model_detail_key;
		if (logs <= 12)
			platform_log("GE: model detail %08x at %.2f x %.2f: %s", model_detail_key, model_detail_u,
				model_detail_v, bound ? "drawn" : "not in memory yet");
	}
	if (!bound)
		return;
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
	sceGuTexWrap(GU_REPEAT, GU_REPEAT);
	/* on the part's own coordinates, as the base: times the vertices' scale */
	model_tex_scale(model_u_scale * model_detail_u, model_v_scale * model_detail_v);
	if (bias)
		sceGuTexLevelMode(tex_level_mode, tex_bias + bias * 0.25f);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDepthFunc(pull > 0 ? GU_GEQUAL : GU_EQUAL);
	if (pull > 0)
		sceGuDepthOffset(pull > 4096 ? 4096 : pull);
	sceGuDepthMask(GU_TRUE);
	/* fogged toward its neutral colour, which the blend leaves unchanged */
	sceGuFog(fog_near, fog_far, detail_neutral(model_detail_key));
	sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_SRC_COLOR, 0, 0);
	ge_blend(1);
	model_geometry(vertices, indices, index_count, strip, skin, model);
	ge_blend(0);
	if (bias)
		sceGuTexLevelMode(tex_level_mode, tex_bias);
	sceGuFog(fog_near, fog_far, fog_color);
	if (pull > 0)
		sceGuDepthOffset(0);
	sceGuDepthMask(GU_FALSE);
	sceGuDepthFunc(GU_GEQUAL);
}
static int change_pass(void *vertices, const unsigned short *indices, long index_count, int strip,
	struct skin_part *skin, ScePspFMatrix4 *model)
{
	extern const void *psp_tex_change_mask(unsigned int key, int *count);
	const void *mask;
	int count;

	/* the Xbox's channel order, read off the Xbox disc's own Blood Gulch
	(the cyborg's map: the armour plates solid in blue, alpha all white): red
	the reflection mask, green self-illumination, blue the change colour,
	alpha the detail mask. Gearbox's PC release reordered them */
	if (!psp_tex_bind(change_key, NULL) || !(mask = psp_tex_change_mask(change_key, &count)))
	{
		change_unbound++;
		return 0;
	}
	sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
	sceGuClutLoad(count / 8, mask);
	/* white toward the colour by the mask */
	sceGuTexFunc(GU_TFX_BLEND, GU_TCC_RGB);
	sceGuTexEnvColor(change_color);
	sceGuColor(0xFFFFFFFF);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDepthFunc(GU_EQUAL);
	sceGuDepthMask(GU_TRUE);
	/* fogged toward white: far off, nothing changes */
	sceGuFog(fog_near, fog_far, 0xFFFFFFFF);
	sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_FIX, 0, 0);
	ge_blend(1);
	model_geometry(vertices, indices, index_count, strip, skin, model);
	ge_blend(0);
	sceGuFog(fog_near, fog_far, fog_color);
	sceGuDepthMask(GU_FALSE);
	sceGuDepthFunc(GU_GEQUAL);
	sceGuTexEnvColor(0xFFFFFFFF);
	return 1;
}

/* "chiefsheen" (a restoration, on by default): the Xbox adds a shiny part's
reflection over its lit, change-coloured base, by the multipurpose map's
red (the reflection mask). Folded into the ambient light, as every other
part's is, it is multiplied by the base instead, so Chief's armour stayed
the base's saturated green; added, the environment's grey brightens and
greys the plates the mask marks, as on the Xbox. The cube map's average
stands for the cube (no view dependence); the visor keeps its own pass */
static unsigned long sheen_passes = 0;
extern const void *psp_tex_reflection_mask(unsigned int key, int *count);

static int sheen_pass(void *vertices, const unsigned short *indices, long index_count, int strip,
	struct skin_part *skin, ScePspFMatrix4 *model)
{
	float cube[3] = { REFLECTION_CUBE_DEFAULT, REFLECTION_CUBE_DEFAULT, REFLECTION_CUBE_DEFAULT };
	float gain = (float)reflection_words.sheen_gain * (1.0f / 100.0f);
	unsigned int cube_abgr, color;
	const void *mask;
	int count;

	if (!psp_tex_bind(change_key, NULL) || !(mask = psp_tex_reflection_mask(change_key, &count)))
		return 0;
	if (reflection_cube_key && psp_tex_average_known(reflection_cube_key, &cube_abgr))
	{
		cube[0] = (float)(cube_abgr & 0xFF) * (1.0f / 255.0f);
		cube[1] = (float)((cube_abgr >> 8) & 0xFF) * (1.0f / 255.0f);
		cube[2] = (float)((cube_abgr >> 16) & 0xFF) * (1.0f / 255.0f);
	}
	color = 0xFF000000u |
		((unsigned int)PSP_CLAMP_255((int)(reflection_rgb[2] * cube[2] * gain * 255.0f)) << 16) |
		((unsigned int)PSP_CLAMP_255((int)(reflection_rgb[1] * cube[1] * gain * 255.0f)) << 8) |
		(unsigned int)PSP_CLAMP_255((int)(reflection_rgb[0] * cube[0] * gain * 255.0f));
	sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
	sceGuClutLoad(count / 8, mask);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
	sceGuColor(color);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuDepthFunc(GU_EQUAL);
	sceGuDepthMask(GU_TRUE);
	/* added light fades to nothing in the fog */
	sceGuFog(fog_near, fog_far, 0xFF000000);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
	ge_blend(1);
	model_geometry(vertices, indices, index_count, strip, skin, model);
	ge_blend(0);
	sceGuFog(fog_near, fog_far, fog_color);
	sceGuDepthMask(GU_FALSE);
	sceGuDepthFunc(GU_GEQUAL);
	sceGuColor(0xFFFFFFFF);
	return 1;
}

/* build 87: "skincut" (on by default; "noskincut": dropped, as builds 41-86).
A bending model's triangle reaching past the near plane used to be left
out whole: the tank's treads and the warthog's wheels opened holes when the
driving camera came close (b79's shots 5 and 6, and the distance where
it stops). Now its three corners are bent on the CPU, by the same weights
and node matrices the GE uses, and cut at the near plane and guard band as
the level's triangles are; the pieces are drawn already bent (the model
matrix is the identity for a bending part), their normals turned the same
way so the GE lights them alike */
static int skin_cut = -1;
static unsigned long skin_cut_triangles = 0, skin_cut_pieces = 0;

/* one skinned corner: bent position (camera-relative, the GE's space for a
bending part), bent normal (8-bit units), texture coordinates */
static void skin_corner(const unsigned char *record, int weights, int uv_at, const ScePspFMatrix4 *bone,
	float *position, float *normal, float *uv)
{
	const float *p = (const float *)(record + uv_at + 12), *t = (const float *)(record + uv_at);
	const signed char *n = (const signed char *)(record + uv_at + 8);
	int slot, axis;

	for (axis = 0; axis < 3; axis++)
		position[axis] = normal[axis] = 0.0f;
	for (slot = 0; slot < weights; slot++)
	{
		const ScePspFMatrix4 *m = &bone[slot];
		float w = record[slot] * (1.0f / 128.0f);

		if (!record[slot])
			continue;
		position[0] += w * (m->x.x * p[0] + m->y.x * p[1] + m->z.x * p[2] + m->w.x);
		position[1] += w * (m->x.y * p[0] + m->y.y * p[1] + m->z.y * p[2] + m->w.y);
		position[2] += w * (m->x.z * p[0] + m->y.z * p[1] + m->z.z * p[2] + m->w.z);
		normal[0] += w * (m->x.x * n[0] + m->y.x * n[1] + m->z.x * n[2]);
		normal[1] += w * (m->x.y * n[0] + m->y.y * n[1] + m->z.y * n[2]);
		normal[2] += w * (m->x.z * n[0] + m->y.z * n[1] + m->z.z * n[2]);
	}
	uv[0] = t[0];
	uv[1] = t[1];
}

/* build 106: a bending corner's camera-space position on the VFPU. Each of
a batch's slots (its node matrices' rows along the camera's right, up and
forward: depth_row) is loaded once a batch as a VFPU matrix, M100 to M700,
row a of the batch's numbers in column a, so vhtfm4 gives the three dot
products with (p, 1) at once (the probe, psp_vfpu.c: with M, element a is
column a dotted with (p, 1)). A corner is then one transform per weighted
slot, scaled by its weight and summed: on b30's beach these corners were
most of model_geometry, 9% of the emulator's time. Up to 7 slots (M000
holds the corner's working columns); 8-slot batches stay on the FPU.
The sums run in the VFPU's own order, so a corner can differ from the FPU's
in its last bit: a corner exactly on the near margin may then fall the
other way (b30's beach: 2,193,417 corners compared, none did).
Off unless keep.txt says "vfpuskin": in the emulator it came out 2-4%
slower over the frame (the emulator's VFPU costs are its own), so it waits
for the PSP's A/B. "skinvfpucheck": both, and the corners whose verdicts
differ counted in the log */
extern int psp_vfpu_on;
static int skin_vfpu = -1, skin_vfpu_check = 0;
static unsigned long skin_vfpu_corners = 0, skin_vfpu_differ = 0;

#define SKIN_VFPU_LOAD(m, rows) __asm__ volatile( \
	"lv.q C" #m "00, 0(%0)\n" "lv.q C" #m "10, 16(%0)\n" "lv.q C" #m "20, 32(%0)\n" "vzero.q C" #m "30\n" \
	: : "r"(rows) : "memory")
#define SKIN_VFPU_SLOT(m, weight) __asm__ volatile( \
	"mtv %0, S030\n" "vhtfm4.q C010, M" #m "00, C000\n" "vscl.t C010, C010, S030\n" "vadd.t C020, C020, C010\n" \
	: : "r"(weight))

/* the batch's slots into M100 onward (rows: 16-byte aligned, [slot][3][4]) */
static void skin_vfpu_load(const float (*rows)[3][4], int weights)
{
	switch (weights > 7 ? 7 : weights)
	{
	case 7: SKIN_VFPU_LOAD(7, rows[6]); /* fall through */
	case 6: SKIN_VFPU_LOAD(6, rows[5]); /* fall through */
	case 5: SKIN_VFPU_LOAD(5, rows[4]); /* fall through */
	case 4: SKIN_VFPU_LOAD(4, rows[3]); /* fall through */
	case 3: SKIN_VFPU_LOAD(3, rows[2]); /* fall through */
	case 2: SKIN_VFPU_LOAD(2, rows[1]); /* fall through */
	case 1: SKIN_VFPU_LOAD(1, rows[0]); /* fall through */
	default: break;
	}
}

/* one corner: p its position, record its weight bytes; at[3] out */
static void skin_vfpu_corner(const unsigned char *record, int weights, const float *p, float *at)
{
	int slot;

	__asm__ volatile("lv.s S000, 0(%0)\n" "lv.s S001, 4(%0)\n" "lv.s S002, 8(%0)\n" "vzero.t C020\n"
		: : "r"(p) : "memory");
	for (slot = 0; slot < weights; slot++)
		if (record[slot])
		{
			union { float f; unsigned int u; } w;

			w.f = record[slot] * (1.0f / 128.0f);
			switch (slot)
			{
			case 0: SKIN_VFPU_SLOT(1, w.u); break;
			case 1: SKIN_VFPU_SLOT(2, w.u); break;
			case 2: SKIN_VFPU_SLOT(3, w.u); break;
			case 3: SKIN_VFPU_SLOT(4, w.u); break;
			case 4: SKIN_VFPU_SLOT(5, w.u); break;
			case 5: SKIN_VFPU_SLOT(6, w.u); break;
			default: SKIN_VFPU_SLOT(7, w.u); break;
			}
		}
	__asm__ volatile("sv.s S020, 0(%0)\n" "sv.s S021, 4(%0)\n" "sv.s S022, 8(%0)\n" : : "r"(at) : "memory");
}

/* build 106: a bending part near the eye: what its first pass worked out
(each batch's triangles kept whole, and its pieces cut at the near plane and
guard band), drawn again by the part's other passes (the two-sided pass's
back faces, the change and glow colour passes) from the same lists, which
stay in the display list for the frame. Each pass used to test every corner
again: a swaying tree over the player on b30's beach (two-sided leaves)
cost 24% of the game thread in the emulator's PC samples. Valid for the part
being drawn only (psp_ge_draw_model_body forgets it first) */
static const struct skin_part *near_memo_skin = NULL;
static int near_memo_complete = 0;
static struct
{
	const unsigned short *draw;
	int draw_count;
	const unsigned char *pieces;
	int cut_count;
} near_memo[64];

/* build 107: a bending part's bone matrices, once a part. Each batch loads
its slots' matrices (13 words a slot) before its draw, and every pass of the
part (the two-sided pass, the change and glow colour passes) loaded them all
again: b30's beach fight put 66-74 KB of bone matrices in the list a frame
(1,300-1,450 loads), a third of the models' share when the list was full.
The first pass now writes each batch's loads into the list as a sub-list
ending in a return, and every pass calls it (2 words). The matrices are
worked out on the CPU once a part, too. Valid for the part being drawn only
(psp_ge_draw_model_body forgets it first, as near_memo). Keep word
"bonesdirect": loaded in the list by every pass, as before */
#define BONE_MEMO_BATCHES 64
static const struct skin_part *bone_memo_skin = NULL;
static const unsigned int *bone_memo[BONE_MEMO_BATCHES];
static int bones_direct = -1;

/* a slot's matrix as the GE's bone commands (pspgu's sceGuBoneMatrix: the
slot's place, then the 4 rows' first 3 columns as 24-bit floats) */
static unsigned int *bone_commands(unsigned int *at, int slot, const ScePspFMatrix4 *matrix)
{
	const float *m = (const float *)matrix;
	int row, column;

	*at++ = (0x2Au << 24) | (unsigned int)(slot * 12);
	for (row = 0; row < 4; row++)
		for (column = 0; column < 3; column++)
		{
			union { float f; unsigned int u; } v;

			v.f = m[row * 4 + column];
			*at++ = (0x2Bu << 24) | (v.u >> 8);
		}
	return at;
}

/* build 118: the near path's fast test for a bending batch. Every vertex
of the batch lies in its sphere (the part's own space); each slot's matrix
takes that sphere to a sphere in camera space, its radius stretched by the
matrix's largest stretch; and a bent vertex is a weighted mean of its
slots' images of it, so it lies in the hull of those spheres. All of them
wholly inside the corner tests' planes, every corner passes them, and no
triangle is dropped or cut: the batch is drawn whole, as the corner loop
would have found. "nearspherecheck" runs the loop anyway and counts the
batches the test passed that it didn't draw whole; "nearspherefull": the
loop for every batch, as before (their counters: by near_parts) */

/* the batch's sphere, worked out once from its vertices (the bending
part's records: the position after the texture coordinate and normal) */
static void batch_sphere(struct skin_batch *b, const struct skin_part *skin, const void *vertices)
{
	float low[3] = { 0.0f, 0.0f, 0.0f }, high[3] = { 0.0f, 0.0f, 0.0f }, reach = 0.0f;
	int uv_at = skin->stride - 24, i, axis;

	for (i = 0; i < b->index_count; i++)
	{
		const float *p = (const float *)((const unsigned char *)vertices + skin->stride * b->indices[i] + uv_at + 12);

		for (axis = 0; axis < 3; axis++)
		{
			if (i == 0 || p[axis] < low[axis]) low[axis] = p[axis];
			if (i == 0 || p[axis] > high[axis]) high[axis] = p[axis];
		}
	}
	for (axis = 0; axis < 3; axis++)
		b->center[axis] = (low[axis] + high[axis]) * 0.5f;
	for (i = 0; i < b->index_count; i++)
	{
		const float *p = (const float *)((const unsigned char *)vertices + skin->stride * b->indices[i] + uv_at + 12);
		float dx = p[0] - b->center[0], dy = p[1] - b->center[1], dz = p[2] - b->center[2];
		float d = dx * dx + dy * dy + dz * dz;

		if (d > reach)
			reach = d;
	}
	b->radius = sqrtf(reach);
}

/* whether every used slot's image of the batch's sphere lies wholly inside
the corner tests' planes, near being the larger of their two near limits.
depth_row: each slot's matrix as rows along the camera's right, up and
forward (model_geometry). The largest stretch of a slot's 3x3 part is
bounded by its Gram matrix's largest row sum (Gershgorin), which is exact
for a rotation with one scale, as the game's nodes are */
static int batch_sphere_inside(const struct skin_batch *b, int weights, const float (*depth_row)[3][4], float near)
{
	int slot;

	for (slot = 0; slot < weights; slot++)
	{
		const float (*row)[4] = depth_row[slot];
		float c[3], gram[3][3], stretch = 0.0f;
		int i, j;

		if (b->node[slot] == 0xFF)
			continue;
		for (i = 0; i < 3; i++)
		{
			c[i] = row[i][0] * b->center[0] + row[i][1] * b->center[1] + row[i][2] * b->center[2] + row[i][3];
			for (j = 0; j < 3; j++)
				gram[i][j] = row[i][0] * row[j][0] + row[i][1] * row[j][1] + row[i][2] * row[j][2];
		}
		for (i = 0; i < 3; i++)
		{
			float sum = fabsf(gram[i][0]) + fabsf(gram[i][1]) + fabsf(gram[i][2]);

			if (sum > stretch)
				stretch = sum;
		}
		if (!sphere_inside(c, b->radius * sqrtf(stretch), near))
			return 0;
	}
	return 1;
}

/* build 119: the near path's run test ("NC"). The corner loop works out a
corner's verdict only to find the triangles with a corner behind the near
margin or past a guard plane (camera_corner_out): those are dropped or cut,
and every other triangle is kept as it is. A bending batch round the eye
(a30's trees over the player: 22 ms a frame of corner loop on the PSP)
fails the batch's sphere test, yet most of its triangles lie wholly in
front, and many wholly behind. So the batch's triangles are taken in runs
of NEAR_CLUSTER_TRIANGLES, each with its own sphere (worked out once, as
batch_sphere does), and each slot's image of it is tested as
batch_sphere_inside does. Every image wholly in front of the margin and
inside the guard planes: no corner of the run is out, and its triangles are
kept with none worked out. Every image wholly beyond one and the same plane
(behind the nearer of the two near limits, or past a guard plane): every
corner is out on that plane, so each triangle is dropped, as the loop would
drop it. "nearclusterfull": the loop for every triangle, as before;
"nearclustercheck": the loop runs anyway and counts the triangles whose own
verdict disagrees with their run's */

/* the runs' spheres asked for in the Media Engine's work, made by the main
CPU when the frame is shown (the heap is the main CPU's: its lock is a
system call the ME can't make), so a part has them from its next frame.
Meanwhile it takes the loop for every triangle, which gives the same
answers ("nearclusterfull", "rigidrunsfull") */
#define RUNS_WANTED 48
static struct runs_want
{
	struct skin_batch *batch; /* a skinned batch's, or NULL: a rigid part's */
	const struct skin_part *skin;
	const void *vertices;
	const unsigned short *indices;
	long index_count;
	int strip;
} runs_wanted[RUNS_WANTED];
static int runs_wanted_count = 0;
static unsigned long runs_wanted_made = 0, runs_wanted_dropped = 0;

static void runs_want(struct skin_batch *batch, const struct skin_part *skin, const void *vertices,
	const unsigned short *indices, long index_count, int strip)
{
	int k;

	for (k = 0; k < runs_wanted_count; k++)
	{
		struct runs_want const *w = &runs_wanted[k];

		if (batch ? w->batch == batch :
			!w->batch && w->vertices == vertices && w->indices == indices && w->index_count == index_count && w->strip == strip)
			return;
	}
	if (runs_wanted_count == RUNS_WANTED)
	{
		runs_wanted_dropped++;
		return;
	}
	runs_wanted[runs_wanted_count].batch = batch;
	runs_wanted[runs_wanted_count].skin = skin;
	runs_wanted[runs_wanted_count].vertices = vertices;
	runs_wanted[runs_wanted_count].indices = indices;
	runs_wanted[runs_wanted_count].index_count = index_count;
	runs_wanted[runs_wanted_count].strip = strip;
	runs_wanted_count++;
}

/* the runs' spheres, in the part's own space (the batch's records) */
static void near_clusters_make(struct skin_batch *b, const struct skin_part *skin, const void *vertices)
{
	int triangles = b->index_count / 3, count = (triangles + NEAR_CLUSTER_TRIANGLES - 1) / NEAR_CLUSTER_TRIANGLES;
	int uv_at = skin->stride - 24, c;

	if (PSP_ME_WORK())
	{
		runs_want(b, skin, vertices, NULL, 0, 0);
		return;
	}
	b->cluster_count = 0;
	b->clusters = count > 0 ? cache_malloc(count * sizeof(float[4])) : NULL;
	if (!b->clusters)
		return;
	b->cluster_count = count;
	near_cluster_bytes += (unsigned long)count * sizeof(float[4]);
	for (c = 0; c < count; c++)
	{
		int first = c * NEAR_CLUSTER_TRIANGLES * 3, last = first + NEAR_CLUSTER_TRIANGLES * 3, i, axis;
		float low[3] = { 0.0f, 0.0f, 0.0f }, high[3] = { 0.0f, 0.0f, 0.0f }, reach = 0.0f;

		if (last > triangles * 3)
			last = triangles * 3;
		for (i = first; i < last; i++)
		{
			const float *p = (const float *)((const unsigned char *)vertices + skin->stride * b->indices[i] + uv_at + 12);

			for (axis = 0; axis < 3; axis++)
			{
				if (i == first || p[axis] < low[axis]) low[axis] = p[axis];
				if (i == first || p[axis] > high[axis]) high[axis] = p[axis];
			}
		}
		for (axis = 0; axis < 3; axis++)
			b->clusters[c][axis] = (low[axis] + high[axis]) * 0.5f;
		for (i = first; i < last; i++)
		{
			const float *p = (const float *)((const unsigned char *)vertices + skin->stride * b->indices[i] + uv_at + 12);
			float dx = p[0] - b->clusters[c][0], dy = p[1] - b->clusters[c][1], dz = p[2] - b->clusters[c][2];
			float d = dx * dx + dy * dy + dz * dz;

			if (d > reach)
				reach = d;
		}
		b->clusters[c][3] = sqrtf(reach);
	}
}

/* each used slot's largest stretch (batch_sphere_inside's bound); -1 for a
slot the batch doesn't use */
static void near_slot_stretch(const struct skin_batch *b, int weights, const float (*depth_row)[3][4], float *scale)
{
	int slot;

	for (slot = 0; slot < weights; slot++)
	{
		const float (*row)[4] = depth_row[slot];
		float gram[3][3], stretch = 0.0f;
		int i, j;

		scale[slot] = -1.0f;
		if (b->node[slot] == 0xFF)
			continue;
		for (i = 0; i < 3; i++)
			for (j = 0; j < 3; j++)
				gram[i][j] = row[i][0] * row[j][0] + row[i][1] * row[j][1] + row[i][2] * row[j][2];
		for (i = 0; i < 3; i++)
		{
			float sum = fabsf(gram[i][0]) + fabsf(gram[i][1]) + fabsf(gram[i][2]);

			if (sum > stretch)
				stretch = sum;
		}
		scale[slot] = sqrtf(stretch);
	}
}

/* the planes a camera-space sphere lies wholly beyond, as outside()'s bits:
1 the near limit given, 2 and 4 the guard planes left and right, 8 and 16
up and down. The radius gets sphere_inside's margin; a NaN gives none */
static int sphere_beyond(const float *c, float radius, float near)
{
	float r = radius * 1.01f + 0.01f;
	int bits = 0;

	if (!(radius >= 0.0f))
		return 0;
	if (c[2] + r < near) bits |= 1;
	if (c[0] - guard_x * c[2] > r * guard_x_length) bits |= 2;
	if (-c[0] - guard_x * c[2] > r * guard_x_length) bits |= 4;
	if (c[1] - guard_y * c[2] > r * guard_y_length) bits |= 8;
	if (-c[1] - guard_y * c[2] > r * guard_y_length) bits |= 16;
	return bits;
}

/* a run's verdict: 1 every corner in (kept whole), 2 every corner out on
one plane (dropped), 0 either */
static int near_cluster_verdict(const struct skin_batch *b, int c, int weights, const float (*depth_row)[3][4],
	const float *scale, float near_in, float near_out)
{
	const float *sphere = b->clusters[c];
	int slot, inside = 1, beyond = 0x1F, used = 0;

	for (slot = 0; slot < weights; slot++)
	{
		const float (*row)[4] = depth_row[slot];
		float centre[3], radius;
		int i;

		if (scale[slot] < 0.0f)
			continue;
		used++;
		for (i = 0; i < 3; i++)
			centre[i] = row[i][0] * sphere[0] + row[i][1] * sphere[1] + row[i][2] * sphere[2] + row[i][3];
		radius = sphere[3] * scale[slot];
		if (inside && !sphere_inside(centre, radius, near_in))
			inside = 0;
		if (beyond)
			beyond &= sphere_beyond(centre, radius, near_out);
		if (!inside && !beyond)
			return 0;
	}
	return !used ? 0 : inside ? 1 : beyond ? 2 : 0;
}

static void model_geometry(void *vertices, const unsigned short *indices, long index_count, int strip,
	struct skin_part *skin, ScePspFMatrix4 *model)
{
	if (skin)
	{
		int batch, slot;
		ScePspFMatrix4 bone[SKIN_SLOTS];
		int near = model_near && !model_first_person;
		int from_memo = near && near_memo_complete && near_memo_skin == skin, memo_failed = 0;

		if (skin_cut < 0)
			skin_cut = !psp_keep("noskincut");

		for (batch = 0; batch < skin->batch_count && !list_full(); batch++)
		{
			struct skin_batch *b = &skin->batches[batch];
			const unsigned short *draw = b->indices;
			int draw_count = b->index_count;
			/* each slot's matrix as the depth it gives a point: its rows
			dotted with the camera's forward direction (build 41) */
			/* static: 16-byte aligned for the VFPU's quad loads (build 106);
			this renderer runs on one thread */
			static float depth_row[SKIN_SLOTS][3][4] __attribute__((aligned(16)));
			int on_vfpu;
			/* the CPU needs the matrices only to test the corners near the eye */
			int cpu_matrices = near && !from_memo;
			const unsigned int *sub = bone_memo_skin == skin && batch < BONE_MEMO_BATCHES ? bone_memo[batch] : NULL;
			unsigned int *writing = NULL, *cursor = NULL;

			if (bones_direct < 0)
				bones_direct = psp_keep("bonesdirect");
			if (!sub && !bones_direct && batch < BONE_MEMO_BATCHES)
			{
				int bytes = (skin->weights * 13 + 1) * 4;

				writing = list_take(bytes);
				if (writing)
				{
					model_bone_sub_bytes += bytes;
					if (bone_memo_skin != skin)
					{
						bone_memo_skin = skin;
						memset(bone_memo, 0, sizeof(bone_memo));
					}
					cursor = writing;
				}
			}
			unsigned int slot_start = probe_clock(); /* build 119's probe */

			for (slot = 0; slot < skin->weights; slot++)
			{
				int node = b->node[slot];

				memset(depth_row[slot], 0, sizeof(depth_row[slot]));
				if (node != 0xFF)
				{
					int axis;

					if (sub && !cpu_matrices)
						continue;
					halo_matrix(model_nodes + 13 * (node < model_node_count ? node : 0), model);
					if (cursor)
						cursor = bone_commands(cursor, slot, model);
					else if (!sub)
					{
						sceGuBoneMatrix(slot, model);
						model_bone_loads++;
					}
					bone[slot] = *model;
					if (!cpu_matrices)
						continue;
					/* the matrix's rows along the camera's right, up and forward */
					for (axis = 0; axis < 3; axis++)
					{
						const float *d = axis == 0 ? camera_right : axis == 1 ? camera_up : camera_forward;

						depth_row[slot][axis][0] = model->x.x * d[0] + model->x.y * d[1] + model->x.z * d[2];
						depth_row[slot][axis][1] = model->y.x * d[0] + model->y.y * d[1] + model->y.z * d[2];
						depth_row[slot][axis][2] = model->z.x * d[0] + model->z.y * d[1] + model->z.z * d[2];
						depth_row[slot][axis][3] = model->w.x * d[0] + model->w.y * d[1] + model->w.z * d[2] +
							ge_eye_camera[axis];
					}
				}
			}
			slot_us += probe_clock() - slot_start;
			slot_batches++;
			if (cursor)
			{
				/* the sub-list ends with a return; this pass calls it too */
				*cursor++ = 0x0Bu << 24;
				bone_memo[batch] = writing;
				sub = writing;
			}
			if (sub)
			{
				/* BASE and CALL, as pspgu's sceGuCallList (without its stall update) */
				sceGuSendCommandi(0x10, ((unsigned int)sub >> 8) & 0xF0000);
				sceGuSendCommandi(0x0A, (unsigned int)sub & 0xFFFFFF);
				model_bone_calls++;
			}
			if (from_memo && batch < 64)
			{
				/* the first pass's lists */
				draw = near_memo[batch].draw;
				draw_count = near_memo[batch].draw_count;
				if (near_memo[batch].cut_count)
				{
					if (draw_count)
						sceGumDrawArray(GU_TRIANGLES, GU_WEIGHTS(skin->weights) | GU_WEIGHT_8BIT | GU_TEXTURE_32BITF |
							GU_NORMAL_8BIT | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D, draw_count, draw,
							vertices);
					sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_NORMAL_8BIT | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
						near_memo[batch].cut_count, NULL, near_memo[batch].pieces);
					continue;
				}
				if (!draw_count)
					continue;
			}
			else if (near)
			{
				/* build 41: a bending model at the eye (the warthog round the
				camera behind it: its roll bars, turret, axles) drops the
				triangles not wholly in front of the near plane, as rigid ones
				have since build 38. A corner behind the eye, given to the GE,
				lands on the far side of the screen: the long spikes the tester saw.
				The CPU works out each corner's depth from its weights */
				/* build 107: the copy of the triangles drawn as they are is taken
				at the first one that isn't (dropped or cut), with those before it;
				a batch drawn whole uses its own indices. Taken first, as before,
				every near batch was copied: b30 beside the Pelicans and the
				marines, 70-160 KB of copied data in the list a frame */
				unsigned short *kept = NULL;
				float limit = projection_near * 1.25f;
				int t, k, out = 0, uv_at = skin->stride - 24, cut_count = 0, kept_failed = 0;
				int whole_by_sphere = 0;
				struct cut_store pieces_store;
				/* build 119: the runs (near_cluster_verdict) */
				float slot_scale[SKIN_SLOTS];
				int use_clusters = 0, cluster_end = 0, cluster_state = 0;

				/* build 118: the batch's sphere first (batch_sphere_inside): wholly
				inside, the corner loop is skipped and the batch drawn whole */
				if (near_sphere_full < 0)
				{
					near_sphere_full = psp_keep("nearspherefull");
					near_sphere_check = psp_keep("nearspherecheck");
				}
				if (!near_sphere_full)
				{
					/* the ME's work never works one out: the corner loop
					instead, which draws what the sphere would have */
					if (b->radius < 0.0f && (batch_sphere_old > 0 || !PSP_ME_WORK()))
						batch_sphere(b, skin, vertices);
					whole_by_sphere = b->radius >= 0.0f && batch_sphere_inside(b, skin->weights,
						(const float (*)[3][4])depth_row, limit > NEAR_CLIP ? limit : NEAR_CLIP);
				}

				unsigned int loop_start = probe_clock(); /* build 119's probe */
				unsigned long probe_corners = 0; /* in a register; added once below */

				cut_store_begin(&pieces_store, model_cut, MAXIMUM_MODEL_CUT_CORNERS, CUT_SKIN);

				/* each vertex's verdict once a batch (build 43: worked out per
				corner, it cost up to 6 ms a frame on the PSP in build 42) */
				corner_verdict_reset();
				/* build 106: the corners on the VFPU (skin_vfpu_corner) */
				if (skin_vfpu < 0)
				{
					skin_vfpu = psp_vfpu_on && psp_keep("vfpuskin");
					skin_vfpu_check = psp_keep("skinvfpucheck");
				}
				/* (build 112: never in the Media Engine's work: the ME has no VFPU) */
				on_vfpu = skin_vfpu && skin->weights <= 7 && !PSP_ME_WORK();
				if (on_vfpu)
					skin_vfpu_load((const float (*)[3][4])depth_row, skin->weights);
				if (skin_axis_loop < 0)
				{
					skin_axis_loop = psp_keep("skinaxisloop");
					skin_axis_check = psp_keep("skinaxischeck");
				}
				/* build 119: the runs' spheres (near_cluster_verdict) */
				if (near_cluster_full < 0)
				{
					near_cluster_full = psp_keep("nearclusterfull");
					near_cluster_check = psp_keep("nearclustercheck");
				}
				if (!near_cluster_full && !whole_by_sphere)
				{
					if (b->cluster_count < 0)
						near_clusters_make(b, skin, vertices);
					if (b->cluster_count > 0)
					{
						near_slot_stretch(b, skin->weights, (const float (*)[3][4])depth_row, slot_scale);
						use_clusters = 1;
					}
				}
				for (t = 0; (!whole_by_sphere || near_sphere_check) && t + 2 < draw_count; t += 3)
				{
					/* build 106: each corner's verdict is its planes as bits
					(outside(): near, right, left, top, bottom) and 32 for
					"behind" (camera_corner_out, the near test's margin); a
					triangle wholly outside one plane is dropped here, without
					bending its corners on the CPU only to find the same (the
					tree over the player on b30's beach: most of its behind
					triangles were wholly behind) */
					int behind = 0, bits[3];

					/* build 119: a new run, its verdict */
					if (use_clusters && t >= cluster_end)
					{
						int c = t / (3 * NEAR_CLUSTER_TRIANGLES);

						cluster_end = (c + 1) * 3 * NEAR_CLUSTER_TRIANGLES;
						cluster_state = c < b->cluster_count ? near_cluster_verdict(b, c, skin->weights,
							(const float (*)[3][4])depth_row, slot_scale, limit, limit < NEAR_CLIP ? limit : NEAR_CLIP) : 0;
						if (cluster_state == 1)
							near_cluster_kept++;
						else if (cluster_state == 2)
							near_cluster_dropped++;
						else
							near_cluster_mixed++;
					}
					if (cluster_state == 1 && !near_cluster_check)
					{
						/* every corner in: kept as it is */
						if (kept)
						{
							kept[out++] = draw[t];
							kept[out++] = draw[t + 1];
							kept[out++] = draw[t + 2];
						}
						continue;
					}
					if (cluster_state == 2 && !near_cluster_check)
					{
						/* every corner out on one plane: dropped, as below */
						if (!kept)
						{
							kept = list_take((draw_count * 2 + 3) & ~3);
							if (!kept)
							{
								kept_failed = 1;
								break;
							}
							memcpy(kept, draw, t * sizeof(*kept));
							out = t;
						}
						near_triangles_dropped++;
						continue;
					}

					for (k = 0; k < 3; k++)
					{
						const unsigned char *record = (const unsigned char *)vertices + skin->stride * draw[t + k];
						int known = draw[t + k] < CORNER_VERDICTS ? corner_verdict[draw[t + k]] : -1, code;

						if (known >= corner_stamp && known < corner_stamp + CORNER_STAMP_STEP)
							code = known - corner_stamp;
						else
						{
							const float *p = (const float *)(record + uv_at + 12);
							float at[3] = { 0.0f, 0.0f, 0.0f };
							int axis;

							probe_corners++;
							if (on_vfpu)
								skin_vfpu_corner(record, skin->weights, p, at);
							if (!on_vfpu || skin_vfpu_check)
							{
								float fpu[3] = { 0.0f, 0.0f, 0.0f };

								/* build 119: the three axes written out (they were a loop the
								release build doesn't unroll), so the FPU works on the three
								sums at once rather than one after another: each is the same
								expression as before, in the same order, so the same floats.
								"skinaxisloop": the loop, as before */
								if (skin_axis_loop)
								{
									for (slot = 0; slot < skin->weights; slot++)
										if (record[slot])
											for (axis = 0; axis < 3; axis++)
												fpu[axis] += record[slot] * (1.0f / 128.0f) * (depth_row[slot][axis][0] * p[0] +
													depth_row[slot][axis][1] * p[1] + depth_row[slot][axis][2] * p[2] +
													depth_row[slot][axis][3]);
								}
								else
									for (slot = 0; slot < skin->weights; slot++)
										if (record[slot])
										{
											const float (*row)[4] = depth_row[slot];

											fpu[0] += record[slot] * (1.0f / 128.0f) * (row[0][0] * p[0] + row[0][1] * p[1] +
												row[0][2] * p[2] + row[0][3]);
											fpu[1] += record[slot] * (1.0f / 128.0f) * (row[1][0] * p[0] + row[1][1] * p[1] +
												row[1][2] * p[2] + row[1][3]);
											fpu[2] += record[slot] * (1.0f / 128.0f) * (row[2][0] * p[0] + row[2][1] * p[1] +
												row[2][2] * p[2] + row[2][3]);
										}
								if (skin_axis_check)
								{
									/* "skinaxischeck": the loop as before, the sums compared bit for bit */
									float again[3] = { 0.0f, 0.0f, 0.0f };

									for (slot = 0; slot < skin->weights; slot++)
										if (record[slot])
											for (axis = 0; axis < 3; axis++)
												again[axis] += record[slot] * (1.0f / 128.0f) * (depth_row[slot][axis][0] * p[0] +
													depth_row[slot][axis][1] * p[1] + depth_row[slot][axis][2] * p[2] +
													depth_row[slot][axis][3]);
									skin_axis_checked++;
									if (memcmp(again, fpu, sizeof(again)))
										skin_axis_differ++;
								}
								if (on_vfpu)
								{
									/* "skinvfpucheck": the two verdicts compared */
									skin_vfpu_corners++;
									if (((camera_corner_out(at[0], at[1], at[2], limit) ? 32 : 0) | outside(at)) !=
										((camera_corner_out(fpu[0], fpu[1], fpu[2], limit) ? 32 : 0) | outside(fpu)))
										skin_vfpu_differ++;
								}
								else
									memcpy(at, fpu, sizeof(at));
							}
							code = (camera_corner_out(at[0], at[1], at[2], limit) ? 32 : 0) | outside(at);
							if (draw[t + k] < CORNER_VERDICTS)
								corner_verdict[draw[t + k]] = (unsigned short)(corner_stamp + code);
						}
						bits[k] = code & 31;
						behind |= code & 32;
					}
					/* build 119: "nearclustercheck": the run's verdict against the triangle's own */
					if ((cluster_state == 1 && behind) || (cluster_state == 2 && !(behind && (bits[0] & bits[1] & bits[2]))))
						near_cluster_mismatches++;
					if (behind)
					{
						if (!kept)
						{
							kept = list_take((draw_count * 2 + 3) & ~3);
							if (!kept)
							{
								kept_failed = 1;
								break;
							}
							memcpy(kept, draw, t * sizeof(*kept));
							out = t;
						}
						near_triangles_dropped++;
						if (bits[0] & bits[1] & bits[2])
							continue;
						/* cutstreamoff: the fixed buffer's own check, a triangle's
						worst case of room or the whole triangle lost */
						if (skin_cut && !cut_stream && cut_count >= MAXIMUM_MODEL_CUT_CORNERS - 9)
							cut_pieces_lost++;
						else if (skin_cut)
						{
							/* bent here and cut, as the level's are */
							float polygon[3][CORNER_FLOATS];
							int bits[3];

							memset(polygon, 0, sizeof(polygon));
							for (k = 0; k < 3; k++)
							{
								const unsigned char *record = (const unsigned char *)vertices + skin->stride * draw[t + k];
								float at[3], normal[3], uv[2], world[3];

								skin_corner(record, skin->weights, uv_at, bone, at, normal, uv);
								/* the GE's world space to world, for the camera-space corner */
								world[0] = at[0] + ge_eye[0];
								world[1] = at[1] + ge_eye[1];
								world[2] = at[2] + ge_eye[2];
								to_camera(world, polygon[k]);
								polygon[k][3] = uv[0];
								polygon[k][4] = uv[1];
								polygon[k][5] = normal[0];
								polygon[k][6] = normal[1];
								polygon[k][10] = normal[2];
								polygon[k][7] = at[0];
								polygon[k][8] = at[1];
								polygon[k][9] = at[2];
								bits[k] = outside(polygon[k]);
							}
							if (!(bits[0] & bits[1] & bits[2]))
							{
								if (!(bits[0] | bits[1] | bits[2]))
								{
									/* inside every plane, only inside the margin: drawn whole */
									cut_count += cut_store_whole(polygon, &pieces_store);
								}
								else
									cut_count += clip_triangle(polygon, bits[0] | bits[1] | bits[2], &pieces_store);
								skin_cut_triangles++;
							}
						}
						continue;
					}
					if (kept)
					{
						kept[out++] = draw[t];
						kept[out++] = draw[t + 1];
						kept[out++] = draw[t + 2];
					}
				}
				if (!whole_by_sphere)
					CENSUS(census_near(0, (long)(draw_count / 3)));
				if (probes_on && (!whole_by_sphere || near_sphere_check))
				{
					near_loop_us += probe_clock() - loop_start;
					near_loop_batches++;
					near_loop_triangles += (unsigned long)(draw_count / 3);
					near_corners_worked += probe_corners;
				}
				if (whole_by_sphere)
				{
					/* build 118: "nearspherecheck": the loop ran anyway, and must
					have found the batch whole */
					near_batches_by_sphere++;
					if (near_sphere_check && (kept || cut_count || kept_failed))
						near_sphere_mismatches++;
				}
				if (kept_failed)
				{
					memo_failed = 1;
					continue;
				}
				if (kept)
				{
					draw = kept;
					draw_count = out;
				}
				else
					near_batches_whole++;
				if (cut_count)
				{
					/* the bent pieces, after the batch's own triangles: GE vertices
					(texture coordinates, 8-bit normal, position) already in the
					bending part's space */
					unsigned char *pieces = list_take(cut_count * 24);
					int c;

					if (pieces)
					{
						for (c = 0; c < cut_count; c++)
						{
							unsigned char *record = pieces + 24 * c;
							const float *corner = cut_at(&pieces_store, c);
							float uv[2] = { corner[3], corner[4] };
							int axis;

							memcpy(record, uv, 8);
							for (axis = 0; axis < 3; axis++)
							{
								float v = corner[axis == 2 ? 10 : 5 + axis];

								record[8 + axis] = (unsigned char)(signed char)(v > 127.0f ? 127 : v < -127.0f ? -127 : (int)v);
							}
							record[11] = 0;
							memcpy(record + 12, &corner[7], 12);
						}
						if (draw_count)
							sceGumDrawArray(GU_TRIANGLES, GU_WEIGHTS(skin->weights) | GU_WEIGHT_8BIT | GU_TEXTURE_32BITF |
								GU_NORMAL_8BIT | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D, draw_count, draw,
								vertices);
						sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_NORMAL_8BIT | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
							cut_count, NULL, pieces);
						skin_cut_pieces += cut_count / 3;
						if (batch < 64)
						{
							near_memo[batch].draw = draw;
							near_memo[batch].draw_count = draw_count;
							near_memo[batch].pieces = pieces;
							near_memo[batch].cut_count = cut_count;
						}
						continue;
					}
				}
				if (batch < 64)
				{
					near_memo[batch].draw = draw;
					near_memo[batch].draw_count = draw_count;
					near_memo[batch].pieces = NULL;
					near_memo[batch].cut_count = 0;
				}
				if (!draw_count)
					continue;
			}
			sceGumDrawArray(GU_TRIANGLES, GU_WEIGHTS(skin->weights) | GU_WEIGHT_8BIT | GU_TEXTURE_32BITF |
				GU_NORMAL_8BIT | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D, draw_count, draw,
				vertices);
		}
		if (near && !from_memo)
		{
			/* every batch worked out (none cut short by a full list): the
			part's other passes draw from this */
			near_memo_skin = skin;
			near_memo_complete = !memo_failed && batch == skin->batch_count && skin->batch_count <= 64;
		}
	}
	else
	{
		if (index_count > 0)
			sceGumDrawArray(strip ? GU_TRIANGLE_STRIP : GU_TRIANGLES,
				(part_k14 != 0.0f ? GU_TEXTURE_16BIT | GU_NORMAL_8BIT | GU_VERTEX_16BIT :
				GU_TEXTURE_32BITF | GU_NORMAL_8BIT | GU_VERTEX_32BITF) | GU_INDEX_16BIT | GU_TRANSFORM_3D,
				(int)index_count, indices, vertices);
		if (model_cut_count > 0 && model_cut_vertices)
			sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_NORMAL_8BIT | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
				model_cut_count, NULL, model_cut_vertices);
	}
}

/* a rigid part's corner, as plane bits (outside(): near, sides; with
"modeldrop", 1 when behind the near test's margin), worked out once a part
and kept in corner_verdict. Build 107: the part's matrix and the camera's
axes made one transform for the part (rigid_rows, rigid_rows_for), so a
corner costs one three-row product instead of two (model_to_camera).
Rounded differently, so a corner exactly on a plane may fall the other way.
"nearcheck" in keep.txt: both, and the corners whose verdicts differ
counted in the log */
static void rigid_rows_for(const ScePspFMatrix4 *model)
{
	int axis;

	if (near_check < 0)
		near_check = psp_keep("nearcheck");
	for (axis = 0; axis < 3; axis++)
	{
		const float *d = axis == 0 ? camera_right : axis == 1 ? camera_up : camera_forward;

		rigid_rows[axis][0] = model->x.x * d[0] + model->x.y * d[1] + model->x.z * d[2];
		rigid_rows[axis][1] = model->y.x * d[0] + model->y.y * d[1] + model->y.z * d[2];
		rigid_rows[axis][2] = model->z.x * d[0] + model->z.y * d[1] + model->z.z * d[2];
		rigid_rows[axis][3] = model->w.x * d[0] + model->w.y * d[1] + model->w.z * d[2] + ge_eye_camera[axis];
	}
}

static int rigid_corner_bits(const void *vertices, unsigned short corner, const ScePspFMatrix4 *model, float limit)
{
	int known = corner < CORNER_VERDICTS ? corner_verdict[corner] : -1, bits;
	const float *p;
	float camera_point[3], point[3];

	if (known >= corner_stamp && known < corner_stamp + CORNER_STAMP_STEP)
		return known - corner_stamp;
	p = record_point(vertices, corner, point);
	camera_point[0] = rigid_rows[0][0] * p[0] + rigid_rows[0][1] * p[1] + rigid_rows[0][2] * p[2] + rigid_rows[0][3];
	camera_point[1] = rigid_rows[1][0] * p[0] + rigid_rows[1][1] * p[1] + rigid_rows[1][2] * p[2] + rigid_rows[1][3];
	camera_point[2] = rigid_rows[2][0] * p[0] + rigid_rows[2][1] * p[1] + rigid_rows[2][2] * p[2] + rigid_rows[2][3];
	bits = model_drop ? (camera_corner_out(camera_point[0], camera_point[1], camera_point[2], limit) ? 1 : 0) :
		outside(camera_point);
	if (near_check)
	{
		float two_step[3];

		model_to_camera(model, p, two_step);
		near_check_corners++;
		if (bits != (model_drop ? (camera_corner_out(two_step[0], two_step[1], two_step[2], limit) ? 1 : 0) :
			outside(two_step)))
			near_check_differ++;
	}
	if (corner < CORNER_VERDICTS)
		corner_verdict[corner] = (unsigned short)(corner_stamp + bits);
	return bits;
}

/* a triangle's corners, from a list or a strip (0: a strip's empty triangle) */
static int part_triangle(const unsigned short *indices, int strip, long triangle, unsigned short *corner)
{
	if (strip)
	{
		corner[0] = indices[triangle];
		corner[1] = indices[triangle + ((triangle & 1) ? 2 : 1)];
		corner[2] = indices[triangle + ((triangle & 1) ? 1 : 2)];
		return !(corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2]);
	}
	corner[0] = indices[3 * triangle];
	corner[1] = indices[3 * triangle + 1];
	corner[2] = indices[3 * triangle + 2];
	return 1;
}

/* build 107: whether every triangle of a rigid part near the eye is wholly
inside, so the part draws its own indices with no copy in the list. A model
is "near" when its bounding sphere reaches the near plane or past the GE's
guard band, which a large model's (a Pelican's) does while most of its parts'
triangles are well inside: b30 beside the Pelicans copied up to 160 KB of
kept indices and cut corners into the list a frame. The verdicts stay in
corner_verdict for the copy, when one is needed */
static int rigid_part_whole(const void *vertices, const unsigned short *indices, long index_count, int strip,
	const ScePspFMatrix4 *model, float limit)
{
	long triangle, triangles = strip ? index_count - 2 : index_count / 3;
	unsigned short corner[3];

	if (model_drop < 0)
		model_drop = psp_keep("modeldrop");
	corner_verdict_reset();
	rigid_rows_for(model);
	near_parts++;
	CENSUS(census_near(1, triangles));
	for (triangle = 0; triangle < triangles; triangle++)
		if (part_triangle(indices, strip, triangle, corner) &&
			(rigid_corner_bits(vertices, corner[0], model, limit) | rigid_corner_bits(vertices, corner[1], model, limit) |
			rigid_corner_bits(vertices, corner[2], model, limit)))
			return 0;
	near_parts_whole++;
	return 1;
}

/* build 119: the rigid near path in runs ("NR"), as the bending one's
(near_cluster_verdict). A large rigid part at the eye (c20's and b30's: the
near filter 2.8-3.1 ms a frame on the PSP) has every corner worked out to
find the triangles to keep, drop or cut. Its triangles are taken in runs of
NEAR_CLUSTER_TRIANGLES, each with a sphere in the part's own space, worked
out once and kept in a small table by the part's buffers (as multi_memo
keeps its answers, by the buffers' geometry-cache generations). Through
the part's matrix (rigid_rows; its largest stretch by batch_sphere_inside's
bound) a run wholly inside the planes is kept with no corner worked out, one
wholly beyond one plane is dropped: each triangle's own verdict would be
the same (outside(): NEAR_CLIP and the guard planes). "rigidrunsfull": every
corner as before; "rigidrunscheck": the corners anyway, compared */
#define RIGID_RUN_PARTS 16
static struct rigid_runs
{
	const void *vertices;
	const unsigned short *indices;
	long index_count;
	int strip, count;
	unsigned int vertices_generation, indices_generation, used;
	float (*spheres)[4];
} rigid_runs[RIGID_RUN_PARTS];
static unsigned int rigid_runs_clock = 0;

/* the part's runs' spheres, from the table or worked out now (NULL: no room) */
static struct rigid_runs *rigid_runs_for(const void *vertices, const unsigned short *indices, long index_count, int strip)
{
	long triangles = strip ? index_count - 2 : index_count / 3, t;
	unsigned int vertices_generation = psp_geo_generation(vertices), indices_generation = psp_geo_generation(indices);
	struct rigid_runs *slot = NULL;
	int k, count;

	for (k = 0; k < RIGID_RUN_PARTS; k++)
	{
		struct rigid_runs *r = &rigid_runs[k];

		if (r->spheres && r->vertices == vertices && r->indices == indices && r->index_count == index_count &&
			r->strip == strip && r->vertices_generation == vertices_generation && r->indices_generation == indices_generation)
		{
			r->used = ++rigid_runs_clock;
			return r;
		}
		if (!slot || (!r->spheres && slot->spheres) || (r->spheres && slot->spheres && r->used < slot->used))
			slot = r;
	}
	if (triangles <= 0)
		return NULL;
	if (PSP_ME_WORK())
	{
		runs_want(NULL, NULL, vertices, indices, index_count, strip);
		return NULL;
	}
	count = (int)((triangles + NEAR_CLUSTER_TRIANGLES - 1) / NEAR_CLUSTER_TRIANGLES);
	if (slot->spheres)
	{
		rigid_runs_bytes -= (unsigned long)slot->count * sizeof(float[4]);
		free(slot->spheres);
		slot->spheres = NULL;
	}
	slot->spheres = cache_malloc(count * sizeof(float[4]));
	if (!slot->spheres)
		return NULL;
	rigid_runs_bytes += (unsigned long)count * sizeof(float[4]);
	rigid_runs_made++;
	slot->vertices = vertices;
	slot->indices = indices;
	slot->index_count = index_count;
	slot->strip = strip;
	slot->count = count;
	slot->vertices_generation = vertices_generation;
	slot->indices_generation = indices_generation;
	slot->used = ++rigid_runs_clock;
	for (k = 0; k < count; k++)
	{
		float low[3] = { 0.0f, 0.0f, 0.0f }, high[3] = { 0.0f, 0.0f, 0.0f }, reach = 0.0f;
		long first = (long)k * NEAR_CLUSTER_TRIANGLES, last = first + NEAR_CLUSTER_TRIANGLES;
		int any = 0, axis, c;

		if (last > triangles)
			last = triangles;
		/* every corner of the run's triangles, the empty strip ones too (more
		corners only make the sphere larger) */
		for (t = first; t < last; t++)
			for (c = 0; c < 3; c++)
			{
				unsigned short v = strip ? indices[t + c] : indices[3 * t + c];
				float point[3];
				const float *p = record_point(vertices, v, point);

				for (axis = 0; axis < 3; axis++)
				{
					if (!any || p[axis] < low[axis]) low[axis] = p[axis];
					if (!any || p[axis] > high[axis]) high[axis] = p[axis];
				}
				any = 1;
			}
		for (axis = 0; axis < 3; axis++)
			slot->spheres[k][axis] = (low[axis] + high[axis]) * 0.5f;
		for (t = first; t < last; t++)
			for (c = 0; c < 3; c++)
			{
				unsigned short v = strip ? indices[t + c] : indices[3 * t + c];
				float point[3];
				const float *p = record_point(vertices, v, point);
				float dx = p[0] - slot->spheres[k][0], dy = p[1] - slot->spheres[k][1], dz = p[2] - slot->spheres[k][2];
				float d = dx * dx + dy * dy + dz * dz;

				if (d > reach)
					reach = d;
			}
		slot->spheres[k][3] = sqrtf(reach);
	}
	return slot;
}

/* the largest stretch of rigid_rows' 3x3 part (batch_sphere_inside's bound) */
static float rigid_rows_stretch(void)
{
	float stretch = 0.0f;
	int i, j;

	for (i = 0; i < 3; i++)
	{
		float sum = 0.0f;

		for (j = 0; j < 3; j++)
			sum += fabsf(rigid_rows[i][0] * rigid_rows[j][0] + rigid_rows[i][1] * rigid_rows[j][1] +
				rigid_rows[i][2] * rigid_rows[j][2]);
		if (sum > stretch)
			stretch = sum;
	}
	return sqrtf(stretch);
}

/* a run's verdict: 1 every corner inside, 2 every corner beyond one plane, 0 either */
static int rigid_run_verdict(const float *sphere, float stretch)
{
	float centre[3], radius = sphere[3] * stretch;
	int i;

	for (i = 0; i < 3; i++)
		centre[i] = rigid_rows[i][0] * sphere[0] + rigid_rows[i][1] * sphere[1] + rigid_rows[i][2] * sphere[2] + rigid_rows[i][3];
	if (sphere_inside(centre, radius, NEAR_CLIP))
		return 1;
	return sphere_beyond(centre, radius, NEAR_CLIP) ? 2 : 0;
}

static void psp_ge_draw_model_body(void *vertices, int vertex_count, int uncompressed, const unsigned short *indices,
	long index_count, int strip, unsigned int color, unsigned int base_key, float u_scale, float v_scale,
	int alpha_tested)
{
	ScePspFMatrix4 model;
	/* "nobackpass" (recon): a two-sided part's back faces left out */
	static int back_pass_off = -1;
	int base_alpha = 0, textured, soft = 0, two_sided = (alpha_tested & 2) != 0;
	struct skin_part *skin = NULL;
	unsigned int part_ambient;
	float shine[3] = { 0.0f, 0.0f, 0.0f };
	int shiny, env, sheen, sheen_count;

	alpha_tested &= 1;
	model_cut_count = 0;
	model_cut_vertices = NULL;
	near_memo_skin = NULL; /* build 106: model_geometry's lists are this part's */
	near_memo_complete = 0;
	bone_memo_skin = NULL; /* build 107: and its bone sub-lists */
	if (!frame_open || index_count < 3 || !vertices || !indices || !model_nodes || model_node_count <= 0 ||
		vertex_count <= 0 || list_full() || (in_sky && magenta == 1))
		return;
	if (model_size_floor > 0.0f && model_pixels < model_size_floor && !model_first_person && !in_sky)
	{
		model_floor_left_out++;
		return;
	}
	part_start();
	part_k14 = 0.0f;
	convert_float_only = model_first_person || in_sky;
	if (!model_convert(vertices, vertex_count, uncompressed, indices, (int)index_count, strip, &skin))
		return;
	part_k14 = convert_k;
	part_add(4);
	sceGumMatrixMode(GU_MODEL);
	if (skin)
		sceGumLoadIdentity();
	else
	{
		/* one node: the first vertex's (stored times three, for the Xbox's
		vertex shader constants) */
		node_matrix(vertices, indices, &model);
		if (part_k14 != 0.0f)
		{
			/* the GE reads q / 32768 */
			ScePspFMatrix4 scaled = model;
			float s = part_k14 * 32768.0f;

			scaled.x.x *= s; scaled.x.y *= s; scaled.x.z *= s;
			scaled.y.x *= s; scaled.y.y *= s; scaled.y.z *= s;
			scaled.z.x *= s; scaled.z.y *= s; scaled.z.z *= s;
			sceGumLoadMatrix(&scaled);
		}
		else
			sceGumLoadMatrix(&model);
		part_add(5);
		if (model_near && !model_first_person &&
			!rigid_part_whole(vertices, indices, index_count, strip, &model, projection_near * 1.25f))
		{
			/* build 38: near the eye, only the triangles wholly in front of
			the near plane (with a margin), as a list */
			unsigned short *kept = list_take((int)((strip ? index_count * 3 : index_count) * 2 + 3) & ~3);
			long triangle, triangles = strip ? index_count - 2 : index_count / 3, count = 0;
			float limit = projection_near * 1.25f;

			/* build 59: back to the world's matrix on the way out, as the end
			of this function does. Returning with this part's matrix still
			loaded moved every level draw after it by the part's placement:
			the whole level vanished while a model near the eye had all its
			triangles left out (b58's spot at 33.2 -90.2, and the flicker
			getting into the warthog) */
			if (!kept)
			{
				world_model();
				return;
			}
			/* build 60: each corner's planes as bits (corner_verdict holds
			stamp + bits); a triangle wholly in front is kept as it is, one
			wholly outside a plane is dropped, and one across the near plane or
			the guard band is cut to its inside on the CPU, as the level's are,
			and drawn from model_cut (model_geometry). Dropped whole as before
			(build 38 to 59), the triangle of a rock or wall nearest the eye
			went missing whenever a corner reached past the screen's edge
			(b59's rock shots), and a part with every triangle so placed
			disappeared. "modeldrop": as before */
			/* (rigid_part_whole has reset corner_verdict, set rigid_rows and
			worked out the corners up to the first triangle not wholly inside) */
			/* build 119: the runs (rigid_run_verdict) */
			struct rigid_runs *runs = NULL;
			float stretch = 0.0f;
			int run_state = 0;
			long run_end = 0;
			struct cut_store pieces_store;

			cut_store_begin(&pieces_store, model_cut, MAXIMUM_MODEL_CUT_CORNERS, CUT_RIGID);

			if (ge_hot.rigid_runs_full < 0)
			{
				ge_hot.rigid_runs_full = psp_keep("rigidrunsfull");
				rigid_runs_check = psp_keep("rigidrunscheck");
			}
			if (!ge_hot.rigid_runs_full && !model_drop)
			{
				runs = rigid_runs_for(vertices, indices, index_count, strip);
				if (runs)
					stretch = rigid_rows_stretch();
			}
			for (triangle = 0; triangle < triangles; triangle++)
			{
				unsigned short corner[3];
				int k, bits[3];

				if (runs && triangle >= run_end)
				{
					long r = triangle / NEAR_CLUSTER_TRIANGLES;

					run_end = (r + 1) * NEAR_CLUSTER_TRIANGLES;
					run_state = r < runs->count ? rigid_run_verdict(runs->spheres[r], stretch) : 0;
					if (run_state == 1)
						rigid_runs_kept++;
					else if (run_state == 2)
						rigid_runs_dropped++;
					else
						rigid_runs_mixed++;
				}
				if (!part_triangle(indices, strip, triangle, corner))
					continue;
				if (run_state == 1 && !rigid_runs_check)
				{
					kept[3 * count] = corner[0];
					kept[3 * count + 1] = corner[1];
					kept[3 * count + 2] = corner[2];
					count++;
					continue;
				}
				if (run_state == 2 && !rigid_runs_check)
				{
					near_triangles_dropped++;
					continue;
				}
				for (k = 0; k < 3; k++)
					bits[k] = rigid_corner_bits(vertices, corner[k], &model, limit);
				/* "rigidrunscheck": the run's verdict against the triangle's own */
				if ((run_state == 1 && (bits[0] | bits[1] | bits[2])) || (run_state == 2 && !(bits[0] & bits[1] & bits[2])))
					rigid_runs_mismatches++;
				if (!(bits[0] | bits[1] | bits[2]))
				{
					kept[3 * count] = corner[0];
					kept[3 * count + 1] = corner[1];
					kept[3 * count + 2] = corner[2];
					count++;
					continue;
				}
				near_triangles_dropped++;
				if (model_drop || (bits[0] & bits[1] & bits[2]))
					continue;
				{
					float polygon[3][CORNER_FLOATS];

					for (k = 0; k < 3; k++)
					{
						const unsigned char *record = (const unsigned char *)vertices + 24 * corner[k];
						const float *uv = (const float *)record, *p = (const float *)(record + 12);
						const unsigned char *normal = record + 8;
						float point[3], ge_uv[2], ge_point[3];

						if (part_k14 != 0.0f)
						{
							/* the pieces are drawn through the same scaled matrix and
							texture offset, so they keep the GE's own units */
							const unsigned short *t;
							const short *q;
							int a;

							record = (const unsigned char *)vertices + 14 * corner[k];
							t = (const unsigned short *)record;
							q = (const short *)(record + 8);
							normal = record + 4;
							for (a = 0; a < 3; a++)
							{
								point[a] = q[a] * part_k14;
								ge_point[a] = q[a] * (1.0f / 32768.0f);
							}
							ge_uv[0] = t[0] * (1.0f / 32768.0f);
							ge_uv[1] = t[1] * (1.0f / 32768.0f);
							p = point;
							uv = ge_uv;
						}
						model_to_camera(&model, p, polygon[k]);
						polygon[k][3] = uv[0];
						polygon[k][4] = uv[1];
						polygon[k][5] = (signed char)normal[0];
						polygon[k][6] = (signed char)normal[1];
						polygon[k][10] = (signed char)normal[2];
						polygon[k][7] = part_k14 != 0.0f ? ge_point[0] : p[0];
						polygon[k][8] = part_k14 != 0.0f ? ge_point[1] : p[1];
						polygon[k][9] = part_k14 != 0.0f ? ge_point[2] : p[2];
					}
					model_cut_count += clip_triangle(polygon, bits[0] | bits[1] | bits[2], &pieces_store);
				}
			}
			if (model_cut_count)
			{
				/* the cut corners as GE vertices, in the part's own space */
				unsigned char *out = list_take(model_cut_count * 24);
				int c;

				if (out)
				{
					for (c = 0; c < model_cut_count; c++)
					{
						unsigned char *record = out + 24 * c;
						const float *corner = cut_at(&pieces_store, c);
						float uv[2] = { corner[3], corner[4] };

						memcpy(record, uv, 8);
						record[8] = (unsigned char)(signed char)corner[5];
						record[9] = (unsigned char)(signed char)corner[6];
						record[10] = (unsigned char)(signed char)corner[10];
						record[11] = 0;
						memcpy(record + 12, &corner[7], 12);
					}
					model_cut_vertices = out;
				}
				else
					model_cut_count = 0;
			}
			if (!count && !model_cut_count && !pieces_store.dropped)
			{
				world_model();
				return;
			}
			indices = kept;
			index_count = count * 3;
			strip = 0;
			part_add(6);
		}
	}
	textured = psp_tex_bind(base_key, &base_alpha);
#ifdef PSP_DEBUG_TOOLS
	/* a model's box: its sphere's (psp_ge_model_shape), the whole screen
	without one (first person, the sky) */
	if (sdump_wanted())
	{
		float box[4], c[3];
		char text[64];
		int corner;

		sdump_box_start(box);
		for (corner = 0; corner < 4; corner++)
		{
			float r = model_sphere_radius > 0.0f ? model_sphere_radius : 0.0f;

			c[0] = model_sphere[0] + (corner & 1 ? r : -r);
			c[1] = model_sphere[1] + (corner & 2 ? r : -r);
			c[2] = model_sphere_radius > 0.0f ? model_sphere[2] - r : -1.0f;
			sdump_box_add(box, c);
		}
		if (sdump_box_text(box, text, sizeof(text)))
			platform_log("sdump: model %s map %s alpha tested %d map alpha %d sky %d first person %d indices %ld",
				text, sdump_map_name(base_key), alpha_tested, base_alpha, in_sky, model_first_person, index_count);
	}
#endif
	if (textured)
	{
		/* unlit for now: the texture dimmed toward the level's lighting */
		sceGuEnable(GU_TEXTURE_2D);
		sceGuTexFunc(GU_TFX_MODULATE, base_alpha && alpha_tested ? GU_TCC_RGBA : GU_TCC_RGB);
		sceGuTexWrap(GU_REPEAT, GU_REPEAT);
		model_tex_scale(model_u_scale * (u_scale != 0.0f ? u_scale : 1.0f), model_v_scale * (v_scale != 0.0f ? v_scale : 1.0f));
		/* lit by the GE from the game's lighting; this is the material colour */
		sceGuColor(0xFFFFFFFF);
		if (base_alpha && alpha_tested)
		{
			sceGuAlphaFunc(GU_GREATER, 0x7F, 0xFF);
			sceGuEnable(GU_ALPHA_TEST);
		}
	}
	else
		sceGuColor(color);
	part_ambient = model_ambient;
	shiny = textured && model_camouflage <= 0.0f && !in_sky && reflection_strength(shine);
	if (reflection_chief == 1)
	{
		visor_drawn++;
		visor_textured += textured != 0;
		visor_shiny += shiny != 0;
		visor_pixels = model_pixels;
	}
	env = shiny && env_part(shine);
	sheen = shiny && reflection_words.sheen && reflection_chief == 2 && change_key && !env &&
		psp_tex_reflection_mask(change_key, &sheen_count) != NULL;
	if (textured && model_camouflage <= 0.0f && !in_sky && !env && !sheen)
		model_ambient = ambient_with_reflection(base_key);
	part_specular = shiny && reflection_words.specular > 0 ? 0xFF000000u |
		((unsigned int)PSP_CLAMP_255((int)(shine[2] * reflection_words.specular_gain * 255.0f)) << 16) |
		((unsigned int)PSP_CLAMP_255((int)(shine[1] * reflection_words.specular_gain * 255.0f)) << 8) |
		(unsigned int)PSP_CLAMP_255((int)(shine[0] * reflection_words.specular_gain * 255.0f)) : 0xFF000000u;
	model_lighting(1);
	if (camo_shimmer < 0)
		camo_shimmer = psp_keep("camoshimmer");
	if (model_camouflage > 0.0f && camo_shimmer > 0 && textured)
	{
		/* "camoshimmer" (a look call, off by default): the Xbox bends what
		lies behind a camouflaged model, which the GE can't sample; in its
		place the model's own map, unlit, added faintly over the frame and
		drifting a little each frame, so the outline catches the eye as the
		Xbox's shimmer does instead of vanishing against a dark room. One
		pass, as the faded draw it replaces */
		unsigned int level = (unsigned int)(model_camouflage * CAMO_SHIMMER_LEVEL);
		float drift = (float)(taa_frame & 255) * (1.0f / 256.0f);

		model_lighting(0);
		sceGuColor(0xFF000000u | ((level + level / 4) << 16) | (level << 8) | level);
		sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
		sceGuTexOffset(drift, drift * 0.5f);
		sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
		ge_blend(1);
		sceGuDepthMask(GU_TRUE);
		camo_shimmer_parts++;
	}
	else if (model_camouflage > 0.0f)
	{
		/* the model's own colour faded to a trace (15% at full camouflage) */
		int alpha = (int)((1.0f - model_camouflage * 0.85f) * 255.0f);

		sceGuColor(((unsigned int)PSP_CLAMP_255(alpha) << 24) | 0xFFFFFF);
		sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
		sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
		ge_blend(1);
		sceGuDepthMask(GU_TRUE);
	}
	/* build 36: back faces culled, as the Xbox culls them (D3DCULL_CCW). Drawn,
	a two-sided leaf's two triangles share their 16-bit depth and the lit one
	and the dark one took turns as the plant swayed (b35). A two-sided
	shader's back faces are then drawn with the light from the other side,
	as the Xbox's second pass flips the normals */
	if (model_cull < 0)
		model_cull = psp_keep("nocull") ? 0 : psp_keep("modelflip") ? 2 : 1;
	sceGuFrontFace(model_cull == 2 ? (LEVEL_FRONT == GU_CW ? GU_CCW : GU_CW) : LEVEL_FRONT);
	if (model_cull)
		sceGuEnable(GU_CULL_FACE);
	part_add(7);
	model_geometry(vertices, indices, index_count, strip, skin, &model);
	if (foliage_two_pass < 0)
		foliage_two_pass = psp_keep("foliagetwopass");
	soft = foliage_two_pass && textured && base_alpha && alpha_tested && model_camouflage <= 0.0f && !in_sky;
	if (soft)
		foliage_soft_pass(vertices, indices, index_count, strip, skin, &model);
	part_add(8);
	if (back_pass_off < 0)
		back_pass_off = psp_keep("nobackpass");
	if (two_sided && model_cull && model_camouflage <= 0.0f && !back_pass_off)
	{
		int index;

		sceGuFrontFace(model_cull == 2 ? LEVEL_FRONT : (LEVEL_FRONT == GU_CW ? GU_CCW : GU_CW));
		for (index = 0; index < model_light_count; index++)
		{
			ScePspFVector3 flipped = { -model_light_toward[index].x, -model_light_toward[index].y,
				-model_light_toward[index].z };

			sceGuLight(index, GU_DIRECTIONAL, GU_DIFFUSE, &flipped);
		}
		/* build 117: a point light can't be flipped the same way (it lights
		by place): the back faces go without */
		for (index = 0; index < model_point_count; index++)
			sceGuDisable(GU_LIGHT2 + index);
		lit_points_maybe_on = 0; /* light 3 was already off when only light 2 was used */
		lit_forget(1); /* build 114: statememo */
		model_geometry(vertices, indices, index_count, strip, skin, &model);
		if (soft)
			foliage_soft_pass(vertices, indices, index_count, strip, skin, &model);
		sceGuFrontFace(LEVEL_FRONT);
		two_sided_draws++;
	}
	sceGuDisable(GU_CULL_FACE);
	model_lighting(0);
	model_ambient = part_ambient;
	/* passes 2 and 3: the change colour multiplied in and the
	self-illumination added, where pass 1 drew */
	if (textured && change_key && model_camouflage <= 0.0f && !in_sky)
	{
		/* "noglowpass" (recon): which brightness on a model is its self-illumination */
		static int glow_off = -1;

		if (change_color != 0xFFFFFFFF && change_pass(vertices, indices, index_count, strip, skin, &model))
		{
			change_passes++;
			CENSUS(census_tint(base_key, change_key, change_color));
		}
		if (glow_off < 0)
			glow_off = psp_keep("noglowpass");
		glow_base_key = base_key; /* the clamp's base (glow_map_pass) */
		if (glow_color && !glow_off)
		{
			if (!psp_tex_has(glow_key))
				glow_unmapped++;
			else if (glow_map_pass(vertices, indices, index_count, strip, skin, &model))
				glow_passes++;
		}
	}
	if (sheen && sheen_pass(vertices, indices, index_count, strip, skin, &model))
		sheen_passes++;
	if (env)
	{
		static int visor_logged = 0;

		if (reflection_chief == 1 && !visor_logged && !PSP_ME_WORK())
		{
			visor_logged = 1;
			platform_log("GE: chiefvisor: the visor's pass at strength %.2f %.2f %.2f x gain %d%%, %.0f pixels across",
				shine[0], shine[1], shine[2], reflection_words.chief_visor_gain, model_pixels);
		}
		if (reflection_env_pass(vertices, indices, index_count, strip, skin, &model, shine))
			env_passes++;
		else
			env_unbound++;
		sceGuTexScale(model_u_scale * (u_scale != 0.0f ? u_scale : 1.0f), model_v_scale * (v_scale != 0.0f ? v_scale : 1.0f));
	}
	if (textured && model_detail_key && model_camouflage <= 0.0f && !in_sky)
		model_detail_pass(vertices, indices, index_count, strip, skin, &model);
	if (model_camouflage > 0.0f)
	{
		ge_blend(0);
		sceGuDepthMask(GU_FALSE);
		if (camo_shimmer > 0 && textured)
			sceGuTexOffset(0.0f, 0.0f);
	}
	if (textured)
	{
		sceGuDisable(GU_ALPHA_TEST);
		sceGuTexScale(1.0f, 1.0f);
		sceGuDisable(GU_TEXTURE_2D);
	}
	if (part_k14 != 0.0f)
		sceGuTexOffset(0.0f, 0.0f);
	world_model();
	model_cut_count = 0;
	model_cut_vertices = NULL;
	model_draws_this_frame++;
	part_add(9);
}

/* "modelpretouch" (on; modelpretouchoff): a model part's buffer converted as
its record is written, when the geometry cache has read it afresh since its
last conversion. The Media Engine's replay can't convert one: it asked for
it at the frame's end and left the part out of that frame, and with every
part of a model in one block read again the whole model went for a frame or
two (b30's opening cutscene: the Pelican over the camera, B8). The frame in
flight is settled first, as pretouch does, since the replay reads the table
a conversion may change; most frames convert nothing here */
static int model_pretouch = -1;
static unsigned long model_pretouches = 0, model_pretouch_missed = 0;
static unsigned long pretouch_chunk_skips = 0;

/* the Media Engine still drawing from this frame's records or the last's
(mechunk's pieces handed to it while the frame is being written, or a
frame out that the sync didn't wait for). A conversion here rewrites a
buffer in place and may free skinning plans (the sweep, a stale part's
plan) that it draws from: the PSP-3000's marines drew with spikes across
the screen from their heads (B22, modelpretouch on: 40213200628 run 1);
the emulator runs the ME's work inside the hand-over, so its frames never
showed it. Such a draw is left to the frame's end, as before */
static int pretouch_me_busy(void)
{
	if (inflight_me_out || (chunk_frame && chunk_published))
	{
		pretouch_chunk_skips++;
		return 1;
	}
	return 0;
}

static int model_converted_now(const void *vertices)
{
	unsigned int slot = ((unsigned int)vertices >> 4) % CONVERTED_BUFFERS, tries = 0;

	while (converted[slot] && converted[slot] != vertices)
	{
		slot = (slot + 1) % CONVERTED_BUFFERS;
		if (++tries >= CONVERTED_BUFFERS)
			return 1; /* the table full: model_convert says so itself */
	}
	return converted[slot] && converted_generation[slot] == psp_geo_generation(vertices);
}

void psp_ge_draw_model(void *vertices, int vertex_count, int uncompressed, const unsigned short *indices,
	long index_count, int strip, unsigned int color, unsigned int base_key, float u_scale, float v_scale,
	int alpha_tested)
{
	GE_BENCH_SKIP();
	GE_PRETOUCH(base_key, TXPT_MODEL_BASE);
	if (PRETOUCHING() && vertices && vertex_count > 0)
	{
		if (model_pretouch < 0)
			model_pretouch = !psp_keep("modelpretouchoff");
		if (model_pretouch && !model_converted_now(vertices))
		{
			struct skin_part *skin;

			if (inflight_room_away)
				inflight_room_park();
			if (inflight_pending)
				inflight_sync();
			if (!pretouch_me_busy())
			{
				model_pretouches++;
				if (!model_convert(vertices, vertex_count, uncompressed, indices, (int)index_count, strip, &skin))
					model_pretouch_missed++;
			}
		}
	}
	GE_DEFER(defer_draw_model(vertices, vertex_count, uncompressed, indices, index_count, strip, color, base_key,
		u_scale, v_scale, alpha_tested));
	ge_kind(in_sky ? KIND_SKY : KIND_MODEL); /* build 103 */
	unsigned int start = ge_clock();

	ge_category = GE_SKIP_MODELS;
	model_part_calls++;
	psp_ge_draw_model_body(vertices, vertex_count, uncompressed, indices, index_count, strip, color, base_key, u_scale, v_scale, alpha_tested);
	ge_category = 0;
	ge_entry_us[1] += ge_clock() - start;
}

/* the frame to the screen: the CPU's writes (the triangle lists, the
display list) out of its data cache first, since the GE reads memory */
/* the sky (build 21): drawn at the far plane (the bottom of the reversed depth
range), without writing depth or fog, so the level covers it wherever the
level is */
static unsigned long sky_draws = 0, sky_parts_start = 0, sky_parts = 0;
static int in_sky = 0;

void psp_ge_sky(int on)
{
	GE_DEFER(defer_words(DEFER_SKY, 1, (unsigned int)on, 0, 0, 0, 0, 0, 0, 0)); /* build 112: gedefer */
	if (!frame_open)
		return;
	in_sky = on;
	if (on)
		sky_parts_start = model_draws_this_frame;
	else
		sky_parts += model_draws_this_frame - sky_parts_start;
	if (on)
	{
		sky_draws++;
		sceGuDepthRange(1, 0);
		sceGuDepthMask(GU_TRUE);
		sceGuDisable(GU_FOG);
	}
	else
	{
		sceGuDepthRange(WORLD_DEPTH_NEAR, 0);
		sceGuDepthMask(GU_FALSE);
		sceGuEnable(GU_FOG);
	}
}

/* ---------- a new map or BSP (build 27)

The level streams are copies of a BSP's vertices, and the model buffers
were rewritten where the map's tags were loaded; the next BSP or map loads
at the same addresses, so what was made from the last one is let go. */

static void overlay_backing_forget(void);

void psp_ge_forget_level(void)
{
	GE_DEFER_DRAIN(); /* build 112: gedefer */
	int index;

	rso_forget();

	if (vm_on > 0)
		vm_on = -1; /* build 116: vrammodels' tables again at the next frame */
	for (index = 0; index < LEVEL_STREAMS; index++)
	{
		stream_free(level_streams[index].base);
		stream_free(level_streams[index].lightmap);
		copy_free(level_streams[index].normals);
		copy_free(level_streams[index].radiosity);
		copy_free(level_streams[index].compact_base);
		copy_free(level_streams[index].compact_lightmap);
	}
	memset(level_streams, 0, sizeof(level_streams));
	vram_vertex_next = 0; /* build 103 */
	/* build 64: the overlays' on-a-wall answers belong to this BSP */
	overlay_backing_forget();
	stream_bytes = 0;
	stream_waiting_count = 0;
	platform_log("GE: the level's vertex copies let go (a new BSP or map)");
}

static void transparent_multi_forget(void);

void psp_ge_forget_map(void)
{
	GE_DEFER_DRAIN(); /* build 112: gedefer */
	int index;

	for (index = 0; index < CONVERTED_BUFFERS; index++)
		skin_free(skinned[index]);
	memset(skinned, 0, sizeof(skinned));
	/* build 119: the rigid runs' spheres and the see-through parts' node
	answers (M1, build 118) are kept by buffer address and generation, and a
	part in the tags has generation 0 in every map: forgotten with the map */
	for (index = 0; index < RIGID_RUN_PARTS; index++)
		if (rigid_runs[index].spheres)
		{
			rigid_runs_bytes -= (unsigned long)rigid_runs[index].count * sizeof(float[4]);
			free(rigid_runs[index].spheres);
		}
	memset(rigid_runs, 0, sizeof(rigid_runs));
	transparent_multi_forget();
	memset(converted, 0, sizeof(converted));
	memset(converted_generation, 0, sizeof(converted_generation));
	converted_used = 0;
	converted_sweep_mark = CONVERTED_BUFFERS / 4; /* build 112 */
	converted_count = reconverted_count = skinned_parts = skin_batches = skin_bytes = skin_failures = 0;
	psp_ge_forget_level();
	carve_close();
	/* build 87: the last map's stencil bake (the next map's pack brings its own) */
	bake_forget();
}

/* ---------- see-through model parts (build 21)

The game sorts its see-through geometry and draws it after the solid world;
the GE path keeps its own list: each part's matrix and texture are fixed
when it is queued (the node matrices it came from may not outlive the
call), and the list is drawn far to near at the end of the frame, without
writing depth. The sky's parts go first, at the far plane. One texture each,
the shader's first map, with the shader's blend function. */

struct transparent_draw
{
	ScePspFMatrix4 matrix;
	void *vertices;
	const unsigned short *indices;
	int index_count, strip;
	unsigned int key;
	float u_scale, v_scale, u_offset, v_offset, distance;
	short blend;
	/* build 116: the shader's fade (psp_limits.h PSP_SEETHROUGH_*): one
	colour for the whole draw (0xFFFFFFFF: none), or, when the fade depends
	on the angle the surface is seen at, a colour per vertex in the record's
	normal bytes (colored) */
	unsigned int color;
	unsigned char colored;
	/* build 116: the shader's tint (psp_ge_seethrough_tint), already in
	color or the per-vertex colours; kept for the near-plane cut */
	unsigned int tint;
	unsigned char sky, sprites;
	/* a widget sprite's (sky 2): whether it is hidden behind the level */
	unsigned char depth_test;
	/* the level's (build 23): 1 indexed into a level stream, 2 cut-up corners */
	unsigned char level;
	/* build 38: a first-person part (its own projection and depth slice) */
	unsigned char first_person;
	/* build 116: a meter's palette (clut, below), in the GE's 8-entry blocks */
	unsigned char clut_blocks;
	/* build 116: drawn with the GE's colour doubling (the tint is half) */
	unsigned char doubled;
	/* how the tint meets the map: SEETHROUGH_MODULATE (multiplied, as
	the GE's TFX_MODULATE), SEETHROUGH_ADD (added, TFX_ADD) or
	SEETHROUGH_FLAT (the tint alone, TFX_BLEND); the alpha is multiplied
	in all three */
	unsigned char tint_model;
	/* the shader's tints face on and edge on (psp_tex_shader_angle_tints),
	blended per vertex by the angle in place of tint; 0 for one tint */
	unsigned char angle_tinted;
	unsigned int tint_face, tint_edge;
	/* SEETHROUGH_EXACT_DEPTH, SEETHROUGH_CLAMPED and SEETHROUGH_ONE_SIDED (below) */
	unsigned short depth_mode;
	/* after b116: the shader (its tag's definition) the part was queued
	with, for tdump; 0 for sprites and parts queued without one */
	unsigned int shader;
	/* build 60: a see-through model part's triangles cut at the near plane
	or the guard band: GE vertices in the part's space, drawn after its list */
	void *cut_vertices;
	int cut_count;
	/* build 60: a level overlay's nudge toward the eye, in depth steps
	(0: worked out from distance when drawn, as builds 54-59) */
	short depth_units;
	/* build 116: a meter shader's draw (b113 #16): the palette worked out
	for its value (meter_palette), drawn in place of the map's own, with
	blend BLEND_METER_TINT or BLEND_METER_BRIGHTNESS and the meter's tint
	colour (ARGB) in tint; NULL for every other draw */
	const unsigned int *clut;
	/* a model part whose shader isn't two-sided: its back faces culled
	(PSP_SEETHROUGH_ONE_SIDED; seethrough_cull) */
	unsigned char one_sided;
	/* a self-occluding object's part: its object's number (model_occluder),
	0 for every other draw; and whether it sorts after every other draw */
	unsigned short occluder;
	unsigned char sorted_last;
};

/* build 116: a meter shader's blends, as the Xbox sets them
(rasterizer_xbox_transparent_geometry.c): in tint mode 2 the frame gets
colour x tint + frame x alpha, as the HUD's meters; otherwise colour x the
meter's brightness + frame x tint, with the alpha test standing in for the
Xbox's alpha kill. Past the blend functions' 0-7 */
#define BLEND_METER_TINT 14
#define BLEND_METER_BRIGHTNESS 15

/* build 116: the see-through fade (defined with transparent_slot) */
/* a see-through shader's tint, for the next queue call's draws: the GE
draws one map of the shader, and the tint stands for the rest of it
(seethrough.py works it out from the shader's stages over that map's
texels). The engine side sets it just before each call (psp_fixups.py,
psp_ge_seethrough_shader); the call's pieces all take it, and it goes back
to white when the call returns. Untinted, b30's ocean drew white */
static unsigned int seethrough_next_tint = 0xFFFFFFFF;
static unsigned int seethrough_next_shader = 0; /* after b116: for tdump */
/* a generic shader that asks to be left out of its object's effect (its flags
byte +41, bit 5: shader_ignores_effect): no part in a self-occluding
object's depth */
static int seethrough_ignores_effect(unsigned int shader)
{
	const unsigned char *definition = (const unsigned char *)shader;

	return shader && *(const short *)(definition + 36) == 5 && (definition[41] & 0x20) != 0;
}
/* build 116: the tint is half the shader's colour, drawn with the GE's
colour doubling (a shader brighter than its map: Cortana, the holograms) */
static unsigned char seethrough_next_doubled = 0;
static unsigned char seethrough_next_angle = 0;
static unsigned int seethrough_next_face, seethrough_next_edge;
static unsigned long seethrough_angle_tinted_draws = 0;
static unsigned long seethrough_doubled_draws = 0;
/* the next queue call's tint model (struct transparent_draw, tint_model):
the bake's fit of the shader over its map's texels (seethrough.py). A
reflection the PSP doesn't draw comes in added (a10's monitor glass) or, when
the map hardly shows in the shader at all, flat (a10's cryo glass) */
#define SEETHROUGH_MODULATE 0
#define SEETHROUGH_ADD 1
#define SEETHROUGH_FLAT 2
static unsigned char seethrough_next_model = SEETHROUGH_MODULATE;
static unsigned long seethrough_added_draws = 0, seethrough_flat_draws = 0, seethrough_rounded_draws = 0;
/* The GE rounds a blend's result down to the frame's 5-6-5 steps, so a faint
flat tint added over the frame loses up to a step a channel: a10's cryo glass,
(9,10,7) a pane, adds (8,8,0), olive where the Xbox adds a grey. A flat tint
is one colour, so half a step added to it first makes the round down land on
the nearest step, without a dither's pattern. "flatnoround": as before */
static int flat_round = -1;
static unsigned int flat_color; /* the draw's rounded tint, 0 when not rounded */
static unsigned int flat_rounded(unsigned int color)
{
	unsigned int r = (color & 0xFF) + 4, g = ((color >> 8) & 0xFF) + 2, b = ((color >> 16) & 0xFF) + 4;

	return (color & 0xFF000000) | ((b > 255 ? 255 : b) << 16) | ((g > 255 ? 255 : g) << 8) | (r > 255 ? 255 : r);
}
/* How the Xbox depth-tests a see-through surface: LESSEQUAL, with a depth
bias only for a shader flagged decal (rasterizer_xbox_transparent_geometry.c;
shaders.c, shader_is_decal). And how it addresses the map: clamped where the
map's flags say (generic and chicago), always for the water shader's base map
(rasterizer_xbox_water.c), repeated otherwise. The port's nudges stand in for the decal bias on
16-bit depth; a surface that isn't a decal gets none, as on the Xbox, so the
far ocean wins its ties with the seabed instead of losing them as the near
plane changes, and the ocean's draws stop taking different pushes (the steps
along the shore). Read from the shader tag on the engine's side (the queue
may run deferred), in the tint call's flags. Keep words "seethroughpush" and
"seethroughwrap": the port's pushes, and every map repeated */
#define SEETHROUGH_EXACT_DEPTH 16
#define SEETHROUGH_CLAMP_U 32
#define SEETHROUGH_CLAMP_V 64
#define SEETHROUGH_CLAMPED (SEETHROUGH_CLAMP_U | SEETHROUGH_CLAMP_V)
/* A water shader flagged for atmospheric fog fades its added reflection with
the fog (rasterizer_xbox_water.c: the final combiner takes the fog factor).
The GE's fog mixes toward its colour, so an added draw gets the fog colour
black: it adds less with distance and nothing at the fog's end. Unfogged, the
far ocean stayed bright against the fog-coloured sky at the horizon (the
white bar between them). Keep word "waterunfogged": as before */
#define SEETHROUGH_FOG_FADE 128
/* the shader has tints face on and edge on (seethrough_tint_set's flags) */
#define SEETHROUGH_ANGLE_TINTED 256
/* A generic or chicago shader without its two-sided flag (bit 2 of the flags
byte at +41) is drawn front faces only on the level, as the Xbox's
environment passes cull (D3DCULL_CCW). Drawn both sides with the overlay's
nudge toward the eye, a back face at a silhouette corner passed the depth
test over the front face beside it: a dashed one-pixel line along a pillar's
edge (b30's interior). Keep word "overlaysnocull": both sides, as before */
#define SEETHROUGH_ONE_SIDED 512
static unsigned short seethrough_next_depth = 0;
static int seethrough_map_clamp = 0; /* the engine side's, for the next psp_ge_seethrough_shader */
static unsigned long seethrough_exact_draws = 0, seethrough_clamped_draws = 0, seethrough_fog_faded_draws = 0;
static unsigned long level_culled_draws = 0;
/* build 116 (b113 #16): a meter shader's parameters, for the next queue
call, set as the tint is (psp_ge_seethrough_meter; seethrough_meter_next 0:
not a meter). Builds 21-115 dropped meter shaders: the plasma weapons' heat
gauges and the Warthog's meters weren't drawn */
static struct psp_seethrough_meter seethrough_next_meter;
static int seethrough_meter_next = 0;
static unsigned long meter_parts = 0, meter_parts_dropped = 0;
static unsigned int *meter_palette(unsigned int key, struct psp_seethrough_meter const *meter, int *count_out);
static int seethrough_fades(int blend);
static unsigned int seethrough_color(int f, int blend);
static unsigned int seethrough_tinted(unsigned int color, unsigned int tint);
static int seethrough_fade_of(const unsigned char *rgba, const struct transparent_draw *draw);
static float seethrough_fade(short packed);
static void seethrough_vertex_colors(struct transparent_draw *draw, int vertex_count, int placed, int mode,
	float fade);
static unsigned int seethrough_lit(unsigned int color, const float *wn, float nn);
/* the see-through fade mode the engine side packs for a glass shader's
diffuse map (psp_transparent_map): lit by the model's light (after b116) */
#define SEETHROUGH_LIT 3

/* a sprite batch's GE vertices: texture coordinate, colour, position */
struct sprite_vertex
{
	float u, v;
	unsigned int color;
	float x, y, z;
};

/* the game's build_sprite_vertex: position (view space), texture coordinate, colour (ARGB) */
struct game_sprite_vertex
{
	float x, y, z, u, v;
	unsigned int argb;
};
/* build 114: 320 (160 before; "seethrough160" for it). b113's Test 1 dropped
494 see-through draws with the table full, Test 3 with the tracers 712
(hardware_b113.md): smoke, sparks and shots not drawn. 128 bytes each, so
the table comes from the heap when the GE starts (40 KB): as a static array
it pushed the program past one of the heap's 64 KB steps (5,888 -> 5,824 KB) */
#define MAXIMUM_TRANSPARENT_DRAWS 320
#define TRANSPARENT_DRAWS_B113 160
static struct transparent_draw *transparent = NULL;
static int transparent_capacity = 0;
static int transparent_count = 0, transparent_order[MAXIMUM_TRANSPARENT_DRAWS];
static unsigned long transparent_drawn = 0, transparent_dropped = 0;

static int transparent_full(void)
{
	return transparent_count >= transparent_capacity;
}

/* at least this much stays free outside the heap once the table is there,
for what may be made later: a thread (the main menu's saved-game check
takes 48 KB while it runs), the blackbox's buffer (32 KB, or 8 when there's
no room) */
#define TRANSPARENT_TABLE_LEAVES_FREE (80 * 1024)

/* once, with the display list (display_list_acquire). Like the list, from
the free user RAM above the heap when it leaves enough there, else from
the heap. "seethroughheap": from the heap, as before */
static void transparent_table_acquire(void)
{
	int wanted = psp_keep("seethrough160") ? TRANSPARENT_DRAWS_B113 : MAXIMUM_TRANSPARENT_DRAWS;
	int bytes = wanted * (int)sizeof(*transparent);
	const char *where = "the heap";
	SceUID block = -1;

	if (transparent)
		return;
	if (!psp_keep("seethroughheap") && (int)sceKernelTotalFreeMemSize() >= bytes + 64 + TRANSPARENT_TABLE_LEAVES_FREE)
		block = sceKernelAllocPartitionMemory(2, "halo see-through table", PSP_SMEM_High, bytes + 64, NULL);
	if (block >= 0)
	{
		transparent = (struct transparent_draw *)(((unsigned int)sceKernelGetBlockHeadAddr(block) + 63) & ~63u);
		where = "free user RAM outside the heap";
	}
	else
		/* the replay (on the ME under geinflight) queues into it: whole
		lines of its own, as psp_tex.c's line_calloc says why */
		transparent = psp_keep("texlinesold") ? memalign(16, bytes) : memalign(64, (bytes + 63) & ~63);
	transparent_capacity = transparent ? wanted : 0;
	platform_log("GE: the see-through table: %d draws a frame, %d KB from %s%s; then %d KB free in user RAM",
		transparent_capacity, bytes / 1024, where, transparent ? "" : " (no memory: none drawn)",
		sceKernelTotalFreeMemSize() / 1024);
}

static void hud_blend(short blend);

/* build 36: a node matrix far off or not a number (the tester saw something stretch
across the screen as the warthog rolled over a ghost): counted, and the
first few logged with the frame */
static unsigned long wild_nodes = 0;

static void halo_matrix(const float *node, ScePspFMatrix4 *model)
{
	float dx = node[10] - camera_eye[0], dy = node[11] - camera_eye[1], dz = node[12] - camera_eye[2];

	if (!(node[0] > 0.0f && node[0] < 100.0f) || !(dx * dx + dy * dy + dz * dz < 4.0e6f))
	{
		if (++wild_nodes <= 8)
			platform_log("GE: frame %lu, a wild node matrix: scale %f at %f %f %f", frames, node[0], node[10], node[11],
				node[12]);
	}
	model->x.x = node[0] * node[1]; model->x.y = node[0] * node[2]; model->x.z = node[0] * node[3]; model->x.w = 0.0f;
	model->y.x = node[0] * node[4]; model->y.y = node[0] * node[5]; model->y.z = node[0] * node[6]; model->y.w = 0.0f;
	model->z.x = node[0] * node[7]; model->z.y = node[0] * node[8]; model->z.z = node[0] * node[9]; model->z.w = 0.0f;
	model->w.x = node[10]; model->w.y = node[11]; model->w.z = node[12]; model->w.w = 1.0f;
	relative(model);
}

static void node_matrix(const void *vertices, const unsigned short *indices, ScePspFMatrix4 *model)
{
	int node_index = ((const unsigned char *)vertices)[part_k14 != 0.0f ? 14 * indices[0] + 7 : 24 * indices[0] + 11] / 3;

	if (node_index >= model_node_count)
		node_index = 0;
	halo_matrix(model_nodes + 13 * node_index, model);
}

/* a batch of sprites (build 22): quads in view space, drawn with the view's
own matrix back to the world; queued with the see-through parts */
static unsigned long sprites_queued = 0;

static int psp_ge_sprites_body(void const *vertices, int sprite_count, unsigned int bitmap_key, short blend, float const *view_to_world)
{
	struct game_sprite_vertex const *in = vertices;
	struct sprite_vertex *out;
	struct transparent_draw *draw;
	int sprite, corner;
	static const int fan[6] = { 0, 1, 2, 0, 2, 3 };
	float cx = 0.0f, cy = 0.0f, cz = 0.0f;

	if (!frame_open || !vertices || sprite_count <= 0 || !bitmap_key || list_full())
		return 0;
	if (transparent_full())
	{
		transparent_dropped++;
		return 0;
	}
	out = list_take(sprite_count * 6 * sizeof(struct sprite_vertex));
	if (!out)
		return 0;
	for (sprite = 0; sprite < sprite_count; sprite++)
	{
		for (corner = 0; corner < 6; corner++)
		{
			struct game_sprite_vertex const *v = &in[4 * sprite + fan[corner]];
			struct sprite_vertex *o = &out[6 * sprite + corner];
			unsigned int argb = v->argb;

			o->u = v->u;
			o->v = v->v;
			o->color = (argb & 0xFF00FF00) | ((argb >> 16) & 0xFF) | ((argb & 0xFF) << 16);
			o->x = v->x; o->y = v->y; o->z = v->z;
		}
		cx += in[4 * sprite].x; cy += in[4 * sprite].y; cz += in[4 * sprite].z;
	}
	draw = &transparent[transparent_count++];
	draw->one_sided = 0;
	draw->occluder = 0;
	draw->sorted_last = 0;
	draw->color = 0xFFFFFFFF; /* build 116: no fade unless the filler sets one */
	draw->colored = 0;
	draw->tint = 0xFFFFFFFF;
	draw->clut = NULL; /* build 116: a meter's own palette, or none */
	draw->doubled = 0;
	draw->tint_model = SEETHROUGH_MODULATE;
	draw->shader = 0;
	halo_matrix(view_to_world, &draw->matrix);
	draw->vertices = out;
	draw->indices = NULL;
	draw->index_count = sprite_count * 6;
	draw->strip = 0;
	draw->key = bitmap_key;
	draw->u_scale = draw->v_scale = 1.0f;
	draw->u_offset = draw->v_offset = 0.0f;
	draw->blend = blend;
	draw->sky = 0;
	draw->sprites = 1;
	draw->level = 0;
	draw->first_person = 0;
	/* view space: distance from the eye */
	cx /= sprite_count; cy /= sprite_count; cz /= sprite_count;
	draw->distance = cx * cx + cy * cy + cz * cz;
	sprites_queued += sprite_count;
	return 1;
}

int psp_ge_sprites(void const *vertices, int sprite_count, unsigned int bitmap_key, short blend, float const *view_to_world)
{
	GE_PRETOUCH(bitmap_key, TXPT_SPRITE);
	GE_DEFER(defer_sprites(vertices, sprite_count, bitmap_key, blend, view_to_world), 1); /* build 112: gedefer */
	GE_BENCH_SKIP(0);
	ge_kind(KIND_SPRITE); /* build 103 */
	unsigned int start = ge_clock();
	int result = psp_ge_sprites_body(vertices, sprite_count, bitmap_key, blend, view_to_world);

	ge_entry_us[4] += ge_clock() - start;
	return result;
}

/* ---------- contrails (b113_candidates.md #1; on by default, "nocontrails" off)

The trail behind a projectile (the plasma rifle's and pistol's bolts, the
needler's needles): render_contrails.c builds a strip of quads along the
trail's points, two corners a point, in world coordinates, and hands it to
the transparent-geometry groups, which the PSP never draws. Here the strip
is queued with the see-through draws as a triangle strip in the sprites'
format (the corners in the game's own order: its two triangles a segment are
a strip's), under a matrix that moves the world so the camera is at its
origin, with the contrail shader's texture and blend. 48 bytes of list a
segment. The game has already faded the corners (the trail's ends, the angle
to the eye) in their alpha */
static unsigned long contrails_queued = 0, contrail_segments = 0;
/* build 114: a contrail's pairs of corners the GE can take, and for each
pair the one it is drawn from (itself, or the nearest one before it) */
#define CONTRAIL_PAIRS 256
static unsigned char contrail_pair_ok[CONTRAIL_PAIRS];
static short contrail_pair_from[CONTRAIL_PAIRS];
static unsigned long contrail_pairs_folded = 0, contrails_out_of_view = 0, contrails_faded = 0;

/* The Xbox's effect shader takes the vertex alpha into the colour wherever
the framebuffer blend has no alpha of its own (rasterizer_xbox_transparent_
geometry.c, the effect's last combiner): added, subtracted and max colours
are scaled by it, multiplied and min colours move toward white by it, double
multiplied toward grey. A contrail's alpha is its fade, by the states over
each point's life and to zero at both ends, so on the GE's added blend,
which never reads alpha, the trail drew at full strength from muzzle to
projectile: a line. Here the corners' colours take their alpha as the Xbox
does. Keep word "contrailsflat": as before (alpha left to the blend) */
static unsigned int contrail_faded_color(unsigned int abgr, int blend)
{
	unsigned int a = abgr >> 24, r = abgr & 0xFF, g = (abgr >> 8) & 0xFF, b = (abgr >> 16) & 0xFF, toward;

	switch (blend)
	{
	case 3: case 4: case 6: case 7: /* add, subtract, max, alpha-multiply-add */
		r = r * a / 255; g = g * a / 255; b = b * a / 255;
		break;
	case 1: case 2: case 5: /* multiply, double multiply, min */
		toward = blend == 2 ? 128 : 255;
		r = (r * a + toward * (255 - a)) / 255;
		g = (g * a + toward * (255 - a)) / 255;
		b = (b * a + toward * (255 - a)) / 255;
		break;
	default: /* alpha blend: the GE reads the alpha itself */
		break;
	}
	return (abgr & 0xFF000000) | (b << 16) | (g << 8) | r;
}

static int psp_ge_contrail_body(void const *vertices, int vertex_count, unsigned int bitmap_key, short packed)
{
	struct game_sprite_vertex const *in = vertices;
	struct sprite_vertex *out;
	struct transparent_draw *draw;
	int segments = vertex_count / 2 - 1, corner;
	float cx = 0.0f, cy = 0.0f, cz = 0.0f;
	/* the blend in the low byte; the shader's map flags above it
	(render_contrails.c's call: bit 1 u clamped, bit 2 v clamped) */
	short blend = (short)(packed & 0xFF);
	int map_flags = (packed >> 8) & 0xFF;
	static int flat = -1;

	if (flat < 0)
		flat = psp_keep("contrailsflat");

	if (!frame_open || !vertices || segments <= 0 || !bitmap_key || list_full())
		return 0;
	if (transparent_full())
	{
		transparent_dropped++;
		return 0;
	}
	vertex_count = 2 * (segments + 1);
	if (vertex_count > 2 * CONTRAIL_PAIRS)
		vertex_count = 2 * CONTRAIL_PAIRS;
	{
		/* build 114: the pairs the GE can't take (b113 Test 3: the tester saw the
		plasma rifle's shots "streak badly and don't always render"). A
		corner behind the near plane, or so far to a side that its screen
		position passes the GE's 2048 pixels, wraps the triangle into a spike
		across the screen or loses it (camera_corner_out; the player's own
		shots trail back past the camera). Such a pair is folded onto the
		nearest pair the GE can take, so the triangles that touch it have no
		area; the trail stops where it leaves the view */
		int pairs = vertex_count / 2, pair, last_good = -1, any_good = 0;
		/* the models' margin (psp_ge_draw_model_body) */
		float limit = projection_near * 1.25f;

		for (pair = 0; pair < pairs; pair++)
		{
			float a[3], b[3];

			to_camera(&in[2 * pair].x, a);
			to_camera(&in[2 * pair + 1].x, b);
			contrail_pair_ok[pair] = !camera_corner_out(a[0], a[1], a[2], limit) &&
				!camera_corner_out(b[0], b[1], b[2], limit);
			any_good |= contrail_pair_ok[pair];
		}
		if (!any_good)
		{
			contrails_out_of_view++;
			return 0;
		}
		for (pair = 0; pair < pairs && !contrail_pair_ok[pair]; pair++)
			;
		last_good = pair;
		for (pair = 0; pair < pairs; pair++)
		{
			if (contrail_pair_ok[pair])
				last_good = pair;
			else
				contrail_pairs_folded++;
			contrail_pair_from[pair] = (short)last_good;
		}
	}
	out = list_take(vertex_count * sizeof(struct sprite_vertex));
	if (!out)
		return 0;
	for (corner = 0; corner < vertex_count; corner++)
	{
		struct game_sprite_vertex const *v = &in[2 * contrail_pair_from[corner / 2] + (corner & 1)];
		struct sprite_vertex *o = &out[corner];
		unsigned int argb = v->argb;

		o->u = v->u;
		o->v = v->v;
		o->color = (argb & 0xFF00FF00) | ((argb >> 16) & 0xFF) | ((argb & 0xFF) << 16);
		if (!flat)
			o->color = contrail_faded_color(o->color, blend);
		o->x = v->x; o->y = v->y; o->z = v->z;
		cx += v->x; cy += v->y; cz += v->z;
	}
	if (!flat)
		contrails_faded++;
	draw = &transparent[transparent_count++];
	/* the slot's last user's depth and wrap flags would stay otherwise; the
	map repeats along the trail unless the shader clamps it, as on the Xbox */
	draw->depth_mode = flat ? 0 :
		(unsigned short)(((map_flags & 2) ? SEETHROUGH_CLAMP_U : 0) | ((map_flags & 4) ? SEETHROUGH_CLAMP_V : 0));
	draw->one_sided = 0;
	draw->occluder = 0;
	draw->sorted_last = 0;
	draw->color = 0xFFFFFFFF; /* build 116: no fade unless the filler sets one */
	draw->colored = 0;
	draw->tint = 0xFFFFFFFF;
	draw->clut = NULL; /* build 116: a meter's own palette, or none */
	draw->doubled = 0;
	draw->tint_model = SEETHROUGH_MODULATE;
	draw->shader = 0;
	memset(&draw->matrix, 0, sizeof(draw->matrix));
	draw->matrix.x.x = draw->matrix.y.y = draw->matrix.z.z = draw->matrix.w.w = 1.0f;
	relative(&draw->matrix);
	draw->vertices = out;
	draw->indices = NULL;
	draw->index_count = vertex_count;
	draw->strip = 1;
	draw->key = bitmap_key;
	draw->u_scale = draw->v_scale = 1.0f;
	draw->u_offset = draw->v_offset = 0.0f;
	draw->blend = blend;
	draw->sky = 0;
	draw->sprites = 1;
	draw->level = 0;
	draw->first_person = 0;
	draw->cut_vertices = NULL;
	draw->cut_count = 0;
	/* from the eye, squared, as the other see-through draws sort */
	cx = cx / vertex_count - camera_eye[0];
	cy = cy / vertex_count - camera_eye[1];
	cz = cz / vertex_count - camera_eye[2];
	draw->distance = cx * cx + cy * cy + cz * cz;
	contrails_queued++;
	contrail_segments += segments;
	return 1;
}

int psp_ge_contrail(void const *vertices, int vertex_count, unsigned int bitmap_key, short blend)
{
	GE_PRETOUCH(bitmap_key, TXPT_CONTRAIL);
	GE_DEFER(defer_contrail(vertices, vertex_count, bitmap_key, blend), 1);
	GE_BENCH_SKIP(0);
	ge_kind(KIND_SPRITE);
	unsigned int start = ge_clock();
	int result = psp_ge_contrail_body(vertices, vertex_count, bitmap_key, blend);

	ge_entry_us[4] += ge_clock() - start;
	return result;
}

/* ---------- the widget layer's sprites (build 31): lens flares and light
volumes. The game hands over a world point, a radius, a scale, a rotation
and a colour (ARGB, alpha the brightness); the Xbox draws a square facing the
screen, the texture times the colour, added with the alpha as its weight. Here
the colour is premultiplied by its alpha and the square added, built in the
world on the camera's right and up so the GE's depth test applies (for those
that ask for it). A far one (the sun's, 1,024 units out) is pulled in along
its line of sight to within the far plane, shrinking with it, so it looks the
same. The Xbox also adds a white-hot core (the texture to the sixth power,
by a tint factor); that is left out */
static unsigned int widget_key = 0;
static int widget_depth = 1;
static unsigned long widget_sprites = 0, widget_begins = 0, widget_untextured = 0, occlusion_tests = 0,
	occlusion_visible = 0;
extern unsigned long psp_occlusion_rays_cast, psp_occlusion_rays_kept; /* rasterizer_xbox_widgets.c (psp_fixups.py) */
extern unsigned long psp_flare_walls_hidden, psp_flare_walls_checked, psp_flare_walls_differed; /* rasterizer_xbox_widgets.c (psp_fixups.py) */

void psp_ge_widget_begin(int depth_test)
{
	GE_DEFER(defer_words(DEFER_WIDGET_BEGIN, 1, (unsigned int)depth_test, 0, 0, 0, 0, 0, 0, 0)); /* build 112: gedefer */
	widget_depth = depth_test;
	widget_begins++;
}

void psp_ge_widget_texture(unsigned int key)
{
	GE_PRETOUCH(key, TXPT_WIDGET);
	GE_DEFER(defer_words(DEFER_WIDGET_TEXTURE, 1, key, 0, 0, 0, 0, 0, 0, 0)); /* build 112: gedefer */
	widget_key = key;
	if (!key)
		widget_untextured++;
}

/* the occlusion rays' tally, for the log */
void psp_ge_widget_occlusion(int visible)
{
	GE_DEFER(defer_words(DEFER_WIDGET_OCCLUSION, 1, (unsigned int)visible, 0, 0, 0, 0, 0, 0, 0)); /* build 112: gedefer */
	occlusion_tests++;
	if (visible)
		occlusion_visible++;
}

void psp_ge_widget_sprite(float const *point, float radius, float scale_x, float scale_y, float rotation,
	unsigned long color)
{
	GE_DEFER(defer_widget_sprite(point, radius, scale_x, scale_y, rotation, color)); /* build 112: gedefer */
	GE_BENCH_SKIP();
	static int sprites_off = -1;

	/* "nowidgetsprites": lens flares and glows left out, for A/Bs */
	if (sprites_off < 0)
		sprites_off = psp_keep("nowidgetsprites");
	if (sprites_off)
		return;
	ge_kind(KIND_WIDGET); /* build 103 */
	static const float corner_uv[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
	static const int fan[6] = { 0, 1, 2, 0, 2, 3 };
	struct sprite_vertex *out;
	struct transparent_draw *draw;
	float centre[3], offset[4][2], camera[3], axis_x, axis_y, alpha, limit, pull = 1.0f;
	unsigned int r, g, b;
	int corner;

	if (!frame_open || !widget_key || radius <= 0.0f || list_full())
		return;
	if (transparent_full())
	{
		transparent_dropped++;
		return;
	}
	to_camera(point, camera);
	if (camera[2] <= NEAR_CLIP)
		return;
	limit = view_far * 0.8f;
	if (camera[2] > limit)
		pull = limit / camera[2];
	centre[0] = camera_eye[0] + (point[0] - camera_eye[0]) * pull;
	centre[1] = camera_eye[1] + (point[1] - camera_eye[1]) * pull;
	centre[2] = camera_eye[2] + (point[2] - camera_eye[2]) * pull;
	radius *= pull;
	/* rasterizer_widget_draw_sprite3d's corners, in radii: its axes are the
	radius turned by the rotation (in degrees there), x right, y down */
	{
		float angle = rotation * (3.14159265f / 180.0f), c = cosf(angle), s = sinf(angle);

		axis_x = radius * (c - s);
		axis_y = radius * (s + c);
	}
	offset[0][0] = -scale_x * axis_x; offset[0][1] = -scale_y * axis_y;
	offset[1][0] = scale_x * axis_y; offset[1][1] = -scale_y * axis_x;
	offset[2][0] = scale_x * axis_x; offset[2][1] = scale_y * axis_y;
	offset[3][0] = -scale_x * axis_y; offset[3][1] = scale_y * axis_x;
	alpha = (float)((color >> 24) & 0xFF) * (1.0f / 255.0f);
	r = (unsigned int)(((color >> 16) & 0xFF) * alpha);
	g = (unsigned int)(((color >> 8) & 0xFF) * alpha);
	b = (unsigned int)((color & 0xFF) * alpha);
	if (!(r | g | b))
		return;
	out = list_take(6 * sizeof(struct sprite_vertex));
	if (!out)
		return;
	for (corner = 0; corner < 6; corner++)
	{
		int k = fan[corner];
		struct sprite_vertex *o = &out[corner];

		o->u = corner_uv[k][0];
		o->v = corner_uv[k][1];
		o->color = 0xFF000000 | (b << 16) | (g << 8) | r;
		/* screen y runs down, the camera's up runs up */
		o->x = centre[0] + camera_right[0] * offset[k][0] - camera_up[0] * offset[k][1];
		o->y = centre[1] + camera_right[1] * offset[k][0] - camera_up[1] * offset[k][1];
		o->z = centre[2] + camera_right[2] * offset[k][0] - camera_up[2] * offset[k][1];
	}
	draw = &transparent[transparent_count++];
	draw->one_sided = 0;
	draw->occluder = 0;
	draw->sorted_last = 0;
	draw->color = 0xFFFFFFFF; /* build 116: no fade unless the filler sets one */
	draw->colored = 0;
	draw->tint = 0xFFFFFFFF;
	draw->clut = NULL; /* build 116: a meter's own palette, or none */
	draw->doubled = 0;
	draw->tint_model = SEETHROUGH_MODULATE;
	draw->shader = 0;
	gumLoadIdentity(&draw->matrix);
	relative(&draw->matrix);
	draw->vertices = out;
	draw->indices = NULL;
	draw->index_count = 6;
	draw->strip = 0;
	draw->key = widget_key;
	draw->u_scale = draw->v_scale = 1.0f;
	draw->u_offset = draw->v_offset = 0.0f;
	draw->blend = 3;
	draw->sky = 2;
	draw->depth_test = (unsigned char)widget_depth;
	draw->sprites = 1;
	draw->level = 0;
	draw->first_person = 0;
	draw->distance = camera[2] * camera[2] * pull * pull;
	widget_sprites++;
}

/* build 118: the sphere first. Most see-through parts are wholly in view,
and for those transparent_cut (below) transforms every corner and tests
every triangle only to leave the draw as it was (1.3-3.5 ms a frame in the
emulator, b117_candidates.md [E]). The model's bounding sphere decides it
first (psp_ge_model_shape: the object's, its root node's bounding offset
and the tag's bounding radius times its scale, the sphere the game culls the
object by). If the sphere lies wholly inside every plane outside() tests,
so does every corner within it, no triangle has a bit set, and the loop
would return with the draw untouched: it is skipped.
That holds only while every corner lies within the sphere, which nothing
guarantees: a tag's radius set too small, a part animated out past it, a
blended corner placed by its first node alone (the rigid record keeps only
that, psp_ge_queue_transparent_body). The opaque parts have trusted the
same sphere since build 56 (model_near: a model whose sphere is clear has
no triangle checked). Parts without a sphere (the sky's, a flag's: radius
0) take the loop as before. Plain floats and no system call: this can run
on the Media Engine.
"tcutcheck": the loop runs anyway after the sphere says inside, and the
log counts the parts where it found a triangle outside (the first few with
how far out their farthest corner lies). "tcutfull": no sphere test, as
builds 60-117 */
static int tcut_full = -1, tcut_check = -1;
static unsigned long tcut_parts = 0, tcut_no_sphere = 0, tcut_inside = 0, tcut_mismatches = 0;
static unsigned long tcut_mismatches_logged = 0;

/* whether the model's sphere lies wholly inside the planes outside()
tests (sphere_inside, at the near plane outside() uses) */
static int model_sphere_inside(void)
{
	return model_sphere_radius > 0.0f && sphere_inside(model_sphere, model_sphere_radius, NEAR_CLIP);
}

/* "tcutcheck": a part its model's sphere called wholly in view has a
triangle outside some plane (bits, outside()'s). Counted; the first few
logged, with the part's corner farthest from the sphere's centre */
static void transparent_sphere_missed(const struct transparent_draw *draw, int vertex_count, int bits)
{
	float farthest = 0.0f;
	long i;

	tcut_mismatches++;
	if (tcut_mismatches_logged >= 8)
		return;
	tcut_mismatches_logged++;
	for (i = 0; i < draw->index_count; i++)
	{
		float c[3], dx, dy, dz;

		if (draw->indices[i] >= vertex_count)
			continue;
		model_to_camera(&draw->matrix, (const float *)((const unsigned char *)draw->vertices + 24 * draw->indices[i] + 12), c);
		dx = c[0] - model_sphere[0];
		dy = c[1] - model_sphere[1];
		dz = c[2] - model_sphere[2];
		if (dx * dx + dy * dy + dz * dz > farthest)
			farthest = dx * dx + dy * dy + dz * dz;
	}
	/* (ge_log: from the ME's work, counted only) */
	ge_log("GE: tcutcheck: a see-through part (key %08x, %d vertices) its model's sphere (radius %.3f, %.2f ahead) called wholly in view has a triangle outside (planes %d); its farthest corner is %.3f from the centre",
		draw->key, vertex_count, model_sphere_radius, model_sphere[2], bits, sqrtf(farthest));
}

/* build 60: a see-through model part checked against the camera as the
level is: triangles wholly inside kept, wholly outside a plane skipped, the
rest cut on the CPU. These parts were given to the GE whole. The sky is
one: a dome some 40 units round the eye, its planet a single quad that at
some headings has a corner behind the eye, and the GE then drew nothing of
it (b59's shots 67-70, the planet gone while in view; reproduced in the
emulator at that spot, 29.9 -114.4, "skydump") */
static void transparent_cut(struct transparent_draw *draw, int vertex_count)
{
	const unsigned short *indices = draw->indices;
	long triangles = draw->strip ? draw->index_count - 2 : draw->index_count / 3, t, count = 0;
	unsigned short *kept = NULL;
	int k, any = 0, cut = 0, sphere_inside = 0;
	struct cut_store pieces_store;

	/* build 118: the model's sphere first (above) */
	if (tcut_full < 0)
		tcut_full = psp_keep("tcutfull");
	if (tcut_check < 0)
		tcut_check = psp_keep("tcutcheck");
	tcut_parts++;
	if (!tcut_full)
	{
		if (model_sphere_radius <= 0.0f)
			tcut_no_sphere++;
		else if (model_sphere_inside())
		{
			tcut_inside++;
			if (!tcut_check)
				return;
			sphere_inside = 1;
		}
	}
	cut_store_begin(&pieces_store, model_cut, MAXIMUM_MODEL_CUT_CORNERS, CUT_SEE);
	corner_verdict_reset();
	for (t = 0; t < triangles; t++)
	{
		unsigned short corner[3];
		int bits[3];

		if (draw->strip)
		{
			corner[0] = indices[t];
			corner[1] = indices[t + ((t & 1) ? 2 : 1)];
			corner[2] = indices[t + ((t & 1) ? 1 : 2)];
			if (corner[0] == corner[1] || corner[1] == corner[2] || corner[0] == corner[2])
				continue;
		}
		else
		{
			corner[0] = indices[3 * t];
			corner[1] = indices[3 * t + 1];
			corner[2] = indices[3 * t + 2];
		}
		if (corner[0] >= vertex_count || corner[1] >= vertex_count || corner[2] >= vertex_count)
			continue;
		for (k = 0; k < 3; k++)
		{
			int known = corner[k] < CORNER_VERDICTS ? corner_verdict[corner[k]] : -1;

			if (known >= corner_stamp && known < corner_stamp + CORNER_STAMP_STEP)
				bits[k] = known - corner_stamp;
			else
			{
				float c[3];

				model_to_camera(&draw->matrix, (const float *)((const unsigned char *)draw->vertices + 24 * corner[k] + 12), c);
				bits[k] = outside(c);
				if (corner[k] < CORNER_VERDICTS)
					corner_verdict[corner[k]] = (unsigned short)(corner_stamp + bits[k]);
			}
		}
		if (!any && (bits[0] | bits[1] | bits[2]))
		{
			/* the first triangle not wholly inside: from here a list of our own */
			long u;

			any = 1;
			if (sphere_inside)
				transparent_sphere_missed(draw, vertex_count, bits[0] | bits[1] | bits[2]);
			kept = list_take((int)((triangles * 3) * sizeof(unsigned short) + 3) & ~3);
			if (!kept)
				return;
			/* the triangles before this one were all inside: copied as a list */
			count = 0;
			for (u = 0; u < t; u++)
			{
				unsigned short a, b, c;

				if (draw->strip)
				{
					a = indices[u];
					b = indices[u + ((u & 1) ? 2 : 1)];
					c = indices[u + ((u & 1) ? 1 : 2)];
					if (a == b || b == c || a == c)
						continue;
				}
				else
				{
					a = indices[3 * u]; b = indices[3 * u + 1]; c = indices[3 * u + 2];
				}
				if (a >= vertex_count || b >= vertex_count || c >= vertex_count)
					continue;
				kept[3 * count] = a; kept[3 * count + 1] = b; kept[3 * count + 2] = c;
				count++;
			}
		}
		if (!(bits[0] | bits[1] | bits[2]))
		{
			if (any)
			{
				kept[3 * count] = corner[0]; kept[3 * count + 1] = corner[1]; kept[3 * count + 2] = corner[2];
			}
			count++;
			continue;
		}
		if (bits[0] & bits[1] & bits[2])
			continue;
		{
			float polygon[3][CORNER_FLOATS];

			for (k = 0; k < 3; k++)
			{
				const unsigned char *record = (const unsigned char *)draw->vertices + 24 * corner[k];
				const float *uv = (const float *)record, *p = (const float *)(record + 12);

				model_to_camera(&draw->matrix, p, polygon[k]);
				polygon[k][3] = uv[0];
				polygon[k][4] = uv[1];
				/* build 116: a coloured draw's fade in the normal's place:
				the pieces' colour is rebuilt from it */
				polygon[k][5] = draw->colored ? (float)seethrough_fade_of(record + 8, draw) : (float)(signed char)record[8];
				polygon[k][6] = (signed char)record[9];
				polygon[k][10] = (signed char)record[10];
				polygon[k][7] = p[0];
				polygon[k][8] = p[1];
				polygon[k][9] = p[2];
			}
			cut += clip_triangle(polygon, bits[0] | bits[1] | bits[2], &pieces_store);
		}
	}
	if (!any)
		return;
	draw->indices = kept;
	draw->index_count = (int)count * 3;
	draw->strip = 0;
	if (cut)
	{
		unsigned char *out = list_take(cut * 24);
		int c;

		if (!out)
			return;
		for (c = 0; c < cut; c++)
		{
			unsigned char *record = out + 24 * c;
			const float *corner = cut_at(&pieces_store, c);
			float uv[2] = { corner[3], corner[4] };

			memcpy(record, uv, 8);
			if (draw->colored)
				*(unsigned int *)(record + 8) = seethrough_tinted(seethrough_color((int)(corner[5] + 0.5f),
					draw->blend), draw->tint);
			else
			{
				record[8] = (unsigned char)(signed char)corner[5];
				record[9] = (unsigned char)(signed char)corner[6];
				record[10] = (unsigned char)(signed char)corner[10];
				record[11] = 0;
			}
			memcpy(record + 12, &corner[7], 12);
		}
		draw->cut_vertices = out;
		draw->cut_count = cut;
	}
}

/* build 118: whether a see-through part's triangles use more than one node
(the scan below, build 42's), remembered per part. The scan read every
index's vertex every frame for every see-through part, and its answer only
changes with the part's data: c40's see-through queue was 18% of its
drawing's instructions (nearwins118). Kept under the vertex and index
buffers, their counts, and the geometry cache's generation for each buffer
(an address the cache loads with something else gets a new one). "multiscan"
(keep.txt): scanned every frame, as before; "multicheck": a remembered
answer is scanned again and compared */
#define MULTI_MEMO 64
static struct
{
	const void *vertices;
	const unsigned short *indices;
	long index_count;
	int vertex_count;
	unsigned int vertices_generation, indices_generation;
	int multi;
} multi_memo[MULTI_MEMO];
static int multi_scan = -1, multi_check = 0;
static unsigned long multi_scans = 0, multi_remembered = 0, multi_mismatches = 0;

static int transparent_part_scan(const unsigned char *base, const unsigned short *indices, long index_count, int vertex_count)
{
	int first = indices[0] < vertex_count ? base[24 * indices[0] + 11] : 0;
	long i;

	for (i = 0; i < index_count; i++)
		if (indices[i] < vertex_count && base[24 * indices[i] + 11] != first)
			return 1;
	return 0;
}

/* build 119: forgotten with the map (psp_ge_forget_map) */
static void transparent_multi_forget(void)
{
	memset(multi_memo, 0, sizeof(multi_memo));
}

static int transparent_part_multi(const unsigned char *base, const unsigned short *indices, long index_count, int vertex_count)
{
	unsigned int slot = (((unsigned int)base >> 4) ^ ((unsigned int)indices >> 2) ^ (unsigned int)index_count) & (MULTI_MEMO - 1);
	unsigned int vertices_generation, indices_generation;
	int multi;

	if (multi_scan < 0)
	{
		multi_scan = psp_keep("multiscan");
		multi_check = psp_keep("multicheck");
	}
	if (multi_scan)
		return transparent_part_scan(base, indices, index_count, vertex_count);
	vertices_generation = psp_geo_generation(base);
	indices_generation = psp_geo_generation(indices);
	if (multi_memo[slot].vertices == base && multi_memo[slot].indices == indices &&
		multi_memo[slot].index_count == index_count && multi_memo[slot].vertex_count == vertex_count &&
		multi_memo[slot].vertices_generation == vertices_generation && multi_memo[slot].indices_generation == indices_generation)
	{
		multi_remembered++;
		if (multi_check && transparent_part_scan(base, indices, index_count, vertex_count) != multi_memo[slot].multi)
			multi_mismatches++;
		return multi_memo[slot].multi;
	}
	multi_scans++;
	multi = transparent_part_scan(base, indices, index_count, vertex_count);
	multi_memo[slot].vertices = base;
	multi_memo[slot].indices = indices;
	multi_memo[slot].index_count = index_count;
	multi_memo[slot].vertex_count = vertex_count;
	multi_memo[slot].vertices_generation = vertices_generation;
	multi_memo[slot].indices_generation = indices_generation;
	multi_memo[slot].multi = multi;
	return multi;
}

/* A see-through part on several nodes is placed vertex by vertex on the CPU,
and on c40 (Cortana, Guilty Spark) that is most of the see-through queue's
work. "seethroughvfpu" (keep.txt, off): each node's matrix is loaded into the
VFPU once per run of vertices on it, and a vertex is one vhtfm4. The VFPU
sums the products in its own order, so a vertex can land a last bit away
from the FPU's: a placement change too small to see, but not bit-exact, so
it is the maintainer's call after an A/B. Never in the Media Engine's work (no VFPU) */
static int seethrough_vfpu = -1;
static unsigned long seethrough_vfpu_vertices = 0;

/* rows m->x .. m->w into R100..R103, so column a of M100 is the matrix's
row a with its offset: vhtfm4 with (p, 1) then gives the placed point */
#define SEETHROUGH_VFPU_LOAD(m) __asm__ volatile( \
	"lv.q R100, 0(%0)\n" "lv.q R101, 16(%0)\n" "lv.q R102, 32(%0)\n" "lv.q R103, 48(%0)\n" \
	: : "r"(m) : "memory")

static void seethrough_vfpu_place(const float *p, float *q)
{
	__asm__ volatile("lv.s S000, 0(%0)\n" "lv.s S001, 4(%0)\n" "lv.s S002, 8(%0)\n"
		"vhtfm4.q C010, M100, C000\n"
		"sv.s S010, 0(%1)\n" "sv.s S011, 4(%1)\n" "sv.s S012, 8(%1)\n"
		: : "r"(p), "r"(q) : "memory");
}

/* A see-through model part is drawn one-sided unless its shader is
two-sided, as the Xbox draws it (rasterizer_xbox_transparent_geometry.c:
D3DCULL_CCW unless the shader's two-sided flag; the flag rides in the queue's
blend word, PSP_SEETHROUGH_ONE_SIDED). Both sides drawn, an added hologram
(Cortana) took her back's light on top of her front's: brighter, and her
far side's markings over her face, inside out. "seethroughtwosided": every
see-through part drawn from both sides, as before */
static int seethrough_cull = -1;
static unsigned long seethrough_culled_draws = 0;

static int seethrough_cull_on(void)
{
	if (seethrough_cull < 0)
		seethrough_cull = !psp_keep("seethroughtwosided");
	return seethrough_cull;
}

static void psp_ge_queue_transparent_body(void *vertices, int vertex_count, int uncompressed, const unsigned short *indices,
	long index_count, int strip, unsigned int key, float u_scale, float v_scale, float u_offset, float v_offset,
	short blend, const float *centroid)
{
	struct transparent_draw *draw;

	unsigned int *clut = NULL;
	int clut_count = 0;

	if (!frame_open || index_count < 3 || !vertices || !indices || !model_nodes || model_node_count <= 0 ||
		vertex_count <= 0 || !model_convert(vertices, vertex_count, uncompressed, NULL, 0, 0, NULL))
		return;
	part_k14 = 0.0f;
	if (transparent_full())
	{
		transparent_dropped++;
		return;
	}
	if (seethrough_meter_next)
	{
		/* build 116: a meter's palette for its value now (the cache slot is
		this frame's until the list has run), from its map's, brought into
		memory first (the flush binds it later); none (a map without a 32-bit
		palette, not loaded yet, the list full): the part is left out */
		meter_parts++;
		clut = psp_tex_resident(key) ? meter_palette(key, &seethrough_next_meter, &clut_count) : NULL;
		if (!clut || clut_count < 8)
		{
			meter_parts_dropped++;
			return;
		}
	}
	draw = &transparent[transparent_count++];
	draw->one_sided = 0;
	draw->occluder = 0;
	draw->sorted_last = 0;
	draw->color = 0xFFFFFFFF; /* build 116: no fade unless the filler sets one */
	draw->colored = 0;
	draw->tint = 0xFFFFFFFF;
	draw->clut = NULL; /* build 116: a meter's own palette, or none */
	draw->doubled = 0;
	draw->tint_model = SEETHROUGH_MODULATE;
	draw->shader = 0;
	draw->cut_vertices = NULL;
	draw->cut_count = 0;
	node_matrix(vertices, indices, &draw->matrix);
	draw->first_person = (unsigned char)model_first_person;
	draw->vertices = vertices;
	{
		/* build 42: a see-through part hanging from several nodes (the
		warthog's lights on its hull and gun mount, the chain gun's belt on
		the mount, chain and gun) was drawn on its first vertex's node, so
		the rest stretched toward it as the turret turned. Its vertices are
		placed on the CPU each frame instead, each by its own node (blended
		ones by the first: the rigid record keeps only that), and drawn with
		no further matrix */
		const unsigned char *base = vertices;
		int multi = transparent_part_multi(base, indices, index_count, vertex_count); /* build 118: remembered */

		if (multi && vertex_count <= 4096)
		{
			unsigned char *out = list_take(vertex_count * 24);

			if (out)
			{
				static ScePspFMatrix4 node_at[SKIN_NODES] __attribute__((aligned(16)));
				static unsigned char node_done[SKIN_NODES];
				int v, on_vfpu, loaded = -1;

				if (seethrough_vfpu < 0)
					seethrough_vfpu = psp_vfpu_on && psp_keep("seethroughvfpu");
				on_vfpu = seethrough_vfpu && !PSP_ME_WORK();
				memset(node_done, 0, sizeof(node_done));
				for (v = 0; v < vertex_count; v++)
				{
					const unsigned char *in = base + 24 * v;
					const float *p = (const float *)(in + 12);
					float *q = (float *)(out + 24 * v + 12);
					int node = in[11] / 3;
					ScePspFMatrix4 *m;

					if (node >= SKIN_NODES || node >= model_node_count)
						node = 0;
					m = &node_at[node];
					if (!node_done[node])
					{
						halo_matrix(model_nodes + 13 * node, m);
						node_done[node] = 1;
					}
					memcpy(out + 24 * v, in, 12);
					if (on_vfpu)
					{
						if (node != loaded)
						{
							SEETHROUGH_VFPU_LOAD(m);
							loaded = node;
						}
						seethrough_vfpu_place(p, q);
						continue;
					}
					q[0] = m->x.x * p[0] + m->y.x * p[1] + m->z.x * p[2] + m->w.x;
					q[1] = m->x.y * p[0] + m->y.y * p[1] + m->z.y * p[2] + m->w.y;
					q[2] = m->x.z * p[0] + m->y.z * p[1] + m->z.z * p[2] + m->w.z;
				}
				if (on_vfpu)
					seethrough_vfpu_vertices += vertex_count;
				draw->vertices = out;
				memset(&draw->matrix, 0, sizeof(draw->matrix));
				draw->matrix.x.x = draw->matrix.y.y = draw->matrix.z.z = draw->matrix.w.w = 1.0f;
				transparent_placed++;
			}
		}
	}
	draw->indices = indices;
	draw->index_count = (int)index_count;
	draw->strip = strip;
	draw->key = key;
	draw->u_scale = model_u_scale * (u_scale != 0.0f ? u_scale : 1.0f);
	draw->v_scale = model_v_scale * (v_scale != 0.0f ? v_scale : 1.0f);
	draw->u_offset = u_offset;
	draw->v_offset = v_offset;
	draw->blend = PSP_SEETHROUGH_BLEND(blend);
	draw->one_sided = PSP_SEETHROUGH_ONE_SIDED(blend) && seethrough_cull_on();
	draw->occluder = in_sky || seethrough_ignores_effect(seethrough_next_shader) ? 0 : model_occluder;
	draw->sorted_last = model_sorted_last;
	draw->tint = seethrough_next_tint;
	draw->shader = seethrough_next_shader;
	draw->doubled = seethrough_next_doubled;
	draw->tint_model = seethrough_next_model;
	draw->depth_mode = seethrough_next_depth;
	draw->angle_tinted = seethrough_next_angle && seethrough_next_model == SEETHROUGH_MODULATE;
	draw->tint_face = seethrough_next_face;
	draw->tint_edge = seethrough_next_edge;
	draw->color = draw->tint;
	draw->colored = 0;
	if (clut)
	{
		/* build 116: a meter (the palette does its colours; no fade) */
		draw->clut = clut;
		draw->clut_blocks = (unsigned char)(clut_count / 8);
		draw->blend = seethrough_next_meter.tint_mode_2 ? BLEND_METER_TINT : BLEND_METER_BRIGHTNESS;
		draw->tint = seethrough_next_meter.tint_color;
		draw->color = 0xFFFFFFFF;
		draw->doubled = 0;
		draw->tint_model = SEETHROUGH_MODULATE;
	}
	draw->sky = (unsigned char)in_sky;
	draw->sprites = 0;
	draw->level = 0;
	if (centroid)
	{
		float dx = centroid[0] - camera_eye[0], dy = centroid[1] - camera_eye[1], dz = centroid[2] - camera_eye[2];

		draw->distance = dx * dx + dy * dy + dz * dz;
	}
	else
		draw->distance = 0.0f;
	if (draw->sky)
		draw->distance = 1e30f;
	/* build 116: the shader's fade (the source's value, active camouflage,
	the angle), before the cut so the cut pieces carry it */
	if (seethrough_fades(draw->blend))
	{
		float fade = seethrough_fade(blend);
		int mode = PSP_SEETHROUGH_MODE(blend);

		if ((mode != 0 || draw->angle_tinted) && vertex_count <= 4096)
			seethrough_vertex_colors(draw, vertex_count, draw->vertices != vertices, mode, fade);
		else if (fade < 1.0f)
			draw->color = seethrough_tinted(seethrough_color((int)(fade * 255.0f + 0.5f), draw->blend), draw->tint);
	}
	if (model_drop < 0)
		model_drop = psp_keep("modeldrop");
	if (!draw->first_person && !model_drop)
		transparent_cut(draw, vertex_count);
	/* build 60: "skydump": where a sky part's corners lie from the eye */
	if (draw->sky)
	{
		static int dump = -1;
		static unsigned long parts = 0;

		if (dump < 0)
			dump = psp_keep("skydump");
		if (dump && (parts++ % 400) < 8)
		{
			float zmin = 1e30f, zmax = -1e30f, side = 0.0f;
			int behind = 0, beyond = 0, wide = 0, n = 0;
			long i;

			for (i = 0; i < index_count; i++)
			{
				const float *q = (const float *)((const unsigned char *)draw->vertices + 24 * indices[i] + 12);
				float c[3];

				if (indices[i] >= vertex_count)
					continue;
				model_to_camera(&draw->matrix, q, c);
				n++;
				if (c[2] < zmin) zmin = c[2];
				if (c[2] > zmax) zmax = c[2];
				if (c[2] < projection_near) behind++;
				if (c[2] > projection_far) beyond++;
				if (c[2] > 0.0f && (fabsf(c[0]) > guard_x * c[2] || fabsf(c[1]) > guard_y * c[2])) wide++;
				if (c[2] > 0.0f && fabsf(c[0]) / c[2] > side) side = fabsf(c[0]) / c[2];
			}
			platform_log("skydump: part key %08x, %ld indices%s, corners %d: z %.1f to %.1f (far %.1f); behind the near plane %d, past the far plane %d, past the guard band %d",
				key, index_count, strip ? " (strip)" : "", n, zmin, zmax, projection_far, behind, beyond, wide);
		}
	}
}

void psp_ge_queue_transparent(void *vertices, int vertex_count, int uncompressed, const unsigned short *indices,
	long index_count, int strip, unsigned int key, float u_scale, float v_scale, float u_offset, float v_offset,
	short blend, const float *centroid)
{
	GE_BENCH_SKIP();
	GE_PRETOUCH(key, TXPT_SEETHROUGH);
	GE_DEFER(defer_queue_transparent(vertices, vertex_count, uncompressed, indices, index_count, strip, key,
		u_scale, v_scale, u_offset, v_offset, blend, centroid));
	ge_kind(KIND_SEETHROUGH_QUEUE); /* build 103 */
	unsigned int start = ge_clock();

	psp_ge_queue_transparent_body(vertices, vertex_count, uncompressed, indices, index_count, strip, key, u_scale, v_scale, u_offset, v_offset, blend, centroid);
	seethrough_next_tint = 0xFFFFFFFF; /* build 116: this call's tint is used */
	seethrough_next_shader = 0;
	seethrough_next_doubled = 0;
	seethrough_next_angle = 0;
	seethrough_next_model = SEETHROUGH_MODULATE;
	seethrough_next_depth = 0;
	seethrough_meter_next = 0; /* and its meter */
	ge_entry_us[2] += ge_clock() - start;
}

/* the level's see-through surfaces (build 23): as the solid level, each
triangle checked against the camera (kept, skipped, or cut up, the pieces
turned back into the world's coordinates), then queued with an identity
matrix: the kept ones indexed into the material's stream, the pieces as
corners */
static unsigned long level_transparent_draws = 0;
static PSP_MAIN_SIDE unsigned long ge_wait_us = 0, ge_wait_worst = 0;
static PSP_MAIN_SIDE unsigned long frame_end_us[3];

/* build 116: a see-through shader's fade as a GE colour that the texture is
multiplied by (TFX_MODULATE): for an alpha blend the alpha, for the added and
subtracted blends the colour and alpha alike. The multiplied and min/max
blends would need a fade toward their neutral colour, which a modulate can't
do: they stay at full strength (seethrough_fades) */
static int seethrough_fades(int blend)
{
	return blend == 0 || blend == 3 || blend == 4 || blend == 7;
}

static unsigned int seethrough_color(int f, int blend)
{
	f = f < 0 ? 0 : f > 255 ? 255 : f;
	if (blend == 0)
		return ((unsigned int)f << 24) | 0x00FFFFFF;
	return ((unsigned int)f << 24) | ((unsigned int)f << 16) | ((unsigned int)f << 8) | (unsigned int)f;
}

/* the next queue call's tint, its model as the pack gives it
(psp_tex_shader_tint: 0 modulate, 1 modulate doubled, 2 added, 3 flat) and
its shader (the tag's definition, for tdump; 0 for none) */
static void seethrough_tint_set(unsigned int tint, int flags, unsigned int shader, unsigned int face,
	unsigned int edge)
{
	GE_DEFER(defer_words(DEFER_SEETHROUGH_TINT, 5, tint, (unsigned int)flags, shader, face, edge, 0, 0, 0)); /* build 112: gedefer */
	seethrough_next_tint = tint;
	seethrough_next_angle = (flags & SEETHROUGH_ANGLE_TINTED) != 0;
	seethrough_next_face = face;
	seethrough_next_edge = edge;
	seethrough_next_depth = (unsigned short)(flags & (SEETHROUGH_EXACT_DEPTH | SEETHROUGH_CLAMPED | SEETHROUGH_FOG_FADE |
		SEETHROUGH_ONE_SIDED));
	flags &= 15;
	seethrough_next_doubled = (unsigned char)(flags == 1);
	seethrough_next_model = flags == 2 ? SEETHROUGH_ADD : flags == 3 ? SEETHROUGH_FLAT : SEETHROUGH_MODULATE;
	seethrough_next_shader = shader;
}

void psp_ge_seethrough_tint(unsigned int tint)
{
	seethrough_tint_set(tint, 0, 0, 0, 0);
}

/* the engine side's call before each see-through queue call: the shader's
tint from the texture pack (psp_tex.c; white for a shader it has none for) */
/* the map psp_transparent_map chose for a generic or chicago shader: whether
it is clamped in u and in v (the engine side, just before the shader call) */
void psp_ge_seethrough_map_clamp(int u, int v)
{
	seethrough_map_clamp = (u ? SEETHROUGH_CLAMP_U : 0) | (v ? SEETHROUGH_CLAMP_V : 0);
}

void psp_ge_seethrough_shader(unsigned int shader)
{
	static int pushed = -1, wrapped = -1, unfogged = -1, both_sides = -1;
	int flags = 0;
	unsigned int tint = psp_tex_shader_tint(shader, &flags), face = 0, edge = 0;

	if (pushed < 0)
	{
		pushed = psp_keep("seethroughpush");
		wrapped = psp_keep("seethroughwrap");
		unfogged = psp_keep("waterunfogged");
		both_sides = psp_keep("overlaysnocull");
	}
	if (shader)
	{
		/* the shader's type at +36 (Invader's numbering: generic 5, chicago
		6, water 7, glass 8, meter 9, plasma 10); generic's and chicago's
		flags byte at +41, glass's and meter's word at +40 */
		const unsigned char *definition = (const unsigned char *)shader;
		short type = *(const short *)(definition + 36);
		int decal = type == 5 || type == 6 ? (definition[41] & 2) != 0 :
			type == 8 ? (*(const unsigned short *)(definition + 40) & 2) != 0 :
			type == 9 ? (*(const unsigned short *)(definition + 40) & 1) != 0 : 0;

		if (!pushed && !decal && type >= 5 && type <= 10)
			flags |= SEETHROUGH_EXACT_DEPTH;
		if (!both_sides && (type == 5 || type == 6) && !(definition[41] & 4))
			flags |= SEETHROUGH_ONE_SIDED;
		if (!wrapped)
			flags |= type == 7 ? SEETHROUGH_CLAMPED : type == 5 || type == 6 ? seethrough_map_clamp : 0;
		/* water's flags word at +40: bit 2 atmospheric fog */
		if (!unfogged && type == 7 && (*(const unsigned short *)(definition + 40) & 4))
			flags |= SEETHROUGH_FOG_FADE;
	}
	seethrough_map_clamp = 0;
	if (shader && psp_tex_shader_angle_tints(shader, &face, &edge))
		flags |= SEETHROUGH_ANGLE_TINTED;
	/* glass with no diffuse map draws its reflection (psp_fixups.py,
	psp_transparent_map): the perpendicular colour face on, the parallel one
	edge on, each times its own alpha (argb floats at +140 and +156) */
	if (shader && *(const short *)((const unsigned char *)shader + 36) == 8 &&
		*(const long *)((const unsigned char *)shader + 356) == -1)
	{
		const float *perpendicular = (const float *)((const unsigned char *)shader + 140);
		const float *parallel = (const float *)((const unsigned char *)shader + 156);
		float a[3] = { perpendicular[1] * perpendicular[0], perpendicular[2] * perpendicular[0],
			perpendicular[3] * perpendicular[0] };
		float b[3] = { parallel[1] * parallel[0], parallel[2] * parallel[0], parallel[3] * parallel[0] };
		float middle[3] = { 0.5f * (a[0] + b[0]), 0.5f * (a[1] + b[1]), 0.5f * (a[2] + b[2]) };

		face = abgr_from_rgb(a);
		edge = abgr_from_rgb(b);
		/* the level's see-through draws take one tint: the two's middle */
		tint = abgr_from_rgb(middle);
		flags = (flags & ~15) | SEETHROUGH_ANGLE_TINTED;
	}

	seethrough_tint_set(tint, flags, shader, face, edge);
}

/* Glass multiplies what lies behind it by its tint colour before its
reflection is added (rasterizer_xbox_transparent_geometry.c, the glass tint
pass: the frame times the tint map times the tint colour). Glass drawn by its
reflection alone is one added pass here, so the tint rides in that pass as
the GE's destination factor: the frame times the tint, plus the reflection,
which is what the Xbox's two passes give. Without it the glass was clear: the
Pelican's canopy showed its lit cockpit where the original's is smoked
violet, with the reflection on top. A tint map can't ride a fixed factor:
such glass leaves the frame as it is (the Pelican's and the lifepod's
glass have none). The tint colour at +84, the tint map's index at +112.
"glassnotint": the frame left as it is, as before */
static int glass_tint_off = -1;
static unsigned long glass_tinted_draws = 0;

static void __attribute__((noinline)) glass_tint_behind(unsigned int shader)
{
	const unsigned char *definition = (const unsigned char *)shader;
	const float *tint = (const float *)(definition + 84);
	unsigned int fix = 0;
	int k;

	if (glass_tint_off < 0)
	{
		glass_tint_off = psp_keep("glassnotint");
		if (glass_tint_off)
			platform_log("GE: glassnotint: glass leaves the frame behind it untinted");
	}
	if (glass_tint_off || *(const short *)(definition + 36) != 8 || *(const long *)(definition + 356) != -1 ||
		*(const long *)(definition + 112) != -1 || tint[0] + tint[1] + tint[2] <= 0.0f)
		return;
	for (k = 0; k < 3; k++)
		fix |= (unsigned int)PSP_CLAMP_255((int)(tint[k] * 255.0f + 0.5f)) << (8 * k);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, fix);
	glass_tinted_draws++;
}

/* build 116: the next queue call's meter (struct psp_seethrough_meter), or
NULL for a part that isn't one. The engine side calls it before every model
part it queues (psp_fixups.py), so a meter never carries over to the next */
void psp_ge_seethrough_meter(const void *meter)
{
	const unsigned int *w = meter;

	GE_DEFER(w ? defer_words(DEFER_SEETHROUGH_METER, 1 + METER_WORDS, 1, w[0], w[1], w[2], w[3], w[4], w[5], w[6]) :
		defer_words(DEFER_SEETHROUGH_METER, 1, 0, 0, 0, 0, 0, 0, 0, 0)); /* build 112: gedefer */
	seethrough_meter_next = meter != NULL;
	if (meter)
		memcpy(&seethrough_next_meter, meter, sizeof(seethrough_next_meter));
}

/* two colours multiplied channel by channel (ABGR, 0-255 each) */
/* a shader's tint at a vertex seen at c (|cos| of the angle between its
normal and the view: 1 face on), between its tints edge on and face on */
static unsigned int seethrough_angle_tint(const struct transparent_draw *draw, float c)
{
	unsigned int out = 0;
	int shift, w = (int)(c * 256.0f);

	w = w < 0 ? 0 : w > 256 ? 256 : w;
	for (shift = 0; shift < 32; shift += 8)
	{
		int edge = (draw->tint_edge >> shift) & 255, face = (draw->tint_face >> shift) & 255;

		out |= (unsigned int)((edge * (256 - w) + face * w + 128) >> 8) << shift;
	}
	return out;
}

static unsigned int seethrough_tinted(unsigned int color, unsigned int tint)
{
	unsigned int out = 0;
	int shift;

	if (tint == 0xFFFFFFFF)
		return color;
	for (shift = 0; shift < 32; shift += 8)
		out |= ((((color >> shift) & 255) * ((tint >> shift) & 255) + 127) / 255) << shift;
	return out;
}

/* the fade (0-255) a tinted per-vertex colour was made from, for the
near-plane cut: through the alpha for an alpha blend, otherwise through the
tint's strongest channel (an added blend's colour is the fade in every
channel) */
static int seethrough_fade_of(const unsigned char *rgba, const struct transparent_draw *draw)
{
	int channel = 3, k, t;

	if (draw->blend != 0)
		for (k = 0; k < 3; k++)
			if (((draw->tint >> (8 * k)) & 255) > ((draw->tint >> (8 * channel)) & 255))
				channel = k;
	t = (int)((draw->tint >> (8 * channel)) & 255);
	if (t <= 0)
		return 0;
	t = (rgba[channel] * 255 + t / 2) / t;
	return t > 255 ? 255 : t;
}

/* the fade a packed blend asks for, times active camouflage's (the Xbox's
1 - intensity, unless the shader says to keep it), 0-1 (model_camouflage:
psp_ge_model_effect) */
static float seethrough_fade(short packed)
{
	float fade = PSP_SEETHROUGH_FADE(packed);

	if (!PSP_SEETHROUGH_KEEP_WITH_CAMOUFLAGE(packed))
		fade *= 1.0f - model_camouflage;
	return fade;
}

/* build 116: the fade by the angle a see-through part is seen at, a colour
per vertex in a copy of its records (the 8-bit normal's 4 bytes become the
colour; the flush draws it with GU_COLOR_8888). As the Xbox's vertex program
for transparent shaders works it out (rasterizer_xbox_vertex_shaders_data.inc,
program 24, translated by the port's nv2a_vsh.c): c = |n . the camera's
forward direction| (1 seen face on, 0 seen edge on); its second colour
carries c in blue and 1 - c in alpha, and "fade when perpendicular" (mode 1)
takes the alpha, "fade when parallel" (mode 2) the blue. So a surface that
fades when perpendicular vanishes seen face on (a light volume looked
straight into), and one that fades when parallel vanishes edge on. The first
version had the two the other way round (c40's pale discs stayed). Placed
records (several nodes, already in the camera's space) keep their node in
byte 11; the others go through the draw's matrix */
static unsigned long seethrough_angle_draws = 0, seethrough_lit_draws = 0;
/* after build 116: a glass shader's diffuse map lit as the Xbox lights it
(its vertex program for glass's diffuse pass): the model's ambient colour
plus each of its distant lights by the normal's angle to it, times the
colour. Glass drew at full brightness in dark rooms (a10's bridge monitor:
a bright green pattern); the shader's reflection (a cube map) and tint
passes aren't drawn yet. Colours are the GE's ABGR; nn is the world
normal's squared length */
static unsigned int seethrough_lit(unsigned int color, const float *wn, float nn)
{
	float light[3], inverse = nn > 0.0f ? 1.0f / sqrtf(nn) : 0.0f;
	unsigned int out = color & 0xFF000000u;
	int index, channel;

	for (channel = 0; channel < 3; channel++)
		light[channel] = (float)((model_ambient >> (8 * channel)) & 0xFF);
	for (index = 0; index < model_light_count; index++)
	{
		float d = (wn[0] * model_light_toward[index].x + wn[1] * model_light_toward[index].y +
			wn[2] * model_light_toward[index].z) * inverse;

		if (d > 0.0f)
			for (channel = 0; channel < 3; channel++)
				light[channel] += d * (float)((model_light_color[index] >> (8 * channel)) & 0xFF);
	}
	for (channel = 0; channel < 3; channel++)
	{
		float value = (float)((color >> (8 * channel)) & 0xFF) * (light[channel] > 255.0f ? 255.0f : light[channel]) /
			255.0f;

		out |= (unsigned int)(value + 0.5f) << (8 * channel);
	}
	return out;
}

static void seethrough_vertex_colors(struct transparent_draw *draw, int vertex_count, int placed, int mode,
	float fade)
{
	static ScePspFMatrix4 at_node[SKIN_NODES];
	static unsigned char done[SKIN_NODES];
	unsigned char *records = (unsigned char *)draw->vertices;
	int v;

	if (!placed)
	{
		unsigned char *copy = list_take(vertex_count * 24);

		if (!copy)
			return;
		memcpy(copy, records, vertex_count * 24);
		records = copy;
	}
	memset(done, 0, sizeof(done));
	for (v = 0; v < vertex_count; v++)
	{
		unsigned char *record = records + 24 * v;
		float n[3] = { (signed char)record[8], (signed char)record[9], (signed char)record[10] };
		const ScePspFMatrix4 *m = &draw->matrix;
		float wn[3], nn, c, a;

		if (placed)
		{
			int node = record[11] / 3;

			if (node >= SKIN_NODES || node >= model_node_count)
				node = 0;
			if (!done[node])
			{
				halo_matrix(model_nodes + 13 * node, &at_node[node]);
				done[node] = 1;
			}
			m = &at_node[node];
		}
		wn[0] = m->x.x * n[0] + m->y.x * n[1] + m->z.x * n[2];
		wn[1] = m->x.y * n[0] + m->y.y * n[1] + m->z.y * n[2];
		wn[2] = m->x.z * n[0] + m->y.z * n[1] + m->z.z * n[2];
		nn = wn[0] * wn[0] + wn[1] * wn[1] + wn[2] * wn[2];
		if (mode == SEETHROUGH_LIT)
		{
			*(unsigned int *)(record + 8) = seethrough_lit(seethrough_tinted(seethrough_color((int)(fade * 255.0f + 0.5f),
				draw->blend), draw->tint), wn, nn);
			continue;
		}
		c = nn > 0.0f ? fabsf(wn[0] * camera_forward[0] + wn[1] * camera_forward[1] + wn[2] * camera_forward[2]) /
			sqrtf(nn) : 1.0f;
		a = mode == 0 ? 1.0f : mode == 1 ? 1.0f - c : c;
		*(unsigned int *)(record + 8) = seethrough_tinted(seethrough_color((int)(fade * a * 255.0f + 0.5f), draw->blend),
			draw->angle_tinted ? seethrough_angle_tint(draw, c) : draw->tint);
	}
	draw->vertices = records;
	draw->colored = 1;
	if (mode == SEETHROUGH_LIT)
		seethrough_lit_draws++;
	else if (mode != 0)
		seethrough_angle_draws++;
	if (draw->angle_tinted)
		seethrough_angle_tinted_draws++;
}

static struct transparent_draw *transparent_slot(unsigned int key, float u_scale, float v_scale, float u_offset,
	float v_offset, short blend, const float *centroid)
{
	struct transparent_draw *draw;

	if (transparent_full())
	{
		transparent_dropped++;
		return NULL;
	}
	draw = &transparent[transparent_count++];
	draw->one_sided = 0;
	draw->occluder = 0;
	draw->sorted_last = 0;
	draw->color = 0xFFFFFFFF; /* build 116: no fade unless the filler sets one */
	draw->colored = 0;
	draw->tint = 0xFFFFFFFF;
	draw->clut = NULL; /* build 116: a meter's own palette, or none */
	draw->doubled = 0;
	draw->tint_model = SEETHROUGH_MODULATE;
	draw->shader = 0;
	memset(&draw->matrix, 0, sizeof(draw->matrix));
	draw->matrix.x.x = draw->matrix.y.y = draw->matrix.z.z = draw->matrix.w.w = 1.0f;
	relative(&draw->matrix);
	draw->key = key;
	draw->u_scale = u_scale != 0.0f ? u_scale : 1.0f;
	draw->v_scale = v_scale != 0.0f ? v_scale : 1.0f;
	draw->u_offset = u_offset;
	draw->v_offset = v_offset;
	/* build 116: the level's see-through surfaces take the fade's one
	colour; their fade by angle isn't drawn yet (no normals in the level's
	GE copies) */
	draw->blend = PSP_SEETHROUGH_BLEND(blend);
	draw->tint = seethrough_next_tint;
	draw->shader = seethrough_next_shader;
	draw->doubled = seethrough_next_doubled;
	draw->tint_model = seethrough_next_model;
	draw->depth_mode = seethrough_next_depth;
	draw->angle_tinted = 0;
	draw->color = seethrough_tinted(seethrough_fades(draw->blend) && PSP_SEETHROUGH_FADE(blend) < 1.0f ?
		seethrough_color((int)(PSP_SEETHROUGH_FADE(blend) * 255.0f + 0.5f), draw->blend) : 0xFFFFFFFF, draw->tint);
	draw->colored = 0;
	draw->sky = 0;
	draw->sprites = 0;
	draw->strip = 0;
	draw->depth_units = 0;
	draw->cut_vertices = NULL;
	draw->cut_count = 0;
	/* build 89: nothing of a previous frame's draw left in the slot: its
	filler sets these, and a slot left unfilled is skipped at the flush */
	draw->vertices = NULL;
	draw->indices = NULL;
	draw->index_count = 0;
	/* build 63: never set here before, so a level slot kept whatever the
	slot's last user left: after a frame with more first-person see-through
	parts (the pistol's), some of the level's painted symbols, the cave
	fungus and the teleporter's beam went out in the weapon's depth slice,
	in front of everything (b62's shots 6, 18-21; "tdump" showed
	level draws marked first-person). Turning or zooming changed the counts
	and the slots, so it came and went */
	draw->first_person = 0;
	draw->depth_test = 1;
	draw->distance = 0.0f;
	if (centroid)
	{
		float dx = centroid[0] - camera_eye[0], dy = centroid[1] - camera_eye[1], dz = centroid[2] - camera_eye[2];

		draw->distance = dx * dx + dy * dy + dz * dz;
	}
	return draw;
}

/* build 60: how far to nudge a level overlay triangle toward the eye, in
depth steps. Two parts:
- the distance part (build 54): about half a centimetre at the triangle's
  nearest corner;
- the slope part: one pixel's worth of the triangle's depth change across the
  screen. The GE snaps each corner to a sixteenth of a pixel and to a whole
  depth step and interpolates from there, so the wall and the symbol painted
  on it, made of different triangles, disagree by up to some fraction of
  a pixel's depth change, more where their corners lie far off. Seen
  face-on that is a step or two; seen at a slant it is tens of steps a pixel,
  and a fixed nudge lost: the symbols went missing on one side of a line
  across them (b59's shots 78-80). 1/z is linear across the screen for a
  plane n.P = d in camera space: per pixel it changes by |n.x| or |n.y| /
  (pixels per unit x d), and the depth by that times near x 61439.
"overlaynoslope": the distance part only (b54-b59) */
/* build 62: two pixels' worth (b60-61: one; b61's run: the symbols still
cut off). "overlayslope1" / "overlayslope4": one or four */
static float overlay_slope_pixels = 2.0f;
static int overlay_slope = -1;

/* the slope part alone: overlay_slope_pixels pixels of a camera-space
triangle's depth change across the screen, in depth steps (build 65: shared
with the decals) */
static float slope_units(const float *a, const float *b, const float *c)
{
	float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
	float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
	float length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);

	if (overlay_slope < 0)
	{
		overlay_slope = !psp_keep("overlaynoslope");
		overlay_slope_pixels = psp_keep("overlayslope1") ? 1.0f : psp_keep("overlayslope4") ? 4.0f : 2.0f;
	}
	if (overlay_slope && length > 0.0f)
	{
		float plane = fabsf(n[0] * a[0] + n[1] * a[1] + n[2] * a[2]) / length;
		float across = fabsf(n[0]) > fabsf(n[1]) ? fabsf(n[0]) : fabsf(n[1]);

		if (plane < 0.001f)
			plane = 0.001f;
		return overlay_slope_pixels * 61439.0f * projection_near * (across / length) / (pixels_per_unit() * plane);
	}
	return 0.0f;
}

/* build 76: the slope part's ceiling. slope_units() grows without bound as a
surface turns edge-on (its plane passes near the eye), up to the 4096-step
cap; at the 16-bit depth's far end a step is metres (z^2 / (near x 61439):
0.33 units at 50 units, near x2), so a symbol, a wall stencil or a decal
batch could beat whole walls in front of it for the frame or two it was
edge-on (b75's run: "anything 2d ... visible through walls for a frame
or two"). Graphics APIs have the same knob: polygon offset clamp.
build 80: measured as the LIFT OFF THE SURFACE, not along the line of
sight. b76's 0.03 units along the view was barely a depth step at 25-30
units, and held back 80-98% of the slope nudges (b79 run 3): the symbols
at red base tore across and one sank into its wall, b60's bug again. A
nudge of d along the view lifts a point off its plane by d x cos (the
angle between the view ray and the plane's normal), so a wall seen at a
slant may move a long way toward the eye while staying glued to it in 3D,
which is exactly the case that needs the big nudge; a decal far off still
can't jump tens of units through a cliff. The lift allowed is a share of
the distance (the debug menu: off, 1, 2, 4, 8%; default 2), never under
0.01 units (3 cm). "nudgewide": no ceiling (b60-b75) */
static int nudge_wide = -1;
static unsigned long nudges_clamped = 0, nudges_asked = 0;
/* A surface seen nearly edge-on gets no slope part. Its depth changes by
thousands of steps across two pixels, so the slope part (uncapped: the lift
limit is off by default) moved it most of the way to the eye, and a light
strip on a wall behind a pillar drew as a dashed bright line on the pillar's
face. The Xbox draws the level's overlays with no depth bias at all, so it
never shows them through what stands in front. Edge-on, a surface covers a
pixel or two, and losing its slope part costs at most those pixels to its
own wall. "nudgeedgeN": the cosine (in hundredths) between the view and the
surface's normal under which the slope part is dropped; "nudgeedge0" keeps
it at every angle */
static float nudge_edge_cos = -1.0f;
static unsigned long nudges_edge_on = 0;
static float slope_units_capped(float units, const float *a, const float *b, const float *c, float reference)
{
	float ceiling, nearest = a[2], lift, cos_angle, length;
	float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
	float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
	float m[3] = { (a[0] + b[0] + c[0]) / 3.0f, (a[1] + b[1] + c[1]) / 3.0f, (a[2] + b[2] + c[2]) / 3.0f };

	if (nudge_wide < 0)
		nudge_wide = psp_keep("nudgewide");
	if (nudge_edge_cos < 0.0f)
	{
		extern int psp_keep_number(const char *prefix, int fallback);

		nudge_edge_cos = psp_keep_number("nudgeedge", 10) * 0.01f;
	}
	nudges_asked++;
	length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) * sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
	cos_angle = length > 0.0f ? fabsf(n[0] * m[0] + n[1] * m[1] + n[2] * m[2]) / length : 1.0f;
	if (units > 0.0f && cos_angle < nudge_edge_cos)
	{
		nudges_edge_on++;
		return 0.0f;
	}
	if (nudge_wide || nudge_percent <= 0)
		return units;
	if (b[2] < nearest) nearest = b[2];
	if (c[2] < nearest) nearest = c[2];
	/* build 118: the depth the cap is measured at, when the caller gives one
	(the triangle's farthest corner, overlay_units) */
	if (reference > 0.0f)
		nearest = reference;
	if (nearest < projection_near)
		nearest = projection_near;
	/* the lift allowed, then the distance along the view that gives it */
	lift = nudge_percent * 0.01f * nearest;
	if (lift < 0.01f)
		lift = 0.01f;
	if (cos_angle < 0.02f)
		cos_angle = 0.02f;
	ceiling = (lift / cos_angle) * projection_near * 61439.0f / (nearest * nearest);
	if (units > ceiling)
	{
		nudges_clamped++;
		return ceiling;
	}
	return units;
}

/* build 65: a triangle standing free (not on a wall) is nudged away from the
eye instead, by the distance part: where its edge meets something solid (the
teleporter's shield inside its frame, b64's shot 37: a green line along
the frame's edge) the solid side now wins the tie. Returned negative.
build 115: "overlayfreefar" (A/B; b114's run: b30's ocean "clips through
the terrain under the water surface to varying degrees" in the flight). The
ocean is free (the collision has no surface at the water: water is a fog
plane there), and a draw goes out at the LARGEST push of its triangles. A
triangle with a corner at the near plane asks 0.005 units of lift there,
307/near steps (2,457 at x2, the 4,096 cap at x1): hundreds of units at
70 units off, so the water lost to any seabed under it; triangles wholly in
view asked the 3-step floor, about 2 units at 70 off at x2. With the word:
the lift is measured at the triangle's FARTHEST corner, the floor is one
step (the solid side still wins an exact tie), and a draw takes the
SMALLEST push of its free triangles (one constant offset is a lift that
grows with the square of the distance, so the far end decides).
build 116: on by default, a restoration (the ocean behind the seabed, as
the Xbox draws it: b115's test 5 on the PSP); keep word
"overlayfreenear": as builds 65-115 */
static int overlay_free_far = -1, overlay_wall_near = -1;
static int overlay_units(const float *a, const float *b, const float *c, int on_a_wall)
{
	float nearest = a[2], units;

	if (b[2] < nearest) nearest = b[2];
	if (c[2] < nearest) nearest = c[2];
	if (nearest < projection_near)
		nearest = projection_near;
	{
		float lift = nudge_lift_share * nearest;

		if (lift > nudge_lift_ceiling)
			lift = nudge_lift_ceiling;
		if (lift < 0.005f)
			lift = 0.005f;
		units = lift * 61439.0f * projection_near / (nearest * nearest);
	}
	if (!on_a_wall)
	{
		if (overlay_free_far < 0)
			overlay_free_far = !psp_keep("overlayfreenear");
		if (overlay_free_far)
		{
			/* build 115: "overlayfreefar" (above; the default from build 116): measured at the far corner */
			float farthest = a[2], lift;

			if (b[2] > farthest) farthest = b[2];
			if (c[2] > farthest) farthest = c[2];
			if (farthest < projection_near)
				farthest = projection_near;
			lift = nudge_lift_share * farthest;
			if (lift > nudge_lift_ceiling)
				lift = nudge_lift_ceiling;
			if (lift < 0.005f)
				lift = 0.005f;
			units = lift * 61439.0f * projection_near / (farthest * farthest);
			return units < 1.0f ? -1 : units > 4096.0f ? -4096 : -(int)units;
		}
		return units < 3.0f ? -3 : units > 4096.0f ? -4096 : -(int)units;
	}
	/* build 118: an on-wall triangle's nudge measured at its FARTHEST corner,
	as build 115 did for free ones. One depth offset is a lift that grows
	with the square of the distance, so a triangle reaching from the near
	plane outward, its nudge sized at the near end (up to the 4,096-step
	cap), was lifted ~10 units at 3 units off: b30's bunker glass floor
	drawn through the hallway wall (b113 #13, b116 test 1, the emulator's
	pointlights118 batch [E]), and likely c40's bridge glass through Chief's legs
	(b116 test 3) [U]. Keep word "overlaywallnear": as before */
	if (overlay_wall_near < 0)
		overlay_wall_near = psp_keep("overlaywallnear");
	if (!overlay_wall_near)
	{
		float farthest = a[2], lift;

		if (b[2] > farthest) farthest = b[2];
		if (c[2] > farthest) farthest = c[2];
		if (farthest < projection_near)
			farthest = projection_near;
		lift = nudge_lift_share * farthest;
		if (lift > nudge_lift_ceiling)
			lift = nudge_lift_ceiling;
		if (lift < 0.005f)
			lift = 0.005f;
		units = lift * 61439.0f * projection_near / (farthest * farthest);
		units += slope_units_capped(slope_units(a, b, c), a, b, c, farthest) + nudge_floor;
		return units < 3.0f ? 3 : units > 4096.0f ? 4096 : (int)units;
	}
	units += slope_units_capped(slope_units(a, b, c), a, b, c, 0.0f) + nudge_floor;
	return units < 3.0f ? 3 : units > 4096.0f ? 4096 : (int)units;
}

/* the depth-step buckets level overlay triangles are drawn in: a quarter of a
doubling apart, so a draw's nudge is at most 19% over a triangle's own */
#define OVERLAY_BUCKETS 52
static unsigned char overlay_bucket_of(int units)
{
	int bucket;

	/* bucket 0: pushed away (free-standing); a nudge toward the eye is at
	least 3 steps, bucket 6 and up */
	if (units < 0)
		return 0;
	bucket = (int)(log2f((float)units) * 4.0f);
	if (bucket < 1)
		bucket = 1;

	return (unsigned char)(bucket < 0 ? 0 : bucket >= OVERLAY_BUCKETS ? OVERLAY_BUCKETS - 1 : bucket);
}
#define OVERLAY_TRIANGLES 2048
static unsigned char overlay_triangle_bucket[OVERLAY_TRIANGLES];

/* build 64: whether a see-through level triangle lies on a wall. The slope
part of the nudge is for symbols painted on a wall; a surface standing free
(the teleporter's shield: a thin upright panel) seen nearly edge-on got the
largest nudge of all and drew in front of the frame round it (b62-b63:
the beam through the frame, near its axis). Asked once per triangle of the
game's collision: is there a surface within 4 cm in front of it or behind
it (a ray through its middle along its normal; the see-through surfaces
themselves are render-only, not in the collision BSP)? "overlaynowall":
every triangle as on a wall (b60-b63) */
struct collision_result_space { unsigned char bytes[0x50]; } __attribute__((aligned(4)));
extern unsigned char collision_test_vector(unsigned long flags, const float *point, const float *vector,
	long ignore_object_index, struct collision_result_space *collision);
/* build 84: 2048, replaced round-robin when full (b81's 512 filled within a
minute at one base, b83's run: 31,250 lookups overflowed and were treated
as on a wall, the beam among them) */
#define OVERLAY_BACKINGS 2048
/* build 81: keyed by the indices' LASTING identity. Since build 68 the
level's indices live in the geometry cache's blocks, and a block is reused
for other data once evicted: keyed by the pointer (b64-b80), red base's
symbols found blue base's answers at the same address (the two bases are
mirror copies, the same counts), so some triangles were "free" and nudged
AWAY from the eye into their wall. The symbols tore along triangle edges and
one sank entirely (b79-b80's runs at red base); in the whole of those
runs only the first 226 triangles were ever asked. The key is the pack
address when the indices are the cache's, the pointer otherwise */
static struct
{
	unsigned int key;
	long count;
	unsigned char *flags; /* per triangle: 0 not asked, 1 on a wall, 2 free */
} overlay_backing[OVERLAY_BACKINGS] PSP_MAIN_SIDE; /* build 118: the main CPU's, as calls are written down */
static PSP_MAIN_SIDE int overlay_backing_count = 0, overlay_all_walls = -1;
/* after b116: the last list's flags, so its triangles don't search the
table again each (it holds up to 2,048 lists; an independent review) */
static PSP_MAIN_SIDE unsigned int overlay_cached_key = 0;
static PSP_MAIN_SIDE long overlay_cached_count = -1;
static PSP_MAIN_SIDE unsigned char *overlay_cached_flags = NULL;

static void overlay_backing_forget(void)
{
	int index;

	for (index = 0; index < overlay_backing_count; index++)
		free(overlay_backing[index].flags);
	overlay_backing_count = 0;
	overlay_cached_flags = NULL;
}
static PSP_MAIN_SIDE unsigned long overlay_wall_asks = 0, overlay_on_walls = 0, overlay_free = 0, overlay_ask_us = 0;

static PSP_MAIN_SIDE unsigned long overlay_backing_full = 0;

/* "overlaymemo" (on by default; "overlaymemooff": as before): a list whose
answers find no heap is asked again for each of
its triangles (overlay_answers_for), so a full heap failed one calloc per
triangle per frame: b30's burst at heaproom256 (heapwho1) was ~7,000 failed
asks a game second, each a walk of the free lists. Remembering the refused
list for the rest of the frame keeps every answer the same (no room means on
a wall either way) and asks the heap once */
static PSP_MAIN_SIDE int overlay_memo = -1;
static PSP_MAIN_SIDE unsigned int overlay_refused_key = 0, overlay_refused_frame = 0;
static PSP_MAIN_SIDE unsigned long overlay_no_room = 0, overlay_no_room_skipped = 0;

/* build 112: triangles whose answer work marked as the Media Engine's needed
and didn't have (gemesim): finding it takes heap for the answers and casts
the engine's collision rays, which read the game's data while the main CPU
may be moving it. The main CPU asks them at the frame's end
(psp_ge_frame_end); meanwhile the triangle counts as on a wall, as it does
when there's no memory for its answers */
#define WALLS_WANTED 64
static struct
{
	const struct level_stream *stream;
	const unsigned short *indices;
	long triangle_count, triangle;
} walls_wanted[WALLS_WANTED];
static int walls_wanted_count = 0;
static unsigned long walls_wanted_total = 0, walls_wanted_dropped = 0;

static int overlay_want(const struct level_stream *stream, const unsigned short *indices, long triangle_count, long triangle)
{
	if (walls_wanted_count < WALLS_WANTED)
	{
		walls_wanted[walls_wanted_count].stream = stream;
		walls_wanted[walls_wanted_count].indices = indices;
		walls_wanted[walls_wanted_count].triangle_count = triangle_count;
		walls_wanted[walls_wanted_count].triangle = triangle;
		walls_wanted_count++;
		walls_wanted_total++;
	}
	else
		walls_wanted_dropped++;
	return 1;
}

static int overlay_on_a_wall(const struct level_stream *copy, const unsigned short *indices, long triangle_count,
	long triangle)
{
	const struct ge_vertex *stream = copy->base;
	int slot;
	unsigned char *flags = NULL;
	unsigned int key;

	if (overlay_all_walls < 0)
		overlay_all_walls = psp_keep("overlaynowall");
	if (overlay_all_walls)
		return 1;
	/* build 89: keyed by the list's CONTENT (its indices hashed, with the
	material's copies): the see-through level lists come from the game's
	per-frame triangle pool, never the geometry cache, so b81's pack-address
	key was the raw pointer here, the Nth slot of this frame's pool rather
	than a surface, and another overlay with the same triangle count could
	inherit its answers (review). Worked out once per list per frame */
	{
		static PSP_MAIN_SIDE const unsigned short *last_list = NULL;
		static PSP_MAIN_SIDE const struct ge_vertex *last_stream = NULL;
		static PSP_MAIN_SIDE unsigned int last_frame_seen = 0, last_key = 0;
		static PSP_MAIN_SIDE long last_count = -1;

		if (indices != last_list || stream != last_stream || ge_hot.stream_frame != last_frame_seen ||
			triangle_count != last_count)
		{
			unsigned int h = 2166136261u ^ (unsigned int)stream;
			long i;

			for (i = 0; i < triangle_count * 3; i++)
				h = (h ^ indices[i]) * 16777619u;
			last_list = indices;
			last_stream = stream;
			last_frame_seen = ge_hot.stream_frame;
			last_count = triangle_count;
			last_key = h | 1;
		}
		key = last_key;
	}
	/* (the main CPU's alone: the Media Engine's cache doesn't see its writes) */
	if (!PSP_ME_WORK() && overlay_cached_flags && key == overlay_cached_key && triangle_count == overlay_cached_count)
		flags = overlay_cached_flags;
	else
		for (slot = 0; slot < overlay_backing_count; slot++)
			if (overlay_backing[slot].key == key && overlay_backing[slot].count == triangle_count)
			{
				flags = overlay_backing[slot].flags;
				break;
			}
	if (!flags && PSP_ME_WORK())
		return overlay_want(copy, indices, triangle_count, triangle);
	if (!flags)
	{
		if (overlay_memo < 0)
			overlay_memo = !psp_keep("overlaymemooff");
		if (overlay_memo && key == overlay_refused_key && ge_hot.stream_frame == overlay_refused_frame)
		{
			overlay_no_room_skipped++;
			return 1;
		}
		if (!(flags = calloc(triangle_count, 1)))
		{
			overlay_no_room++;
			overlay_refused_key = key;
			overlay_refused_frame = ge_hot.stream_frame;
			return 1;
		}
		if (overlay_backing_count >= OVERLAY_BACKINGS)
		{
			static PSP_MAIN_SIDE int victim = 0;

			slot = victim;
			victim = (victim + 1) % OVERLAY_BACKINGS;
			if (overlay_cached_flags == overlay_backing[slot].flags)
				overlay_cached_flags = NULL;
			free(overlay_backing[slot].flags);
			overlay_backing_full++;
		}
		else
			slot = overlay_backing_count++;
		overlay_backing[slot].key = key;
		overlay_backing[slot].count = triangle_count;
		overlay_backing[slot].flags = flags;
	}
	if (!PSP_ME_WORK())
	{
		overlay_cached_key = key;
		overlay_cached_count = triangle_count;
		overlay_cached_flags = flags;
	}
	if (!flags[triangle] && PSP_ME_WORK())
		return overlay_want(copy, indices, triangle_count, triangle);
	if (!flags[triangle])
	{
		const struct ge_vertex *a = &stream[indices[3 * triangle]], *b = &stream[indices[3 * triangle + 1]],
			*c = &stream[indices[3 * triangle + 2]];
		float e1[3] = { b->x - a->x, b->y - a->y, b->z - a->z }, e2[3] = { c->x - a->x, c->y - a->y, c->z - a->z };
		float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
		float length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		unsigned int start = ge_clock();

		flags[triangle] = 1;
		if (length > 0.0f)
		{
			struct collision_result_space result;
			float point[3], vector[3];
			int k;

			for (k = 0; k < 3; k++)
			{
				/* the collision is the world's */
				float middle = ((&a->x)[k] + (&b->x)[k] + (&c->x)[k]) * (1.0f / 3.0f) + copy->origin[k];

				point[k] = middle + n[k] / length * 0.04f;
				vector[k] = -n[k] / length * 0.08f;
			}
			/* front- and back-facing surfaces of the structure */
			/* build 65: objects too (scenery, machines: a symbol may be painted
			on a scenery piece) */
			flags[triangle] = collision_test_vector(0xC0A3, point, vector, -1, &result) ? 1 : 2;
			{
				static PSP_MAIN_SIDE int walldump = -1;

				if (walldump < 0)
					walldump = psp_keep("walldump");
				if (walldump)
				{
					/* the nearest structure surface on each side, within 25 cm */
					float side_point[3], side_vector[3], gap[2] = { -1.0f, -1.0f };
					int side;

					for (side = 0; side < 2; side++)
					{
						float sign = side ? -1.0f : 1.0f;

						for (k = 0; k < 3; k++)
						{
							side_point[k] = point[k] - n[k] / length * 0.04f;
							side_vector[k] = sign * n[k] / length * 0.25f;
						}
						if (collision_test_vector(0x23, side_point, side_vector, -1, &result))
							gap[side] = *(const float *)(result.bytes + 0x14) * 0.25f;
					}
					platform_log("walldump: key-less triangle %ld of %p: %s; nearest surface in front %.3f, behind %.3f (-1: none within 25 cm)",
						triangle, (const void *)indices, flags[triangle] == 1 ? "on a wall" : "free", gap[0], gap[1]);
				}
			}
		}
		overlay_wall_asks++;
		if (flags[triangle] == 1)
			overlay_on_walls++;
		else
			overlay_free++;
		overlay_ask_us += ge_clock() - start;
	}
	return flags[triangle] == 1;
}

/* build 112: a triangle's wall answer: the one its record carried while
the Media Engine's work carries the call out (overlay_answers), else asked
here */
static int overlay_answer(const struct level_stream *stream, const unsigned short *indices, long triangle_count,
	long triangle)
{
	if (overlay_answers)
		return overlay_answers[triangle];
	/* (the ME's work without answers touches none of the answers' tables,
	which the main CPU fills as calls are written down: it only wants) */
	if (PSP_ME_WORK())
		return overlay_want(stream, indices, triangle_count, triangle);
	return overlay_on_a_wall(stream, indices, triangle_count, triangle);
}

/* every triangle's answer of a see-through level list, worked out on the
main CPU as the call is written down (ME semantics): 1 if out[] holds them,
0 if the level copy isn't there to ask from (the carrying out then asks, or
wants, as before) */
static int overlay_answers_for(const void *vertices, int vertex_count, const unsigned short *indices,
	long triangle_count, unsigned char *out)
{
	/* an existing copy only: building one here would change the copies'
	table and arena while the ME may be reading them (copies are built at
	the frame's end) */
	struct level_stream *stream = level_stream_find(vertices);
	long t;

	if (!stream)
		return 0;
	for (t = 0; t < triangle_count; t++)
		out[t] = (unsigned char)overlay_on_a_wall(stream, indices, triangle_count, t);
	return 1;
}

/* build 87: a baked stencil's layers: its triangles on the pieces of the wall
triangles drawn this frame, cut at the near plane and guard band as those
pieces were (the same positions, the same cuts), queued as see-through draws
that the flush tests with depth EQUAL */
static void bake_queue_layers(unsigned int stencil_key, unsigned int key, float u_scale, float v_scale, float u_offset,
	float v_offset, short blend, const float *centroid)
{
	int k;

	for (k = 0; k < bake.layer_count; k++)
	{
		struct bake_layer *l = &bake.layers[k];
		struct transparent_draw *draw;
		unsigned short *kept;
		long t, kept_count = 0;
		int cut_count = 0;
		struct cut_store pieces_store;

		if (l->stencil_key != stencil_key || !l->wall || !l->wall->drawn_frame)
			continue;
		level_frame(l->placed ? l->origin : world_origin);
		cut_store_begin(&pieces_store, cut_corners, MAXIMUM_CUT_CORNERS, CUT_OTHER);
		kept = (unsigned short *)list_take((int)(l->tri_count * 3 * sizeof(unsigned short) + 3) & ~3);
		if (!kept)
			return;
		for (t = 0; t < (long)l->tri_count; t++)
		{
			const unsigned short *tri = l->tris + 4 * t;
			int bits[3], corner;

			if (tri[3] >= l->wall->affected_count || l->wall->drawn_frame[tri[3]] != ge_hot.stream_frame)
				continue;
			for (corner = 0; corner < 3; corner++)
			{
				float point[3];

				level_to_camera(&l->vertices[tri[corner]].x, point);
				bits[corner] = outside(point);
			}
			if (!(bits[0] | bits[1] | bits[2]))
			{
				kept[3 * kept_count] = tri[0];
				kept[3 * kept_count + 1] = tri[1];
				kept[3 * kept_count + 2] = tri[2];
				kept_count++;
			}
			else if (!(bits[0] & bits[1] & bits[2]))
			{
				float corners[3][CORNER_FLOATS] = { { 0.0f } };

				for (corner = 0; corner < 3; corner++)
				{
					const struct ge_vertex *v = &l->vertices[tri[corner]];

					level_to_camera_corner(&v->x, corners[corner]);
					corners[corner][3] = v->u;
					corners[corner][4] = v->v;
				}
				cut_count += clip_triangle(corners, bits[0] | bits[1] | bits[2], &pieces_store);
			}
		}
		if (kept_count && (draw = transparent_slot(key, u_scale, v_scale, u_offset, v_offset, blend, centroid)) != NULL)
		{
			level_matrix(&draw->matrix, l->placed ? l->origin : world_origin);
			draw->vertices = l->vertices;
			draw->indices = kept;
			draw->index_count = (int)kept_count * 3;
			draw->level = 3;
			bake_layer_draws++;
			bake_layer_triangles += kept_count;
		}
		if (cut_count && (draw = transparent_slot(key, u_scale, v_scale, u_offset, v_offset, blend, centroid)) != NULL)
		{
			struct ge_vertex *out = list_take(cut_count * sizeof(struct ge_vertex));
			int index;

			if (!out)
			{
				/* build 89: the slot given back (it held a previous
				frame's pointers, drawn as garbage at the flush; review) */
				transparent_count--;
				return;
			}
			for (index = 0; index < cut_count; index++)
			{
				const float *c = cut_at(&pieces_store, index);

				out[index].u = c[3];
				out[index].v = c[4];
				out[index].x = c[7];
				out[index].y = c[8];
				out[index].z = c[9];
			}
			level_matrix(&draw->matrix, l->placed ? l->origin : world_origin);
			draw->vertices = out;
			draw->indices = NULL;
			draw->index_count = cut_count;
			draw->level = 4;
			bake_layer_draws++;
		}
	}
}

static int bake_stencil_has(const struct bake_stencil *st, const unsigned short *tri)
{
	unsigned int slot = bake_triple_hash(tri[0], tri[1], tri[2]) & st->hash_mask;

	while (st->hash[slot])
	{
		const unsigned short *b = st->triples + 3 * (st->hash[slot] - 1);

		if (b[0] == tri[0] && b[1] == tri[1] && b[2] == tri[2])
			return 1;
		slot = (slot + 1) & st->hash_mask;
	}
	return 0;
}

/* build 116: a level see-through surface that fades by the angle it is seen
at (the volume lights' cones and shafts: c40's pale discs, drawn flat at
full strength until now). The level's GE copies have no normals, so each
triangle's own plane stands in: its corners take the fade from the angle
between that plane and the line to the eye, in a coloured copy of the
triangles in view (and the pieces of those cut at the screen's edges),
drawn as the cut-up corners are (level 2). The engine side leaves the fade
mode out under "seethroughflat", and this path with it */
/* at most this many triangles a surface take the coloured copy (36 KB of
the display list); a bigger one draws unfaded, as before (review: 2,048
was 150 KB a surface; c40's light volumes are 16 triangles each) */
#define LEVEL_ANGLE_TRIANGLES 512
static unsigned long level_angle_draws = 0;

static void level_angle_colors(struct sprite_vertex *t, int mode, float fade, int blend, unsigned int tint)
{
	float e1[3] = { t[1].x - t[0].x, t[1].y - t[0].y, t[1].z - t[0].z };
	float e2[3] = { t[2].x - t[0].x, t[2].y - t[0].y, t[2].z - t[0].z };
	float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
	float nn = n[0] * n[0] + n[1] * n[1] + n[2] * n[2];
	int k;

	/* the models' rule (seethrough_vertex_colors): against the camera's
	forward direction, mode 1 vanishing face on, mode 2 edge on */
	float c = nn > 0.0f ? fabsf(n[0] * camera_forward[0] + n[1] * camera_forward[1] + n[2] * camera_forward[2]) /
		sqrtf(nn) : 1.0f;
	float a = mode == 1 ? 1.0f - c : c;
	unsigned int color = seethrough_tinted(seethrough_color((int)(fade * a * 255.0f + 0.5f), blend), tint);

	for (k = 0; k < 3; k++)
		t[k].color = color;
}

/* 1: queued (or nothing in view); 0: left to the usual path (no room). The
caller has reset vertex_codes over the triangles' range */
static int level_transparent_angle(const struct level_stream *stream, const unsigned short *indices,
	long triangle_count, unsigned int key, float u_scale, float v_scale, float u_offset, float v_offset, short blend,
	const float *centroid)
{
	struct transparent_draw *draw;
	struct sprite_vertex *out;
	long index, kept_count = 0;
	int cut_count = 0, units, on_wall = 0, free_push = 0, mode = PSP_SEETHROUGH_MODE(blend), total, at = 0, corner;
	float fade = PSP_SEETHROUGH_FADE(blend);
	struct cut_store pieces_store;

	cut_store_begin(&pieces_store, cut_corners, MAXIMUM_CUT_CORNERS, CUT_OTHER);
	/* which triangles are in view, the pushes they ask, the cut pieces */
	for (index = 0; index < triangle_count; index++)
	{
		const unsigned short *triangle = indices + 3 * index;
		int bits[3];
		float p[3][CORNER_FLOATS] = { { 0.0f } };

		for (corner = 0; corner < 3; corner++)
		{
			int code = vertex_codes[triangle[corner]];

			if (code & CODE_UNKNOWN)
			{
				float point[3];

				level_to_camera(&stream->base[triangle[corner]].x, point);
				code = outside(point);
				vertex_codes[triangle[corner]] = (unsigned char)code;
			}
			bits[corner] = code;
		}
		if (bits[0] & bits[1] & bits[2])
			continue;
		for (corner = 0; corner < 3; corner++)
		{
			const struct ge_vertex *b = &stream->base[triangle[corner]];

			level_to_camera_corner(&b->x, p[corner]);
			p[corner][3] = b->u;
			p[corner][4] = b->v;
		}
		units = seethrough_next_depth & SEETHROUGH_EXACT_DEPTH ? 0 :
			overlay_units(p[0], p[1], p[2], overlay_answer(stream, indices, triangle_count, index));
		if (units > on_wall)
			on_wall = units;
		if (units < 0 && (overlay_free_far > 0 ? !free_push || -units < free_push : -units > free_push))
			free_push = -units;
		if (!(bits[0] | bits[1] | bits[2]))
			kept_count++;
		else
			cut_count += clip_triangle(p, bits[0] | bits[1] | bits[2], &pieces_store);
	}
	total = (int)kept_count * 3 + cut_count;
	if (!total)
		return 1;
	out = list_take(total * (int)sizeof(struct sprite_vertex));
	if (!out)
		return 0;
	if ((draw = transparent_slot(key, u_scale, v_scale, u_offset, v_offset, blend, centroid)) == NULL)
		return 1;
	/* the triangles wholly in view, then the cut pieces (world positions) */
	for (index = 0; index < triangle_count && kept_count; index++)
	{
		const unsigned short *triangle = indices + 3 * index;

		if (vertex_codes[triangle[0]] | vertex_codes[triangle[1]] | vertex_codes[triangle[2]])
			continue;
		for (corner = 0; corner < 3; corner++)
		{
			const struct ge_vertex *b = &stream->base[triangle[corner]];

			out[at].u = b->u;
			out[at].v = b->v;
			out[at].x = b->x;
			out[at].y = b->y;
			out[at].z = b->z;
			at++;
		}
	}
	for (index = 0; index < cut_count; index++)
	{
		const float *c = cut_at(&pieces_store, index);

		out[at].u = c[3];
		out[at].v = c[4];
		out[at].x = c[7];
		out[at].y = c[8];
		out[at].z = c[9];
		at++;
	}
	for (index = 0; index + 2 < at; index += 3)
		level_angle_colors(out + index, mode, fade, draw->blend, draw->tint);
	level_matrix(&draw->matrix, stream->origin);
	draw->vertices = out;
	draw->indices = NULL;
	draw->index_count = at - at % 3;
	draw->level = 2;
	draw->colored = 1;
	draw->depth_units = (short)(on_wall > 0 ? on_wall : -free_push);
	level_transparent_draws++;
	level_angle_draws++;
	seethrough_angle_draws++;
	return 1;
}

static void psp_ge_queue_level_transparent_body(const void *vertices, int vertex_count, const unsigned short *indices,
	long triangle_count, unsigned int key, float u_scale, float v_scale, float u_offset, float v_offset, short blend,
	const float *centroid)
{
	const struct bake_stencil *baked = NULL;
	struct level_stream *stream;
	unsigned short *kept;
	long index, kept_count = 0;
	unsigned int lowest = 0xFFFF, highest = 0;
	int cut_count = 0, units, cut_units = 0, cut_units_free = 0, bucket;
	unsigned short bucket_units[OVERLAY_BUCKETS];
	struct transparent_draw *draw;
	struct cut_store pieces_store;

	int exact = (seethrough_next_depth & SEETHROUGH_EXACT_DEPTH) != 0;

	if (!frame_open || triangle_count <= 0 || !vertices || !indices || vertex_count <= 0 || list_full())
		return;
	memset(bucket_units, 0, sizeof(bucket_units));
	/* build 87: a baked stencil is drawn on its walls' pieces; the triangles
	the bake didn't take keep the old path below */
	baked = bake_stencil_of((unsigned int)stream_key(vertices));
	if (baked)
		bake_queue_layers(baked->key, key, u_scale, v_scale, u_offset, v_offset, blend, centroid);
	stream_asked_texture = key;
	stream = level_stream_get(vertices, NULL, vertex_count);
	if (!stream)
		return;
	level_frame(stream->origin);
	for (index = 0; index < triangle_count * 3; index++)
	{
		unsigned int vertex = indices[index];

		if (vertex < lowest) lowest = vertex;
		if (vertex > highest) highest = vertex;
	}
	if (highest >= (unsigned int)vertex_count)
		return;
	memset(vertex_codes + lowest, CODE_UNKNOWN, highest - lowest + 1);
	cut_store_begin(&pieces_store, cut_corners, MAXIMUM_CUT_CORNERS, CUT_OTHER);
	far_cut_begin();
	/* build 116: a surface that fades by the angle it is seen at (above) */
	if (PSP_SEETHROUGH_MODE(blend) != 0 && seethrough_fades(PSP_SEETHROUGH_BLEND(blend)) && !baked &&
		triangle_count <= LEVEL_ANGLE_TRIANGLES &&
		level_transparent_angle(stream, indices, triangle_count, key, u_scale, v_scale, u_offset, v_offset, blend,
			centroid))
		return;
	kept = (unsigned short *)list_take((int)(triangle_count * 3 * sizeof(unsigned short) + 3) & ~3);
	if (!kept)
		return;
	for (index = 0; index < triangle_count; index++)
	{
		const unsigned short *triangle = indices + 3 * index;
		int bits[3], corner, planes, split;

		if (baked && bake_stencil_has(baked, triangle))
			continue;
		if (baked)
			bake_old_path_triangles++;
		for (corner = 0; corner < 3; corner++)
		{
			int code = vertex_codes[triangle[corner]];

			if (code & CODE_UNKNOWN)
			{
				float point[3];

				level_to_camera(&stream->base[triangle[corner]].x, point);
				code = outside(point) | far_cut_code(point[2]);
				vertex_codes[triangle[corner]] = (unsigned char)code;
			}
			bits[corner] = code;
		}
		planes = (bits[0] | bits[1] | bits[2]) & CODE_PLANES;
		split = ((bits[0] | bits[1] | bits[2]) & CODE_FAR) == CODE_FAR;
		if (!planes && !split)
		{
			memcpy(kept + 3 * kept_count, triangle, 3 * sizeof(unsigned short));
			if (kept_count < OVERLAY_TRIANGLES)
			{
				float p[3][3];

				if (exact)
				{
					/* one draw, at the depth it has: no nudge to size, no wall to ask about */
					overlay_triangle_bucket[kept_count] = 0;
					bucket_units[0] = 1;
					kept_count++;
					continue;
				}
				for (corner = 0; corner < 3; corner++)
					level_to_camera(&stream->base[triangle[corner]].x, p[corner]);
				units = overlay_units(p[0], p[1], p[2], overlay_answer(stream, indices, triangle_count, index));
				overlay_triangle_bucket[kept_count] = overlay_bucket_of(units);
				/* build 115: overlayfreefar (the default from build 116), the free bucket at its smallest push */
				if (units < 0 && overlay_free_far > 0)
				{
					if (!bucket_units[0] || -units < bucket_units[0])
						bucket_units[0] = (unsigned short)-units;
				}
				else if (abs(units) > bucket_units[overlay_triangle_bucket[kept_count]])
					bucket_units[overlay_triangle_bucket[kept_count]] = (unsigned short)abs(units);
			}
			kept_count++;
		}
		else if (!(bits[0] & bits[1] & bits[2] & (CODE_PLANES | CODE_PAST)))
		{
			float corners[3][CORNER_FLOATS] = { { 0.0f } };

			for (corner = 0; corner < 3; corner++)
			{
				const struct ge_vertex *b = &stream->base[triangle[corner]];

				level_to_camera_corner(&b->x, corners[corner]);
				corners[corner][3] = b->u;
				corners[corner][4] = b->v;
				corners[corner][5] = corners[corner][6] = 0.0f;
			}
			units = exact ? 0 : overlay_units(corners[0], corners[1], corners[2],
				overlay_answer(stream, indices, triangle_count, index));
			if (units > cut_units)
				cut_units = units;
			if (units < 0 && (overlay_free_far > 0 ? !cut_units_free || -units < cut_units_free : -units > cut_units_free))
				cut_units_free = -units;
			cut_count += split ? clip_at_far(corners, planes, &pieces_store) :
				clip_triangle(corners, planes, &pieces_store);
		}
	}
	if (kept_count > OVERLAY_TRIANGLES)
	{
		/* more than the table holds: one draw at the largest nudge seen */
		int most = 0;

		for (bucket = 0; bucket < OVERLAY_BUCKETS; bucket++)
			if (bucket_units[bucket] > most)
				most = bucket_units[bucket];
		if ((draw = transparent_slot(key, u_scale, v_scale, u_offset, v_offset, blend, centroid)) != NULL)
		{
			level_matrix(&draw->matrix, stream->origin);
			draw->vertices = stream->base;
			draw->indices = kept;
			draw->index_count = (int)kept_count * 3;
			draw->level = 1;
			draw->depth_units = (short)most;
			level_transparent_draws++;
		}
	}
	else if (kept_count)
	{
		/* a draw per bucket of nudges, its triangles gathered together */
		for (bucket = 0; bucket < OVERLAY_BUCKETS; bucket++)
		{
			unsigned short *gathered;
			long t, count = 0;

			if (!bucket_units[bucket])
				continue;
			for (t = 0; t < kept_count; t++)
				count += overlay_triangle_bucket[t] == bucket;
			gathered = (unsigned short *)list_take((int)(count * 3 * sizeof(unsigned short) + 3) & ~3);
			if (!gathered || (draw = transparent_slot(key, u_scale, v_scale, u_offset, v_offset, blend, centroid)) == NULL)
				break;
			count = 0;
			for (t = 0; t < kept_count; t++)
				if (overlay_triangle_bucket[t] == bucket)
				{
					memcpy(gathered + 3 * count, kept + 3 * t, 3 * sizeof(unsigned short));
					count++;
				}
			level_matrix(&draw->matrix, stream->origin);
			draw->vertices = stream->base;
			draw->indices = gathered;
			draw->index_count = (int)count * 3;
			draw->level = 1;
			draw->depth_units = bucket ? (short)bucket_units[bucket] : (short)-bucket_units[bucket];
			level_transparent_draws++;
		}
	}
	if (cut_count && (draw = transparent_slot(key, u_scale, v_scale, u_offset, v_offset, blend, centroid)) != NULL)
	{
		struct ge_vertex *out = list_take(cut_count * sizeof(struct ge_vertex));

		if (!out)
		{
			/* build 89: the slot given back (review) */
			transparent_count--;
			return;
		}
		for (index = 0; index < cut_count; index++)
		{
			const float *c = cut_at(&pieces_store, index);
			int k;

			/* the world position carried through the cut (build 34) */
			for (k = 0; k < 3; k++)
				(&out[index].x)[k] = c[7 + k];
			out[index].u = c[3];
			out[index].v = c[4];
		}
		level_matrix(&draw->matrix, stream->origin);
		draw->vertices = out;
		draw->indices = NULL;
		draw->index_count = cut_count;
		draw->level = 2;
		/* on-a-wall triangles' largest nudge, or if all were free, pushed away */
		draw->depth_units = (short)(cut_units > 0 ? cut_units : -cut_units_free);
		level_transparent_draws++;
	}
}

void psp_ge_queue_level_transparent(const void *vertices, int vertex_count, const unsigned short *indices,
	long triangle_count, unsigned int key, float u_scale, float v_scale, float u_offset, float v_offset, short blend,
	const float *centroid)
{
	GE_PRETOUCH(key, TXPT_LEVEL_SEETHROUGH);
	level_pretouch(vertices, NULL, vertex_count, key);
	GE_DEFER(defer_queue_level_transparent(vertices, vertex_count, indices, triangle_count, key, u_scale, v_scale,
		u_offset, v_offset, blend, centroid)); /* build 112: gedefer */
	GE_BENCH_SKIP();
	ge_kind(KIND_LEVEL_SEETHROUGH); /* build 103 */
	unsigned int start = ge_clock();

	psp_ge_queue_level_transparent_body(vertices, vertex_count, indices, triangle_count, key, u_scale, v_scale, u_offset, v_offset, blend, centroid);
	seethrough_next_tint = 0xFFFFFFFF; /* build 116: this call's tint is used */
	seethrough_next_shader = 0;
	seethrough_next_doubled = 0;
	seethrough_next_angle = 0;
	seethrough_next_model = SEETHROUGH_MODULATE;
	seethrough_next_depth = 0;
	ge_entry_us[3] += ge_clock() - start;
}

#define LEVEL_OVERLAY_OFFSET 24
static int overlay_scaled = -1;
static int overlays_off = -1;
extern volatile int psp_screenshot_requested;
static int tdump = -1;
static long tskip = -1; /* "tskipN" (transparent_flush_body) */

/* whether draw a goes after draw b: far to near, Cortana's parts on a level
that asks for it after every other (psp_ge_model_effect) */
static int transparent_later(const struct transparent_draw *a, const struct transparent_draw *b)
{
	if (a->sorted_last != b->sorted_last)
		return a->sorted_last;
	return a->distance < b->distance;
}

/* the depth of one self-occluding object's run of parts (the draws from
order position index on that carry its number), both sides, nothing else
written, as the Xbox's first go (psp_ge_model_effect). The parts then draw
as usual, and only their nearest surface passes the depth test */
static void transparent_occluder_depth(int index)
{
	unsigned short occluder = transparent[transparent_order[index]].occluder;

	self_occlude_runs++;
	sceGuPixelMask(0xFFFFFFFF);
	sceGuDisable(GU_TEXTURE_2D);
	sceGuDepthMask(GU_FALSE);
	for (; index < transparent_count; index++)
	{
		struct transparent_draw const *draw = &transparent[transparent_order[index]];
		int middle = draw->colored ? GU_COLOR_8888 : GU_NORMAL_8BIT;

		if (draw->occluder != occluder)
			break;
		/* (a first-person part keeps its own projection: left to draw as before) */
		if ((!draw->vertices && !draw->cut_vertices) || draw->sprites || draw->level || draw->sky ||
			draw->first_person || list_full())
			continue;
		sceGumLoadMatrix(&draw->matrix);
		if (draw->index_count > 0)
			sceGumDrawArray(draw->strip ? GU_TRIANGLE_STRIP : GU_TRIANGLES,
				GU_TEXTURE_32BITF | middle | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
				draw->index_count, draw->indices, draw->vertices);
		if (draw->cut_count > 0 && draw->cut_vertices)
			sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | middle | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
				draw->cut_count, NULL, draw->cut_vertices);
		self_occlude_parts++;
	}
	sceGuDepthMask(GU_TRUE);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuPixelMask(0);
}

static void transparent_flush_body(void)
{
	int saved_limit = list_limit;
	unsigned short occluder_drawn = 0;

	int index, other;

	if (!transparent_count)
		return;
	if (blip_depth_live)
	{
		blip_depth_world_after++;
		blip_depth_after_kind[KIND_SEETHROUGH_FLUSH]++;
	}
	if (list_limit < list_bytes - HUD_RESERVE)
		list_limit = list_bytes - HUD_RESERVE;
	if (overlays_off < 0)
		overlays_off = psp_keep("nooverlays");
	if (level_cull < 0)
		level_cull = psp_keep("nocull") ? 0 : psp_keep("cullflip") ? 2 : 1;
	/* build 62: the world's projection and depth slice for the world's
	see-through parts, whatever is current. A flush can come while the
	first-person projection is still set (no model end after the weapon's
	last part); the level's painted symbols and the cave's fungus were then
	drawn in the weapon's depth slice, in front of everything (b61:
	only with the pistol out, gone when zoomed, when no weapon is drawn) */
	if (model_first_person)
	{
		use_projection(0);
		first_person_flushes++;
	}
	/* far to near (the sky's parts count as farthest) */
	for (index = 0; index < transparent_count; index++)
	{
		for (other = index; other > 0 && transparent_later(&transparent[transparent_order[other - 1]], &transparent[index]); other--)
			transparent_order[other] = transparent_order[other - 1];
		transparent_order[other] = index;
	}
	sceGuDepthMask(GU_TRUE);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuTexWrap(GU_REPEAT, GU_REPEAT);
	sceGuColor(0xFFFFFFFF);
	sceGumMatrixMode(GU_MODEL);
	for (index = 0; index < transparent_count; index++)
	{
		struct transparent_draw const *draw = &transparent[transparent_order[index]];

		/* the frame's end is one record: its see-through draws can take
		milliseconds, which audio and the ring mustn't wait out (geinflight) */
		if (replay_inflight)
			inflight_serve();

		/* build 89: a slot whose filling never finished (see transparent_slot) */
		if (!draw->vertices && !draw->cut_vertices)
			continue;
		/* build 63: "tdump": each see-through draw of the frame a screenshot
		is asked for, with the state it goes out under */
		if (tdump < 0)
			tdump = psp_keep("tdump");
		if (tdump && psp_screenshot_requested)
		{
			extern char const *psp_bitmap_tag_name(void const *bitmap); /* hud_draw.c (psp_fixups.py) */
			unsigned int state_depth = sceGuGetStatus(GU_DEPTH_TEST);
			/* and the map's bitmap tag (after b116: a10's bridge window); a
			walk over the tags, so not on the Media Engine (gemeon) */
			char const *name = PSP_ME_WORK() ? NULL : psp_bitmap_tag_name((void const *)draw->key);
			extern char const *psp_shader_tag_name(void const *shader); /* hud_draw.c (psp_fixups.py) */
			char const *shader_name = PSP_ME_WORK() || !draw->shader ? NULL : psp_shader_tag_name((void const *)draw->shader);

			if (shader_name)
			{
				const char *definition = (const char *)draw->shader;
				short type = *(const short *)(definition + 36);

				platform_log("tdump: %d/%d shader %s (type %d, depth mode %d)", index, transparent_count, shader_name,
					type, draw->depth_mode);
				if (type == 5 || type == 6)
				{
					/* every map of a generic or chicago shader: the GE draws one */
					extern unsigned int psp_tex_bitmap_key(long bitmap_tag_index, int bitmap);
					int chicago = type == 6, stride = chicago ? 220 : 100, scale_at = chicago ? 84 : 4;
					int map_at = chicago ? 120 : 40, m;
					long count = *(const long *)(definition + 84);
					const char *maps = *(const char *const *)(definition + 88);

					for (m = 0; maps && m < count && m < 4; m++)
					{
						const char *map = maps + stride * m;
						char const *map_name = psp_bitmap_tag_name((void const *)psp_tex_bitmap_key(*(const long *)(map + map_at), 0));

						platform_log("tdump: %d/%d   map %d %s flags %04x scale %.2f %.2f%s", index, transparent_count, m,
							map_name ? map_name : "?", *(const unsigned short *)map, *(const float *)(map + scale_at),
							*(const float *)(map + scale_at + 4), (unsigned int)draw->key == psp_tex_bitmap_key(*(const long *)(map + map_at), 0) ? " (drawn)" : "");
					}
				}
			}
			platform_log("tdump: %d/%d key %08x (%s) sky %d level %d fp %d sprites %d blend %d dist %.1f count %d cut %d units %d at %.2f %.2f %.2f depth test %u mfp %d",
				index, transparent_count, draw->key, name ? name : "no tag", draw->sky, draw->level, draw->first_person,
				draw->sprites, draw->blend, sqrtf(draw->distance), draw->index_count, draw->cut_count, draw->depth_units,
				draw->matrix.w.x, draw->matrix.w.y, draw->matrix.w.z, state_depth, model_first_person);
			/* the bake's tint as the draw carries it (the ocean's colour) */
			platform_log("tdump: %d/%d tint %08x model %d doubled %d angle %d face %08x edge %08x colour %08x coloured %d",
				index, transparent_count, draw->tint, draw->tint_model, draw->doubled, draw->angle_tinted,
				draw->tint_face, draw->tint_edge, draw->color, draw->colored);
		}
		/* "tskipN": the see-through draws whose map is the bitmap_data at
		address N (decimal; tdump prints it in hex) left out, to tell which
		draw a thing on screen is (after b116: c40's pale discs). The tags
		sit at the same addresses in every build, for the same map */
		if (tskip < 0)
		{
			extern int psp_keep_number(const char *prefix, int fallback);

			tskip = (long)psp_keep_number("tskip", 0);
		}
		if (tskip > 0 && draw->key == (unsigned int)tskip)
			continue;
		/* a self-occluding object's run of parts: its depth first */
		if (draw->occluder && draw->occluder != occluder_drawn)
			transparent_occluder_depth(index);
		occluder_drawn = draw->occluder;
		if ((draw->first_person ? list_full_first_person() : list_full()) || (draw->level && overlays_off) ||
			!psp_tex_bind(draw->key, NULL))
			continue;
		if (draw->sky == 2)
		{
			if (!draw->depth_test)
				sceGuDisable(GU_DEPTH_TEST);
			sceGuDisable(GU_FOG);
		}
		else if (draw->sky)
		{
			sceGuDepthRange(1, 0);
			sceGuDisable(GU_FOG);
		}
		else if (draw->blend == 0)
			sceGuEnable(GU_FOG);
		else if (draw->blend == 3 && (draw->depth_mode & SEETHROUGH_FOG_FADE))
		{
			sceGuFog(fog_near, fog_far, 0);
			sceGuEnable(GU_FOG);
			seethrough_fog_faded_draws++;
		}
		else
			/* added or multiplied light fades by its own: no fog colour on top */
			sceGuDisable(GU_FOG);
		if (draw->clut)
		{
			/* build 116: a meter, with its own palette (meter_palette) in
			place of the map's, its colours drawn as they are */
			unsigned int tint = draw->tint, rgb = ((tint & 0xFF) << 16) | (tint & 0xFF00) | ((tint >> 16) & 0xFF);

			sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
			sceGuClutLoad(draw->clut_blocks, draw->clut);
			sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
			ge_blend(1);
			if (draw->blend == BLEND_METER_TINT)
				sceGuBlendFunc(GU_ADD, GU_FIX, GU_SRC_ALPHA, rgb, 0);
			else
			{
				unsigned int brightness = tint >> 24;

				sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, (brightness << 16) | (brightness << 8) | brightness, rgb);
				sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
				sceGuEnable(GU_ALPHA_TEST);
			}
		}
		else
		{
			hud_blend(draw->blend);
			/* glass drawn by its reflection: an added, angle-tinted part */
			if (draw->blend == 3 && draw->angle_tinted && draw->shader)
				glass_tint_behind(draw->shader);
		}
		if (draw->sky)
			ge_layer_dither();
		if (draw->first_person)
			use_projection(1);
		/* build 35: the level's see-through surfaces (the base pillars'
		painted symbols) lie on solid ones; the Xbox's 24-bit depth sorts
		that out, the PSP's 16 bits flickered between them by the view.
		Nudged nearer, as decals are */
		if (draw->level >= 3)
		{
			/* build 87: a baked stencil on its wall's own pieces: the same
			depth exactly (mode 1: two steps of slack, in case; and always
			with levelcompact, whose walls go through the GE as 16-bit
			vertices and another matrix, the stencil's as floats) */
			if (stencil_mode == 1 || level_compact > 0)
				sceGuDepthOffset(STENCIL_SLACK);
			else
				sceGuDepthFunc(GU_EQUAL);
		}
		else if (draw->level)
		{
			/* build 54: the nudge sized by distance, as build 52 did for
			decals: a fixed 24 depth steps is metres far off (the base's
			painted symbols showed through whatever stood in front of them,
			b50-b52) and almost nothing near. Now about half a centimetre
			at the overlay's distance (draw->distance is its square), never
			under 3 steps; "overlayoffset": the fixed 24 */
			int units = LEVEL_OVERLAY_OFFSET;

			if (overlay_scaled < 0)
				overlay_scaled = !psp_keep("overlayoffset");
			if (overlay_scaled && draw->depth_units)
				units = draw->depth_units;
			else if (overlay_scaled && draw->distance > 0.0f)
			{
				float scaled = 0.005f * 61439.0f * projection_near / draw->distance;

				units = scaled < 3.0f ? 3 : scaled > 4096.0f ? 4096 : (int)scaled;
			}
			if (draw->depth_mode & SEETHROUGH_EXACT_DEPTH)
			{
				units = 0;
				seethrough_exact_draws++;
			}
			sceGuDepthOffset(units);
			/* the level's own triangles and their cut pieces keep the base
			pass's winding; a bake's stencil layers (level 3) don't */
			if ((draw->depth_mode & SEETHROUGH_ONE_SIDED) && draw->level <= 2 && level_cull)
			{
				sceGuFrontFace(level_cull == 1 ? LEVEL_FRONT : (LEVEL_FRONT == GU_CW ? GU_CCW : GU_CW));
				sceGuEnable(GU_CULL_FACE);
				level_culled_draws++;
			}
		}
		sceGuTexScale(draw->u_scale, draw->v_scale);
		sceGuTexOffset(draw->u_offset, draw->v_offset);
		sceGumLoadMatrix(&draw->matrix);
		if (draw->one_sided)
		{
			/* the model's own winding, as its solid parts are culled */
			sceGuFrontFace(LEVEL_FRONT);
			sceGuEnable(GU_CULL_FACE);
			seethrough_culled_draws++;
		}
		if (draw->depth_mode & SEETHROUGH_CLAMPED)
		{
			sceGuTexWrap(draw->depth_mode & SEETHROUGH_CLAMP_U ? GU_CLAMP : GU_REPEAT,
				draw->depth_mode & SEETHROUGH_CLAMP_V ? GU_CLAMP : GU_REPEAT);
			seethrough_clamped_draws++;
		}
		if (draw->doubled)
		{
			/* build 116: a shader brighter than its map, its tint halved */
			sceGuEnable(GU_FRAGMENT_2X);
			seethrough_doubled_draws++;
		}
		flat_color = 0;
		if (draw->tint_model == SEETHROUGH_ADD)
		{
			sceGuTexFunc(GU_TFX_ADD, GU_TCC_RGBA);
			seethrough_added_draws++;
		}
		else if (draw->tint_model == SEETHROUGH_FLAT)
		{
			/* the vertex colour where the texel is dark, the environment
			colour where it is bright: the same colour, so the texel only
			gives its alpha. A draw coloured per vertex takes the tint */
			unsigned int flat = draw->sprites || draw->colored ? draw->tint : draw->color;

			if (flat_round < 0)
				flat_round = !psp_keep("flatnoround");
			/* only where the frame takes the colour whole (an added blend)
			and undithered; a coloured draw's colours are its corners' */
			if (flat_round && !draw->sprites && !draw->colored && !draw->doubled && !draw->sky &&
				(draw->blend == 3 || draw->blend == 7) && dither_all <= 0 && flat != 0xFFFFFFFF)
			{
				flat = flat_rounded(flat);
				flat_color = flat;
				seethrough_rounded_draws++;
			}
			sceGuTexFunc(GU_TFX_BLEND, GU_TCC_RGBA);
			sceGuTexEnvColor(flat & 0x00FFFFFF);
			seethrough_flat_draws++;
		}
		/* build 116: the shader's fade as one colour (sprites and coloured
		draws carry their own) */
		if (draw->color != 0xFFFFFFFF && !draw->sprites && !draw->colored)
			sceGuColor(flat_color ? flat_color : draw->color);
		if (draw->sprites)
			/* (build 113: a contrail is a strip; sprites and widgets lists) */
			sceGumDrawArray(draw->strip ? GU_TRIANGLE_STRIP : GU_TRIANGLES,
				GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D, draw->index_count, NULL,
				draw->vertices);
		else if (draw->level == 1 || draw->level == 3)
			sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
				draw->index_count, draw->indices, draw->vertices);
		else if (draw->level == 2 || draw->level == 4)
			/* (build 116: a level surface faded by angle carries a colour a corner) */
			sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | (draw->colored ? GU_COLOR_8888 : 0) | GU_VERTEX_32BITF |
				GU_TRANSFORM_3D, draw->index_count, NULL, draw->vertices);
		else
		{
			/* build 116: a fade per vertex replaces the normal with a colour */
			int middle = draw->colored ? GU_COLOR_8888 : GU_NORMAL_8BIT;

			if (draw->index_count > 0)
				sceGumDrawArray(draw->strip ? GU_TRIANGLE_STRIP : GU_TRIANGLES,
					GU_TEXTURE_32BITF | middle | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
					draw->index_count, draw->indices, draw->vertices);
			if (draw->cut_count > 0 && draw->cut_vertices)
				sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | middle | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
					draw->cut_count, NULL, draw->cut_vertices);
		}
		if (draw->color != 0xFFFFFFFF && !draw->sprites && !draw->colored)
			sceGuColor(0xFFFFFFFF);
		if (draw->one_sided)
			sceGuDisable(GU_CULL_FACE);
		if (draw->clut)
		{
			sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
			sceGuDisable(GU_ALPHA_TEST);
		}
		if (draw->doubled)
			sceGuDisable(GU_FRAGMENT_2X);
		if (draw->depth_mode & SEETHROUGH_CLAMPED)
			sceGuTexWrap(GU_REPEAT, GU_REPEAT);
		if (draw->blend == 3 && (draw->depth_mode & SEETHROUGH_FOG_FADE) && !draw->sky)
			sceGuFog(fog_near, fog_far, fog_color);
		if (draw->tint_model != SEETHROUGH_MODULATE)
			sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
		if (draw->tint_model == SEETHROUGH_FLAT)
			sceGuTexEnvColor(0xFFFFFFFF);
		if (draw->level)
		{
			sceGuDepthOffset(0);
			sceGuDisable(GU_CULL_FACE);
		}
		if (draw->level >= 3)
			sceGuDepthFunc(GU_GEQUAL);
		if (draw->first_person)
			use_projection(0);
		if (draw->sky == 2)
			sceGuEnable(GU_DEPTH_TEST);
		else if (draw->sky)
			sceGuDepthRange(WORLD_DEPTH_NEAR, 0);
		transparent_drawn++;
	}
	world_model();
	if (model_first_person)
		use_projection(1);
	sceGuTexScale(1.0f, 1.0f);
	sceGuTexOffset(0.0f, 0.0f);
	ge_blend(0);
	sceGuDisable(GU_TEXTURE_2D);
	sceGuEnable(GU_FOG);
	sceGuDepthMask(GU_FALSE);
	transparent_count = 0;
	list_limit = saved_limit;
}

static void transparent_flush(void)
{
	unsigned int start = ge_clock();
	int kind = ge_kind_now;

	ge_kind(KIND_SEETHROUGH_FLUSH); /* build 103 */
	transparent_flush_body();
	ge_kind(kind);
	ge_entry_us[8] += ge_clock() - start;
}

/* ---------- the HUD (build 20)

The game draws its HUD as screen quads: four corners in its own screen's
pixels (848 x 480 here), texture coordinates from 0 to 1, a colour each,
and one bitmap with a tint, a fade and a way to blend with the frame. They
are drawn in the GE's 2D mode, scaled to 480 x 272, after the 3D world.

A meter (the shield bar, ammo counts) is a bitmap whose alpha orders its
pixels from empty to full: the game passes the fill as an alpha threshold,
and pixels below it take the fill colour, the rest the empty colour; the GE
does this with its alpha test, in two draws. */

struct hud_vertex
{
	float u, v;
	unsigned int color;
	float x, y, z;
};
#define PSP_GLYPH_PIXELS
#include "psp_glyphs.h"

/* the sheet lives in the texture RAM block's first 32 KB, outside the
texture cache (psp_tex.c, GLYPH_ROOM_BYTES), so neither each build's heap nor
VRAM's rooms pay for it; the program holds it run-length coded (14 KB) and
unpacks it there, again after each sleep (the block goes back to the system) */
void *psp_tex_glyph_room(unsigned int *generation);
void *psp_tex_glyph_room_held(unsigned int *generation);
/* written by the main CPU only (glyph_sheet): the Media Engine's replay
only reads them */
static PSP_MAIN_SIDE unsigned int glyph_generation = 0;
static PSP_MAIN_SIDE int glyph_me_old = -1;

static void __attribute__((noinline, cold)) glyph_sheet_unpack(unsigned char *room)
{
	unsigned char *to = room;
	unsigned char *const end = to + PSP_GLYPH_SHEET_WIDTH * PSP_GLYPH_SHEET_HEIGHT;
	const unsigned char *from = psp_glyph_packed, *const last = psp_glyph_packed + sizeof(psp_glyph_packed);

	/* tools/psp_glyphs.py, packbits(): a control byte under 128 is followed
	by control + 1 bytes as they are; from 128, by one byte repeated
	control - 125 times */
	while (from < last && to < end)
	{
		unsigned int control = *from++, count;

		if (control < 128)
			for (count = control + 1; count && from < last && to < end; count--)
				*to++ = *from++;
		else if (from < last)
		{
			unsigned char value = *from++;

			for (count = control - 125; count && to < end; count--)
				*to++ = value;
		}
	}
	if (to != end || from != last)
		platform_log("GE: the button glyph sheet unpacked wrong (%d of %d bytes, %d of %d read): its prompts will look broken",
			(int)(to - (end - PSP_GLYPH_SHEET_WIDTH * PSP_GLYPH_SHEET_HEIGHT)), PSP_GLYPH_SHEET_WIDTH * PSP_GLYPH_SHEET_HEIGHT,
			(int)(from - psp_glyph_packed), (int)sizeof(psp_glyph_packed));
	/* the GE reads main RAM, not the CPU's cache */
	sceKernelDcacheWritebackRange(room, PSP_GLYPH_SHEET_WIDTH * PSP_GLYPH_SHEET_HEIGHT);
}

/* the sheet, unpacked if the RAM block is new; NULL without one.
The Media Engine's work (the replay under gemeon) never unpacks it:
unpacking hands the sheet to the GE with a system call, and a missing block
is asked for with more, which the ME can't make. The recording unpacks it
first, on the main CPU (glyph_sheet_ready); a replay that finds the block
changed since (a sleep between) leaves the quad out for that frame.
"glyphmeold": unpacked wherever it is first drawn */
static const void *glyph_sheet(void)
{
	unsigned int generation;
	unsigned char *room;

	if (PSP_ME_WORK() && glyph_me_old <= 0)
	{
		room = psp_tex_glyph_room_held(&generation);
		return room && generation == glyph_generation ? room : NULL;
	}
	room = psp_tex_glyph_room(&generation);
	if (room && generation != glyph_generation)
	{
		glyph_sheet_unpack(room);
		glyph_generation = generation;
	}
	return room;
}

/* the main CPU's unpack, as a button glyph is recorded: the replay then
finds the sheet there */
static void glyph_sheet_ready(void)
{
	if (glyph_me_old < 0)
		glyph_me_old = psp_keep("glyphmeold") != 0;
	if (!glyph_me_old && !PSP_ME_WORK())
		(void)glyph_sheet();
}

/* the game's dynamic_screen_vertex: position, texture coordinates, colour (ARGB) */
struct game_screen_vertex
{
	float x, y, u, v;
	unsigned int argb;
};

/* the game's rasterizer_meter_parameters (colours ARGB): build 116 keeps
the layout in psp_seethrough.h (struct psp_seethrough_meter), shared with the
models' meter shaders; its seven words are what the defer queue copies */
typedef char meter_words_check[sizeof(struct psp_seethrough_meter) == METER_WORDS * 4 ? 1 : -1];

#define GAME_SCREEN_WIDTH 848.0f
#define GAME_SCREEN_HEIGHT 480.0f

/* where the game's screen coordinates land (build 26): its 848 x 480 in play
fills the PSP's screen; the menus' 640 x 480 (4:3) is pillarboxed, as the
3D view behind them isn't */
static float ui_scale_x = SCREEN_WIDTH / GAME_SCREEN_WIDTH, ui_scale_y = SCREEN_HEIGHT / GAME_SCREEN_HEIGHT;
static float ui_offset_x = 0.0f, ui_offset_y = 0.0f;

/* The menus drawn larger: "menuzoomN", N percent (110, 115), about the
screen's centre. On the PSP's 4.3 inches the menus' text, at the TV's share
of the screen, reads small. One zoom for the whole 640 x 480 page keeps every
box and line where it was against the others, so nothing comes to overlap;
115 is the most that keeps the Xbox's title-safe area (32 of 480 from each
edge, where its menus lay out) on the screen: 240 / 208. 115 by default;
"menuzoom100" draws the menus at the TV's size again */
#define MENU_ZOOM_MOST 115
#define MENU_ZOOM_DEFAULT 115
static float menu_zoom = 0.0f;

/* the menus' button glyphs drawn larger, "menuglyphN", N percent (125, 150):
a screen key's glyph (its label to its right) grows up and to the left,
into the gap before the key ahead of it; a glyph in a line of text grows up
and to the right, the line's cursor moving on by as much (psp_sdl.c,
psp_button_glyph_extra), and no taller than its line's icon box. 125 by
default; "menuglyph100" draws them at the original size */
#define MENU_GLYPH_MOST 150
#define MENU_GLYPH_DEFAULT 125
static float menu_glyph_zoom = 0.0f;

static float keep_percent(const char *prefix, int fallback, int most)
{
	int percent = psp_keep_number(prefix, fallback);

	return (percent < 100 ? 100 : percent > most ? most : percent) / 100.0f;
}

/* how much larger a glyph in a menu's text draws (1: not in the menus, or
"menuglyph100"); psp_sdl.c widens the text's cursor by the same */
static float menu_glyph_zoom_get(void)
{
	if (menu_glyph_zoom == 0.0f)
		menu_glyph_zoom = keep_percent("menuglyph", MENU_GLYPH_DEFAULT, MENU_GLYPH_MOST);
	return menu_glyph_zoom;
}

float psp_ge_menu_glyph_zoom(int glyph, int menus)
{
	if (!menus || glyph < 0 || glyph >= PSP_GLYPH_COUNT)
		return 1.0f;
	return fminf(menu_glyph_zoom_get(), 100.0f / psp_glyph_height[glyph]);
}
/* build 36: the HUD laid out in a smaller window (hud.c, psp_fixups.py) is
drawn this much larger */
static float hud_zoom = 1.0f;

void psp_ge_hud_zoom(float zoom)
{
	GE_DEFER(defer_words(DEFER_HUD_ZOOM, 1, defer_bits(zoom), 0, 0, 0, 0, 0, 0, 0)); /* build 112: gedefer */
	hud_zoom = zoom > 0.0f ? zoom : 1.0f;
}

static int ui_centered = 0;

void psp_ge_ui_centered(int centered)
{
	GE_DEFER(defer_words(DEFER_UI_CENTERED, 1, (unsigned int)centered, 0, 0, 0, 0, 0, 0, 0)); /* build 112: gedefer */
	ui_centered = centered;
}

void psp_ge_ui_space(int width, int height)
{
	GE_DEFER(defer_words(DEFER_UI_SPACE, 2, (unsigned int)width, (unsigned int)height, 0, 0, 0, 0, 0, 0)); /* build 112: gedefer */
	if (width <= 0 || height <= 0)
		return;
	if (ui_centered)
		width = 640;
	if (menu_zoom == 0.0f)
		menu_zoom = keep_percent("menuzoom", MENU_ZOOM_DEFAULT, MENU_ZOOM_MOST);
	ui_scale_y = (float)SCREEN_HEIGHT / height;
	ui_offset_y = 0.0f;
	if (width * 3 <= height * 4 + 4)
	{
		/* 4:3 or narrower: square pixels, centred */
		if (ui_centered)
		{
			ui_scale_y *= menu_zoom;
			ui_offset_y = (SCREEN_HEIGHT - height * ui_scale_y) * 0.5f;
		}
		ui_scale_x = ui_scale_y;
		ui_offset_x = (SCREEN_WIDTH - width * ui_scale_x) * 0.5f;
	}
	else
	{
		ui_scale_x = (float)SCREEN_WIDTH / width;
		ui_offset_x = 0.0f;
	}
}
int psp_tex_size(unsigned int key, int *width, int *height);
int psp_tex_palette(unsigned int key, const unsigned int **palette, int *count);
static unsigned long hud_quads = 0;

static unsigned int argb_to_abgr(unsigned int argb, float red, float green, float blue, float alpha)
{
	unsigned int a = (unsigned int)(((argb >> 24) & 0xFF) * alpha), r = (unsigned int)(((argb >> 16) & 0xFF) * red);
	unsigned int g = (unsigned int)(((argb >> 8) & 0xFF) * green), b = (unsigned int)((argb & 0xFF) * blue);

	if (a > 255) a = 255;
	if (r > 255) r = 255;
	if (g > 255) g = 255;
	if (b > 255) b = 255;
	return (a << 24) | (b << 16) | (g << 8) | r;
}

static void hud_blend(short blend)
{
	ge_blend(1);
	switch (blend)
	{
	case 1: sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_FIX, 0, 0); break;
	case 2: sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_SRC_COLOR, 0, 0); break;
	case 3: sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF); break;
	case 4: sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF); break;
	case 5: sceGuBlendFunc(GU_MIN, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF); break;
	case 6: sceGuBlendFunc(GU_MAX, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF); break;
	/* alpha-multiply-add: the colour is added and the alpha only darkens
	what is behind (the crosshairs and most of the HUD have no alpha, so
	they are purely additive) */
	case 7: sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_ALPHA, 0xFFFFFF, 0); break;
	default: sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0); break;
	}
}

/* how the quad's texture coordinates become the pack texture's texels: the
game's coordinates times its texture scale are 0 to 1 across the bitmap, or,
for its linear (interface) bitmaps, in the bitmap's own texels (build 26) */
static float hud_u_scale = 1.0f, hud_v_scale = 1.0f;

/* "hudsnap" puts each HUD quad's corners on whole pixels, so the HUD
stops looking soft. Placed at the game's coordinates times the screen
scale, an element's edge lands part-way across a pixel, and the bilinear
filter spreads it over two. On whole pixels a texel drawn at its own size
(hud177) is exactly one pixel, as the screen copy relies on (shift 0, above);
at other zooms the edges at least stop smearing, and every element is as
sharp as every other. A slanted quad's corners move by at most half a pixel.
On by default since its A/B; "hudnosnap" turns it off */
static int hud_snap = -1;

static float hud_pixel(float position)
{
	return hud_snap ? floorf(position + 0.5f) : position;
}

static void hud_draw(struct game_screen_vertex const *in, int width, int height, float offset_x, float offset_y,
	int use_vertex_color, unsigned int color, float red, float green, float blue, float fade)
{
	ge_kind(KIND_HUD); /* build 103 */
	struct hud_vertex *out = list_take(4 * sizeof(struct hud_vertex));
	int index;

	if (!out)
		return;
	if (hud_snap < 0)
		hud_snap = !psp_keep("hudnosnap");
	for (index = 0; index < 4; index++)
	{
		out[index].u = in[index].u * hud_u_scale * width;
		out[index].v = in[index].v * hud_v_scale * height;
		out[index].color = argb_to_abgr(use_vertex_color ? in[index].argb : color, red, green, blue, fade);
		out[index].x = hud_pixel((in[index].x + offset_x) * ui_scale_x * hud_zoom + ui_offset_x);
		out[index].y = hud_pixel((in[index].y + offset_y) * ui_scale_y * hud_zoom + ui_offset_y);
		out[index].z = 0.0f;
	}
	sceGuDrawArray(GU_TRIANGLE_FAN, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 4, NULL, out);
}

/* worked-out meter palettes, kept while their meter doesn't change (build 31);
a slot's frame is the frame (counted from 1) that last used it */
/* build 116: ten slots (six before), as the models' meter shaders share
them: a plasma rifle's gauge, the Warthogs' meters in view (4 KB more) */
#define METER_CACHE 10
static unsigned int meter_cache_palette[METER_CACHE][256] __attribute__((aligned(64)));
static struct
{
	unsigned int key;
	struct psp_seethrough_meter meter;
	unsigned long frame;
	int count;
	unsigned int source; /* a hash of the bitmap's palette it was worked out from */
} meter_cache[METER_CACHE];
static unsigned long meter_reused = 0, meter_worked = 0;

static unsigned long hud_quad_us = 0, text_us = 0;
static void screen_quad(void const *vertices, unsigned int key, short blend, float red, float green, float blue,
	float fade, float offset_x, float offset_y, void const *meter_parameters, int wrapped, float u_scale, float v_scale);

/* "hdump": each HUD quad of a frame a screenshot is asked for, with its
bitmap's tag, its corners in the game's 640x480 screen and its colour, before
the defer queue (the tag name is looked up on this thread). For a HUD element
drawn where the Xbox doesn't draw it (a50's white bars, b116) */
static void hud_quad_dump(void const *vertices, unsigned int key, short blend, float red, float green, float blue,
	float fade, void const *meter_parameters, int wrapped)
{
	extern char const *psp_bitmap_tag_name(void const *bitmap); /* hud_draw.c (psp_fixups.py) */
	static PSP_MAIN_SIDE int hdump = -1; /* build 118: set as the calls are written down */
	struct game_screen_vertex const *in = vertices;
	const unsigned char *bitmap = (const unsigned char *)key;
	float x0, y0, x1, y1;
	char const *name;
	int corner;

	if (hdump < 0)
		hdump = psp_keep("hdump");
	/* not again when the defer queue replays the quad (gedefer), which may
	run on the Media Engine (gemeon) */
	if (!hdump || defer_replaying || psp_on_me() || !psp_screenshot_requested || !vertices || !key)
		return;
	x0 = x1 = in[0].x;
	y0 = y1 = in[0].y;
	for (corner = 1; corner < 4; corner++)
	{
		x0 = fminf(x0, in[corner].x);
		x1 = fmaxf(x1, in[corner].x);
		y0 = fminf(y0, in[corner].y);
		y1 = fmaxf(y1, in[corner].y);
	}
	name = psp_bitmap_tag_name(bitmap);
	platform_log("hdump: %s (bitmap %dx%d) at %.0f,%.0f to %.0f,%.0f, uv %.3f,%.3f to %.3f,%.3f; colour %.2f %.2f %.2f "
		"fade %.2f argb %08x; blend %d%s%s",
		name ? name : "(no tag)", *(const short *)(bitmap + 4), *(const short *)(bitmap + 6), x0, y0, x1, y1,
		in[0].u, in[0].v, in[2].u, in[2].v, red, green, blue, fade, in[0].argb, blend, meter_parameters ? ", meter" : "",
		wrapped ? ", wrapped" : "");
	if (meter_parameters)
	{
		/* the meter's words as hud_draw_meter set them: the fill rides in the
		top bytes of the gradient's minimum and the flash colours */
		unsigned long const *words = meter_parameters;

		platform_log("hdump: meter words %08lx %08lx %08lx %08lx %08lx %08lx %08lx", words[0], words[1], words[2],
			words[3], words[4], words[5], words[6]);
	}
}

/* the HUD's quads, timed for the log (build 31) */
/* the PSP's button glyphs in place of the Xbox's button icons
(docs/game_feature_survey.md, section 4.7). The game draws an icon as a HUD
quad of the icon's bitmap; hud_messaging.c and ui_widget.c (psp_fixups.py
FIXES_CONTROLS) mark the glyph just before, and the quad they draw then
takes the glyph's key: a number no bitmap's address can be, which carries
through the defer queue unchanged. The quad keeps the game's place, size and
colour for the icon; only the picture is the PSP's */
#define GLYPH_KEY_BASE 0x100u
/* in the key, beside the glyph: its label is to its right (a menu's screen
key), so a larger glyph grows away from it */
#define GLYPH_LABELLED 0x80u
static PSP_MAIN_SIDE int glyph_next = -1;
static unsigned long glyphs_drawn = 0;

void psp_ge_glyph_next(int glyph)
{
	glyph_next = glyph >= 0 && glyph < PSP_GLYPH_COUNT ? glyph : -1;
}

void psp_ge_glyph_next_labelled(int glyph)
{
	glyph_next = glyph >= 0 && glyph < PSP_GLYPH_COUNT ? glyph | (int)GLYPH_LABELLED : -1;
}

void psp_ge_screen_quad(void const *vertices, unsigned int key, short blend, float red, float green, float blue,
	float fade, float offset_x, float offset_y, void const *meter_parameters, int wrapped, float u_scale, float v_scale)
{
	hud_quad_dump(vertices, key, blend, red, green, blue, fade, meter_parameters, wrapped);
	/* the mark is the game thread's: not read again when the queue replays */
	if (glyph_next >= 0 && !defer_replaying && !psp_on_me())
	{
		key = GLYPH_KEY_BASE + (unsigned int)glyph_next;
		glyph_next = -1;
		glyph_sheet_ready();
	}
	GE_PRETOUCH_HUD(key, TXPT_SCREEN);
	/* "meterload": the recording loads a HUD meter's map before the quad is
	queued, outside pretouch's guards. Off by default: on the PSP it left the
	bars as empty as before and lost model palettes (Chief's change colours),
	so the bars' fault lies upstream of the map. Kept for the bars' A/B */
	if (meter_parameters && !defer_replaying && !psp_on_me())
	{
		static int meter_load = -1;

		if (meter_load < 0)
			meter_load = psp_keep("meterload") != 0;
		if (meter_load)
			(void)psp_tex_resident(key);
	}
	GE_DEFER(defer_screen_quad(vertices, key, blend, red, green, blue, fade, offset_x, offset_y, meter_parameters,
		wrapped, u_scale, v_scale)); /* build 112: gedefer */
	GE_BENCH_SKIP();
	ge_kind(KIND_SCREEN); /* build 103 */
	unsigned int start = ge_clock();

	screen_quad(vertices, key, blend, red, green, blue, fade, offset_x, offset_y, meter_parameters, wrapped, u_scale,
		v_scale);
	hud_quad_us += ge_clock() - start;
}

static unsigned long screen_quad_bailed = 0;

/* build 113: a zoomed weapon's mask (b113_candidates.md #20; keep word
"zoommask"). The HUD's screen effect hands its full-screen bitmap to
rasterizer_screen_effect (rasterizer.c, psp_fixups.py). What the Xbox does
with it (rasterizer_xbox_screen_effect.c), by the pixel shaders [D]:
  - with no blur, its last pass darkens the frame by the mask's alpha:
    frame x (1 - alpha);
  - with a blur (the pistol's and the sniper rifle's zoom: "warp"), the mask
    says where the blur goes: the last combiner mixes the sharp frame and
    three offset copies by the mask's alpha, so the middle stays sharp and
    the edges blur.
The PSP has no cheap blur, so with one this darkens by a third of the
mask's alpha instead, a stand-in for the soft edge (strength 85 of 255);
without one, the whole alpha, as the Xbox. One quad over the 3D view, before
the HUD. The mask is drawn for 4:3 (640x480): its square is kept round in
the middle of the 480x272 screen, and the sides take its edge texels
(clamped). The emulator's first run darkened the pistol's
zoom by the whole alpha: a clear circle about half the screen's height in a
black surround, which the Xbox's zoom doesn't show [U: as the game is
remembered; the shaders say the mask weighs the blur] */
static unsigned long zoom_masks = 0, zoom_masks_missing = 0;

void psp_ge_zoom_mask(unsigned int key, unsigned int strength)
{
	GE_PRETOUCH(key, TXPT_ZOOM);
	GE_DEFER(defer_words(DEFER_ZOOM_MASK, 2, key, strength, 0, 0, 0, 0, 0, 0));
	GE_BENCH_SKIP();
	ge_kind(KIND_SCREEN);
	struct hud_vertex *out;
	int width, height;
	/* the 4:3 square's width on the screen, and the texture's span across
	the whole screen that makes it so */
	float span = (float)SCREEN_WIDTH / (SCREEN_HEIGHT * 4.0f / 3.0f);

	if (!frame_open || !key)
		return;
	list_hud_phase();
	transparent_flush();
	if (list_full())
		return;
	if (!psp_tex_size(key, &width, &height) || !psp_tex_bind(key, NULL))
	{
		zoom_masks_missing++;
		return;
	}
	psp_tex_hud_note(key);
	out = list_take(2 * sizeof(struct hud_vertex));
	if (!out)
		return;
	out[0].u = (0.5f - span * 0.5f) * width;
	out[0].v = 0.0f;
	out[1].u = (0.5f + span * 0.5f) * width;
	out[1].v = (float)height;
	out[0].color = out[1].color = ((strength > 255 ? 255 : strength) << 24) | 0x00FFFFFF;
	out[0].x = 0.0f; out[0].y = 0.0f; out[0].z = 0.0f;
	out[1].x = (float)SCREEN_WIDTH; out[1].y = (float)SCREEN_HEIGHT; out[1].z = 0.0f;
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_FOG);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuTexScale(1.0f, 1.0f);
	sceGuTexOffset(0.0f, 0.0f);
	ge_blend(1);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, NULL, out);
	ge_blend(0);
	sceGuDisable(GU_TEXTURE_2D);
	sceGuEnable(GU_FOG);
	sceGuEnable(GU_DEPTH_TEST);
	zoom_masks++;
}

/* a meter's palette (build 23; a routine of its own from build 116, for the
models' meter shaders as well as the HUD's meters). The Xbox works a meter
out per pixel in four register-combiner stages; with a palette texture the
same sum can be done once per palette entry instead. The texture's grey (its
blue) is the position along the meter and its alpha the shape:
  shape 0                    nothing drawn (the Xbox's alpha kill)
  position past the flash    the empty colour
  otherwise                  min to max colour along the meter,
                             plus the flash colour past the fill
each multiplied by the shape; the frame then gets
colour x tint + frame x alpha (alpha: the fade, or the empty colour's).
NULL when the map has no 32-bit palette or the display list is full;
*count_out: the palette's entries */
static int meter_swap = -1;

static unsigned int *meter_palette(unsigned int key, struct psp_seethrough_meter const *meter, int *count_out)
{
	const unsigned int *palette;
	unsigned int *computed = NULL, fill = meter->gradient_min_color >> 24, flash_end = meter->flash_color >> 24, source = 0;
	int count, entry, slot, free_slot = -1, hit = 0;

	if (!psp_tex_palette(key, &palette, &count))
		return NULL;
	/* the bitmap's palette itself is part of the match: one worked out
	before the bitmap's palette was in place was kept for as long as the
	meter's words stayed the same, so a full health bar drew empty until
	something changed its words (the overshield woke it: B3) */
	for (entry = 0; entry < count; entry++)
		source = source * 31u + palette[entry];
	/* a meter that hasn't changed since it was last worked out (most of
	them, most frames) reuses that palette (build 31): the sum is a
	dozen divisions an entry, about a third of a millisecond a meter */
	for (slot = 0; slot < METER_CACHE; slot++)
	{
		if (meter_cache[slot].key == key && meter_cache[slot].count == count && meter_cache[slot].source == source &&
			!memcmp(&meter_cache[slot].meter, meter, sizeof(*meter)))
		{
			computed = meter_cache_palette[slot];
			meter_cache[slot].frame = frames + 1;
			meter_reused++;
			hit = 1;
			break;
		}
		if (meter_cache[slot].frame != frames + 1 &&
			(free_slot < 0 || meter_cache[slot].frame < meter_cache[free_slot].frame))
			free_slot = slot;
	}
	if (!computed)
	{
		/* into a slot not used this frame (the GE reads the palettes when
		the frame's list runs), or else the display list's memory */
		if (free_slot >= 0)
		{
			computed = meter_cache_palette[free_slot];
			meter_cache[free_slot].key = key;
			meter_cache[free_slot].count = count;
			meter_cache[free_slot].meter = *meter;
			meter_cache[free_slot].frame = frames + 1;
			meter_cache[free_slot].source = source;
		}
		else
		{
			computed = list_take(count * 4 + 16);
			if (!computed)
				return NULL;
			computed = (unsigned int *)(((unsigned int)computed + 15) & ~15u);
		}
		meter_worked++;
	}
	if (meter_swap < 0)
		meter_swap = psp_keep("meterswap");
	for (entry = hit ? count : 0; entry < count; entry++)
	{
		unsigned int texel = palette[entry], shape = texel >> 24, position = (texel >> 16) & 0xFF;
		int channel, out[3], alpha_out;

		/* B3 test: the position in the alpha and the shape in the grey */
		if (meter_swap)
		{
			unsigned int grey = position;

			position = shape;
			shape = grey;
		}

		/* build 116: outside tint mode 2 (the models' meters only; the HUD's
		are all in it) the blend takes no alpha, so the alpha is the alpha
		test's: 0 where the Xbox kills the pixel, otherwise all */
		if (!shape)
		{
			computed[entry] = meter->tint_mode_2 ? 0xFF000000 : 0;
			continue;
		}
		if (position >= flash_end)
		{
			for (channel = 0; channel < 3; channel++)
				out[channel] = (meter->background_color >> (16 - 8 * channel)) & 0xFF;
			alpha_out = meter->background_color >> 24;
		}
		else
		{
			/* past the fill by more than a quarter: the flash at full */
			int past = 255 - 2 * PSP_CLAMP_255(4 * ((int)fill - (int)position));
			/* build 116: the colour's run along the meter, by its gradient
			(stage 0's alpha: the position over the gradient, at least an
			eighth; the HUD's gradient is always 1, the position itself) */
			int along = (int)position;

			if (meter->gradient != 1.0f)
			{
				float gradient = meter->gradient > 0.125f ? meter->gradient : 0.125f;

				along = (int)((float)position / gradient + 0.5f);
				along = along > 255 ? 255 : along;
			}
			if (past < 0)
				past = 0;
			for (channel = 0; channel < 3; channel++)
			{
				int low = (meter->gradient_min_color >> (16 - 8 * channel)) & 0xFF;
				int high = (meter->gradient_max_color >> (16 - 8 * channel)) & 0xFF;
				int flash = (meter->flash_color >> (16 - 8 * channel)) & 0xFF;

				out[channel] = low + (high - low) * along / 255 +
					(meter->flash_color_is_negative ? -1 : 1) * flash * past / 255;
				out[channel] = PSP_CLAMP_255(out[channel]);
			}
			alpha_out = meter->tint_color >> 24;
		}
		if (!meter->tint_mode_2)
			alpha_out = 255;
		/* ABGR, each colour times the shape */
		computed[entry] = ((unsigned int)alpha_out << 24) | ((out[2] * shape / 255) << 16) |
			((out[1] * shape / 255) << 8) | (out[0] * shape / 255);
	}
	/* build 39: out of the CPU's cache before the GE reads it. The GE
	draws each piece as soon as it is listed (GU_DIRECT moves its stall
	address on at every draw), long before the frame's end writes the
	cache back: a meter worked out afresh was drawn with whatever that slot
	held in memory before, a meter's previous value, for one frame each time
	it changed (the "rubber banding" ammo and flashlight meters). The
	display list's own memory is uncached, so only the kept slots need it */
	if (!hit && computed >= &meter_cache_palette[0][0] && computed < &meter_cache_palette[METER_CACHE][0])
		ge_writeback(computed, count * 4);
	*count_out = count;
	return computed;
}

/* the glyph sheet's palette for one colour: each texel is alpha << 4 |
shade (tools/psp_glyphs.py, palette(): the same rule), and the shade runs
from the colour darkened to 15% (0), through the colour itself (0.7 of the
way), to white, so the glass takes the colour the game gave the icon, its
text's. Written into the display list's memory (list_take writes it back
before the GE reads it), 1 KB a glyph drawn: a frame has a handful */
static unsigned int *glyph_palette(unsigned int abgr)
{
	unsigned int *palette = list_take(256 * 4 + 16);
	float tint[3] = { (float)(abgr & 0xFF), (float)((abgr >> 8) & 0xFF), (float)((abgr >> 16) & 0xFF) };
	float alpha = (float)(abgr >> 24) / (15.0f * 255.0f), colour[16][3];
	int shade, a, c;

	if (!palette)
		return NULL;
	palette = (unsigned int *)(((unsigned int)palette + 15) & ~15u);
	for (shade = 0; shade < 16; shade++)
	{
		float s = shade / 15.0f;

		for (c = 0; c < 3; c++)
			colour[shade][c] = s <= 0.7f ? tint[c] * (0.15f + 0.85f * s / 0.7f) :
				tint[c] + (255.0f - tint[c]) * ((s - 0.7f) / 0.3f);
	}
	for (a = 0; a < 16; a++)
	{
		unsigned int level = (unsigned int)(a * alpha * 255.0f + 0.5f) << 24;

		for (shade = 0; shade < 16; shade++)
			palette[a << 4 | shade] = level | (unsigned int)colour[shade][2] << 16 |
				(unsigned int)colour[shade][1] << 8 | (unsigned int)colour[shade][0];
	}
	return palette;
}

/* a glyph in the icon's quad: its share of the quad's height
(psp_glyph_height) on the quad's bottom, its own shape across; centred when
it is narrower, else from the quad's left edge (the text's cursor moves on
by the difference: psp_button_glyph_extra, psp_sdl.c) */
static void glyph_quad(struct game_screen_vertex const *in, int glyph, int labelled, short blend, float red,
	float green, float blue, float fade, float offset_x, float offset_y)
{
	unsigned short const *cell = psp_glyph_cells[glyph];
	struct hud_vertex *out;
	unsigned int *palette;
	const void *sheet;
	float x0 = in[0].x, x1 = in[0].x, y0 = in[0].y, y1 = in[0].y, wide;
	int corner;

	for (corner = 1; corner < 4; corner++)
	{
		x0 = fminf(x0, in[corner].x);
		x1 = fmaxf(x1, in[corner].x);
		y0 = fminf(y0, in[corner].y);
		y1 = fmaxf(y1, in[corner].y);
	}
	/* the glyph's own share of the box's height, on the box's bottom (the
	text's line), larger in the menus with "menuglyphN" */
	{
		float zoom = !ui_centered ? 1.0f : labelled ? menu_glyph_zoom_get() : psp_ge_menu_glyph_zoom(glyph, 1);
		float box = x1 - x0, height = (y1 - y0) * psp_glyph_height[glyph] / 100.0f;

		/* a screen key's glyph keeps the right edge it has at its own size */
		if (labelled && zoom > 1.0f)
		{
			wide = height * cell[2] / cell[3];
			x1 = wide <= box ? x0 + (box + wide) * 0.5f : x0 + wide;
			x0 = x1 - wide * zoom;
		}
		else
		{
			wide = height * zoom * cell[2] / cell[3];
			if (wide <= box)
				x0 += (box - wide) * 0.5f;
			x1 = x0 + wide;
		}
		y0 = y1 - height * zoom;
	}
	palette = glyph_palette(argb_to_abgr(in[0].argb, red, green, blue, fade));
	out = list_take(2 * sizeof(struct hud_vertex));
	sheet = glyph_sheet();
	if (!out || !palette || !sheet)
		return;
	if (hud_snap < 0)
		hud_snap = !psp_keep("hudnosnap");
	/* half a texel in from the cell's edges, so the filter doesn't take in
	the neighbouring glyph */
	out[0].u = cell[0] + 0.5f;
	out[0].v = cell[1] + 0.5f;
	out[1].u = cell[0] + cell[2] - 0.5f;
	out[1].v = cell[1] + cell[3] - 0.5f;
	out[0].color = out[1].color = 0xFFFFFFFF;
	out[0].x = hud_pixel((x0 + offset_x) * ui_scale_x * hud_zoom + ui_offset_x);
	out[0].y = hud_pixel((y0 + offset_y) * ui_scale_y * hud_zoom + ui_offset_y);
	out[1].x = hud_pixel((x1 + offset_x) * ui_scale_x * hud_zoom + ui_offset_x);
	out[1].y = hud_pixel((y1 + offset_y) * ui_scale_y * hud_zoom + ui_offset_y);
	out[0].z = out[1].z = 0.0f;
	sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
	sceGuClutLoad(256 / 8, palette);
	sceGuTexMode(GU_PSM_T8, 0, 0, 0);
	sceGuTexImage(0, PSP_GLYPH_SHEET_WIDTH, PSP_GLYPH_SHEET_HEIGHT, PSP_GLYPH_SHEET_WIDTH, sheet);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_FOG);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	/* glass over the picture whatever the icon's own blend: added, as the
	HUD's icons mostly are, its dark body would vanish */
	(void)blend;
	ge_blend(1);
	sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
	ge_kind(KIND_HUD);
	sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, NULL, out);
	ge_blend(0);
	sceGuDisable(GU_TEXTURE_2D);
	sceGuEnable(GU_FOG);
	sceGuEnable(GU_DEPTH_TEST);
	if (!glyphs_drawn++)
		platform_log("GE: the PSP's button glyphs drawn for the game's button icons (\"xboxglyphs\" draws the Xbox's)");
}

static void screen_quad(void const *vertices, unsigned int key, short blend, float red, float green, float blue,
	float fade, float offset_x, float offset_y, void const *meter_parameters, int wrapped, float u_scale, float v_scale)
{
	list_hud_phase();
	struct game_screen_vertex const *in = vertices;
	struct psp_seethrough_meter const *meter = meter_parameters;
	int width, height, alpha = 0;

	if (!frame_open || !vertices)
		return;
	/* the 3D world's see-through parts go before the HUD */
	transparent_flush();
	if (list_full())
		return;
	if (((key - GLYPH_KEY_BASE) & ~GLYPH_LABELLED) < PSP_GLYPH_COUNT && !meter)
	{
		glyph_quad(in, (int)((key - GLYPH_KEY_BASE) & ~GLYPH_LABELLED), ((key - GLYPH_KEY_BASE) & GLYPH_LABELLED) != 0,
			blend, red, green, blue, fade, offset_x, offset_y);
		hud_quads++;
		return;
	}
	if (!psp_tex_size(key, &width, &height))
	{
		/* a HUD picture the texture pack doesn't have: named once each */
		static unsigned int unknown[16];
		static int unknown_count = 0;
		int index;

		for (index = 0; index < unknown_count && unknown[index] != key; index++);
		if (index == unknown_count && unknown_count < 16)
		{
			unknown[unknown_count++] = key;
			platform_log("HUD: picture %08x isn't in the texture pack", key);
		}
		return;
	}
	if (!psp_tex_bind(key, &alpha))
		return;
	psp_tex_hud_note(key);
	{
		/* the game's bitmap_data: width +4, height +6, flags +14 (bit 4: linear) */
		const unsigned char *bitmap = (const unsigned char *)key;
		int linear = (*(const unsigned short *)(bitmap + 14) >> 4) & 1;
		int bitmap_width = *(const short *)(bitmap + 4), bitmap_height = *(const short *)(bitmap + 6);

		hud_u_scale = u_scale != 0.0f ? u_scale : 1.0f;
		hud_v_scale = v_scale != 0.0f ? v_scale : 1.0f;
		if (linear && bitmap_width > 0 && bitmap_height > 0)
		{
			hud_u_scale /= bitmap_width;
			hud_v_scale /= bitmap_height;
		}
	}
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_FOG);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexWrap(wrapped ? GU_REPEAT : GU_CLAMP, wrapped ? GU_REPEAT : GU_CLAMP);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	if (meter)
	{
		/* the meter's own palette (meter_palette, above), drawn as the
		Xbox blends it: colour x tint + frame x alpha */
		unsigned int *computed;
		int count = 0;

		/* build 63: the depth test and fog back on the way out, as at the
		end; returned with them off, everything after in the frame drew
		through walls until the next HUD quad switched them back */
		computed = meter_palette(key, meter, &count);
		if (!computed)
		{
			screen_quad_bailed++;
			sceGuEnable(GU_FOG);
			sceGuEnable(GU_DEPTH_TEST);
			return;
		}
		sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
		sceGuClutLoad(count / 8, computed);
		sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
		ge_blend(1);
		{
			unsigned int tint = meter->tint_color;

			sceGuBlendFunc(GU_ADD, GU_FIX, GU_SRC_ALPHA, ((tint & 0xFF) << 16) | (tint & 0xFF00) | ((tint >> 16) & 0xFF), 0);
		}
		hud_draw(in, width, height, offset_x, offset_y, 0, 0xFFFFFFFF, 1.0f, 1.0f, 1.0f, 1.0f);
	}
	else
	{
		hud_blend(blend);
		hud_draw(in, width, height, offset_x, offset_y, 1, 0, red, green, blue, fade);
	}
	ge_blend(0);
	sceGuDisable(GU_TEXTURE_2D);
	sceGuEnable(GU_FOG);
	sceGuEnable(GU_DEPTH_TEST);
	hud_quads++;
}

/* ---------- object shadows (build 25)

The Xbox draws each object's silhouette from the light into a texture and
projects it onto the level around the object. The GE draws a soft round
blob instead, through the same projection: the game still picks the
receiving level triangles (structure_render_shadow) and the shadow's frame
(its centre, the light's direction and two axes across it); each receiving
vertex gets texture coordinates from its position in that frame, and the
blob darkens the frame buffer by the game's shadow colour, fading with
height above or below the object. The receivers lie exactly on the level,
so they are drawn where the depth is equal, cut up near the camera the
same way the level itself is. */

static struct
{
	float position[3], forward[3], left[3], up[3];
	float inverse_radius, radius;
	unsigned int darkness;
	int open;
} shadow;
static unsigned long shadows_drawn = 0, shadow_begins = 0, shadow_requests = 0;
/* build 85: a test of equal-depth drawing on the hardware. The shadow pass
re-draws the level's own triangles with the depth test EQUAL and the blob
on top, and on the PSP they fight in a dotted pattern (b79-b84: the
stripes beside the tank, the shadow on the bunker wall), while the
lightmap pass, which does the same with the wall's own vertex array and
index list, never does. Three ways, from the debug menu:
  0  as before: a copy of the triangles in a vertex format of its own (blob
     coordinates and a colour per corner), drawn without indices;
  1  the wall's draw repeated: its vertex array and an index list, the
     blob's coordinates made from the positions by the GE's texture matrix;
  2  GEQUAL with a nudge toward the eye, as the decals (not exact).
If 1 is clean and 0 isn't, the depth depends on the draw's form, and any
equal-depth pass must repeat the wall's own draw; if 1 fights too, equal
depth can't be trusted on this hardware */
static int shadow_mode;
static unsigned int blob_texture[32 * 32] __attribute__((aligned(16)));
static int blob_ready = 0;

struct shadow_vertex
{
	float u, v;
	unsigned int color;
	float x, y, z;
};

/* receivers are gathered here, then copied into the display list at their
exact size (shadows and lights; build 29) */
#define SCRATCH_TRIANGLES 480 /* build 36: 600, less 100 for the cone textures' room */
static struct shadow_vertex scratch_vertices[3 * SCRATCH_TRIANGLES];

/* the game's real_matrix4x3: scale, forward, left, up, position */
void psp_ge_shadow_begin(const float *matrix, const float *color, float radius)
{
	GE_DEFER(defer_shadow_begin(matrix, color, radius)); /* build 112: gedefer */
	GE_BENCH_SKIP();
	int k;

	shadow.open = 0;
	shadow_begins++;
	if (!frame_open || radius <= 0.0f)
		return;
	for (k = 0; k < 3; k++)
	{
		shadow.forward[k] = matrix[1 + k];
		shadow.left[k] = matrix[4 + k];
		shadow.up[k] = matrix[7 + k];
		shadow.position[k] = matrix[10 + k];
	}
	shadow.radius = radius;
	shadow.inverse_radius = 1.0f / radius;
	{
		/* the colour the frame is multiplied by under the shadow: its distance
		from white is how much the blob takes away */
		int r = PSP_CLAMP_255((int)((1.0f - color[0]) * 255.0f)), g = PSP_CLAMP_255((int)((1.0f - color[1]) * 255.0f)),
			b = PSP_CLAMP_255((int)((1.0f - color[2]) * 255.0f));

		shadow.darkness = (b << 16) | (g << 8) | r;
	}
	shadow.open = shadow.darkness != 0;
	if (!blob_ready)
	{
		/* the blob: full in the middle, nothing at the rim */
		int x, y;

		for (y = 0; y < 32; y++)
			for (x = 0; x < 32; x++)
			{
				float dx = (x + 0.5f) / 16.0f - 1.0f, dy = (y + 0.5f) / 16.0f - 1.0f;
				float d = sqrtf(dx * dx + dy * dy), t = d >= 1.0f ? 0.0f : 1.0f - d;
				int value = (int)(t * t * (3.0f - 2.0f * t) * 255.0f);

				blob_texture[32 * y + x] = 0xFF000000 | ((unsigned int)value * 0x010101);
			}
		ge_writeback(blob_texture, sizeof(blob_texture));
		blob_ready = 1;
	}
}

static void shadow_corner(const float *p, float *u, float *v, unsigned int *color)
{
	float d[3], h, fade;
	int k;
	unsigned int r, g, b;

	for (k = 0; k < 3; k++)
		d[k] = p[k] - shadow.position[k];
	*u = 0.5f + 0.5f * shadow.inverse_radius * (d[0] * shadow.forward[0] + d[1] * shadow.forward[1] + d[2] * shadow.forward[2]);
	*v = 0.5f + 0.5f * shadow.inverse_radius * (d[0] * shadow.left[0] + d[1] * shadow.left[1] + d[2] * shadow.left[2]);
	h = d[0] * shadow.up[0] + d[1] * shadow.up[1] + d[2] * shadow.up[2];
	if (h < 0.0f)
		h = -h;
	fade = 1.0f - h * shadow.inverse_radius * 0.25f;
	if (fade < 0.0f)
		fade = 0.0f;
	r = (unsigned int)((shadow.darkness & 0xFF) * fade);
	g = (unsigned int)(((shadow.darkness >> 8) & 0xFF) * fade);
	b = (unsigned int)(((shadow.darkness >> 16) & 0xFF) * fade);
	*color = 0xFF000000 | (b << 16) | (g << 8) | r;
}

static struct level_stream *level_stream_find(const void *vertices)
{
	const void *key = stream_key(vertices);
	unsigned int slot = ((unsigned int)key >> 5) % LEVEL_STREAMS, tries = 0;

	while (level_streams[slot].source && level_streams[slot].source != key && tries++ < LEVEL_STREAMS)
		slot = (slot + 1) % LEVEL_STREAMS;
	return level_streams[slot].source == key && level_streams[slot].base ? &level_streams[slot] : NULL;
}

/* build 68: a material's copies by their key (object lighting: a material's
compressed vertex data address, which is the key when the data is in the
pack). The copies hold what it asks: base and lightmap texture coordinates */
int psp_ge_level_vertex_texcoords(unsigned int key, int vertex_index, float *uv, float *lightmap_uv)
{
	/* build 112: gedefer. Waiting calls run first so the answer is the one
	the frame so far gives; under ME semantics they needn't: copies are built
	only at the frame's end there, so this reads what is built already */
	if (!defer_me_semantics)
		GE_DEFER_DRAIN();
	unsigned int slot = (key >> 5) % LEVEL_STREAMS, tries = 0;
	const struct level_stream *stream;

	while (level_streams[slot].source && (unsigned int)level_streams[slot].source != key && tries++ < LEVEL_STREAMS)
		slot = (slot + 1) % LEVEL_STREAMS;
	stream = &level_streams[slot];
	if ((unsigned int)stream->source != key || !stream->base || vertex_index < 0 || vertex_index >= stream->count)
		return 0;
	uv[0] = stream->base[vertex_index].u;
	uv[1] = stream->base[vertex_index].v;
	if (stream->lightmap)
	{
		lightmap_uv[0] = stream->lightmap[vertex_index].u;
		lightmap_uv[1] = stream->lightmap[vertex_index].v;
	}
	else
		lightmap_uv[0] = lightmap_uv[1] = 0.0f;
	return 1;
}

/* build 89: a vertex's packed normal and lightmap incident radiosity (the
map's 32-bit forms) for object lighting, which in a map built with its
geometry outside the tags read them at the Xbox's 32-byte stride from the
6-byte position copy: garbage, and reads past the copy (review). 1 if the
copies are there; radiosity 0 without a lightmap */
int psp_ge_level_vertex_lighting(unsigned int key, int vertex_index, unsigned int *normal, unsigned int *radiosity)
{
	/* build 112: gedefer. Waiting calls run first so the answer is the one
	the frame so far gives; under ME semantics they needn't: copies are built
	only at the frame's end there, so this reads what is built already */
	if (!defer_me_semantics)
		GE_DEFER_DRAIN();
	unsigned int slot = (key >> 5) % LEVEL_STREAMS, tries = 0;
	const struct level_stream *stream;

	while (level_streams[slot].source && (unsigned int)level_streams[slot].source != key && tries++ < LEVEL_STREAMS)
		slot = (slot + 1) % LEVEL_STREAMS;
	stream = &level_streams[slot];
	if ((unsigned int)stream->source != key || !stream->base || !stream->normals || vertex_index < 0 ||
		vertex_index >= stream->count)
		return 0;
	*normal = stream->normals[vertex_index];
	*radiosity = stream->radiosity ? stream->radiosity[vertex_index] : 0;
	return 1;
}

/* build 68: with a geometry pack open, the copies over their budget: the
least recently drawn go, never one drawn this frame. Open addressing can't
drop a slot without breaking the chains through it, so a dropped slot keeps
its key with no copies, and the next draw of it rebuilds in place */
static void level_stream_let_go(struct level_stream *stream)
{
	stream_bytes -= stream_size(stream);
	stream_free(stream->base);
	stream_free(stream->lightmap);
	copy_free(stream->normals);
	copy_free(stream->radiosity);
	/* build 89: the 16-bit copies too (leaked on every let-go and rebuild
	before; review) */
	copy_free(stream->compact_base);
	copy_free(stream->compact_lightmap);
	stream->base = stream->lightmap = NULL;
	stream->normals = stream->radiosity = NULL;
	stream->compact_base = stream->compact_lightmap = NULL;
	streams_let_go++;
}

/* build 111: the least recently drawn copy let go, never one drawn this
frame; 0 when none is left to let go. The trim's choice, now shared with the
copies' arena (copy_alloc) */
static int level_stream_let_go_oldest(void)
{
	int index, oldest = -1;

	for (index = 0; index < LEVEL_STREAMS; index++)
	{
		struct level_stream *stream = &level_streams[index];

		if (!stream->source || !stream->base || stream->last_frame == ge_hot.stream_frame)
			continue;
		/* geshow2: the frame being shown may still draw from it */
		if (psp_ge_show_busy && stream->last_frame + 1 == ge_hot.stream_frame)
		{
			show2_spared++;
			continue;
		}
		if (oldest < 0 || stream->last_frame < level_streams[oldest].last_frame)
			oldest = index;
	}
	if (oldest < 0)
		return 0;
	psp_ge_show3_reuse(SHOW3_LEVEL, level_streams[oldest].last_frame);
	level_stream_let_go(&level_streams[oldest]);
	return 1;
}

/* the carve's room counts too: copies kept there cost the heap nothing */
static void level_streams_trim(void)
{
	unsigned long budget = (ARENA_ROOM->bytes ? ARENA_ROOM->bytes - (ARENA_BYTES - LEVEL_STREAM_BUDGET) : LEVEL_STREAM_BUDGET) +
		(room_open(CARVE_ROOM) ? CARVE_ROOM->bytes : 0);

	while (stream_bytes > budget && level_stream_let_go_oldest())
		;
}

/* at the frame's end, when the GE and the Media Engine are done with them:
the copies drawn this frame moved to the level's origin, if it moved. The
rest move when they are next drawn */
static void level_copies_follow(void)
{
	unsigned int start;
	int index;

	if (level_local <= 0 || !level_origin_set || !level_copies_behind)
		return;
	show2_settle(SHOW2_FOLLOW); /* geshow2: the copies it moves are the ones the GE draws from */
	level_copies_behind = 0;
	start = sceKernelGetSystemTimeLow();
	for (index = 0; index < LEVEL_STREAMS; index++)
	{
		struct level_stream *stream = &level_streams[index];

		if (stream->source && stream->base && stream->last_frame == ge_hot.stream_frame &&
			memcmp(stream->origin, level_origin, sizeof(level_origin)))
			level_copy_follow(stream, level_origin);
	}
	level_move_us += sceKernelGetSystemTimeLow() - start;
}

/* A receiving vertex's corner, worked out once per shadow draw instead of
once per triangle that uses it: the receivers come as an index list, and
most of its vertices are shared by several triangles. The same functions on
the same floats, so the same corners. "shadowmemooff" (keep.txt): each
triangle's corners worked out afresh */
#define SHADOW_MEMO 64
struct shadow_memo
{
	int key; /* the vertex's index + 1; 0: empty */
	int bits, shaded;
	unsigned int color;
	float camera[3], u, v;
};
static int shadow_memo_on = -1;
/* "shadowmemocheck" (keep.txt): every corner the memo hands out is worked out
again the old loop's way and compared bit for bit, so the memo's exactness is
counted rather than inferred from pictures, which also move with timing */
static int shadow_memo_check = -1;
static unsigned long shadow_memo_checks = 0, shadow_memo_mismatches = 0, shadow_memo_logged = 0;

static inline unsigned int shadow_float_bits(float f)
{
	union { float f; unsigned int u; } bits;

	bits.f = f;
	return bits.u;
}

static inline void shadow_memo_fill(struct shadow_memo *m, const struct ge_vertex *base, unsigned short index)
{
	m->key = index + 1;
	level_to_camera(&base[index].x, m->camera);
	m->bits = outside(m->camera);
	m->shaded = 0;
}

static inline struct shadow_memo *shadow_memo_at(struct shadow_memo *memo, const struct ge_vertex *base, unsigned short index)
{
	struct shadow_memo *m = &memo[index & (SHADOW_MEMO - 1)];

	if (m->key != index + 1)
		shadow_memo_fill(m, base, index);
	return m;
}

static inline void shadow_memo_shade(struct shadow_memo *m, const struct ge_vertex *b)
{
	if (!m->shaded)
	{
		shadow_corner(&b->x, &m->u, &m->v, &m->color);
		m->shaded = 1;
	}
}

static void shadow_memo_verify(const struct shadow_memo *m, const struct ge_vertex *base, unsigned short index, int shaded)
{
	const struct ge_vertex *b = &base[index];
	float camera[3], u = 0.0f, v = 0.0f;
	unsigned int color = 0;
	int bad;

	level_to_camera(&b->x, camera);
	bad = m->key != index + 1 || memcmp(camera, m->camera, sizeof(camera)) || outside(camera) != m->bits;
	if (shaded)
	{
		shadow_corner(&b->x, &u, &v, &color);
		bad |= memcmp(&u, &m->u, sizeof(u)) || memcmp(&v, &m->v, sizeof(v)) || color != m->color;
	}
	if (bad && shadow_memo_mismatches < 4)
		platform_log("GE: shadow memo mismatch at vertex %u: key %d bits %d/%d camera %08x/%08x u %08x/%08x color %08x/%08x",
			index, m->key, m->bits, outside(camera), shadow_float_bits(camera[0]), shadow_float_bits(m->camera[0]),
			shadow_float_bits(u), shadow_float_bits(m->u), color, m->color);
	shadow_memo_mismatches += bad != 0;
	shadow_memo_checks++;
}

static void psp_ge_shadow_draw_body(const void *vertices, int vertex_count, const unsigned short *indices, long triangle_count)
{
	struct shadow_memo memo[SHADOW_MEMO];
	struct level_stream *stream;
	struct shadow_vertex *out;
	long index, kept_count = 0;
	int cut_count = 0;
	struct cut_store pieces_store;

	cut_store_begin(&pieces_store, cut_corners, MAXIMUM_CUT_CORNERS, CUT_OTHER);
	shadow_requests++;
	if (!shadow.open || !frame_open || triangle_count <= 0 || !vertices || !indices || list_full())
		return;
	/* the level's own copy, made when the level was drawn this frame */
	stream = level_stream_find(vertices);
	if (!stream)
		return;
	/* the shadow's frame from the copy's origin, as its positions are
	(psp_ge_shadow_draw puts it back) */
	level_frame(stream->origin);
	shadow.position[0] -= stream->origin[0];
	shadow.position[1] -= stream->origin[1];
	shadow.position[2] -= stream->origin[2];
	/* build 87: the pieces the level drew, where the stencil bake cut */
	if (stream->bake)
	{
		const unsigned short *swapped = bake_substitute(stream, indices, &triangle_count, 0);

		if (swapped != indices)
		{
			indices = swapped;
			vertex_count = stream->count;
		}
	}
	out = scratch_vertices;
	if (shadow_memo_on < 0)
	{
		shadow_memo_check = psp_keep("shadowmemocheck") != 0;
		shadow_memo_on = shadow_memo_check || !psp_keep("shadowmemooff");
	}
	if (shadow_memo_check && shadow_memo_checks >= shadow_memo_logged + 50000)
	{
		platform_log("GE: shadow memo checked %lu corners, %lu mismatches", shadow_memo_checks, shadow_memo_mismatches);
		shadow_memo_logged = shadow_memo_checks;
	}
	if (shadow_memo_on)
	{
		int k;

		for (k = 0; k < SHADOW_MEMO; k++)
			memo[k].key = 0;
	}
	for (index = 0; shadow_memo_on && index < triangle_count && kept_count < SCRATCH_TRIANGLES; index++)
	{
		const unsigned short *triangle = indices + 3 * index;
		struct shadow_memo *m[3], own[2];
		int corner;

		if (triangle[0] >= vertex_count || triangle[1] >= vertex_count || triangle[2] >= vertex_count)
			return;
		for (corner = 0; corner < 3; corner++)
			m[corner] = shadow_memo_at(memo, stream->base, triangle[corner]);
		/* two corners whose indices share a slot: the later lookup took it, so
		the earlier pointer would read the later vertex. That corner is worked
		out again on the side (the last lookup can't be overtaken) */
		for (corner = 0; corner < 2; corner++)
			if (m[corner]->key != triangle[corner] + 1)
			{
				shadow_memo_fill(&own[corner], stream->base, triangle[corner]);
				m[corner] = &own[corner];
			}
		if (shadow_memo_check)
		{
			int kept = !(m[0]->bits & m[1]->bits & m[2]->bits);

			for (corner = 0; corner < 3; corner++)
			{
				if (kept)
					shadow_memo_shade(m[corner], &stream->base[triangle[corner]]);
				shadow_memo_verify(m[corner], stream->base, triangle[corner], kept);
			}
		}
		if (m[0]->bits & m[1]->bits & m[2]->bits)
			continue;
		for (corner = 0; corner < 3; corner++)
			shadow_memo_shade(m[corner], &stream->base[triangle[corner]]);
		if (!(m[0]->bits | m[1]->bits | m[2]->bits))
		{
			for (corner = 0; corner < 3; corner++)
			{
				const struct ge_vertex *b = &stream->base[triangle[corner]];
				struct shadow_vertex *o = &out[3 * kept_count + corner];

				o->u = m[corner]->u; o->v = m[corner]->v; o->color = m[corner]->color;
				o->x = b->x; o->y = b->y; o->z = b->z;
			}
			kept_count++;
			continue;
		}
		{
			float corners[3][CORNER_FLOATS];

			for (corner = 0; corner < 3; corner++)
			{
				const struct ge_vertex *b = &stream->base[triangle[corner]];
				float *c = corners[corner];

				c[0] = m[corner]->camera[0]; c[1] = m[corner]->camera[1]; c[2] = m[corner]->camera[2];
				c[3] = m[corner]->u;
				c[4] = m[corner]->v;
				c[5] = (float)(m[corner]->color & 0xFF);
				c[6] = (float)((m[corner]->color >> 8) & 0xFF);
				c[7] = b->x; c[8] = b->y; c[9] = b->z;
				c[10] = 0.0f;
			}
			cut_count += clip_triangle(corners, m[0]->bits | m[1]->bits | m[2]->bits, &pieces_store);
		}
	}
	for (index = 0; !shadow_memo_on && index < triangle_count && kept_count < SCRATCH_TRIANGLES; index++)
	{
		const unsigned short *triangle = indices + 3 * index;
		float corners[3][CORNER_FLOATS] = { { 0.0f } };
		int bits[3], corner;

		if (triangle[0] >= vertex_count || triangle[1] >= vertex_count || triangle[2] >= vertex_count)
			return;
		for (corner = 0; corner < 3; corner++)
		{
			level_to_camera_corner(&stream->base[triangle[corner]].x, corners[corner]);
			bits[corner] = outside(corners[corner]);
		}
		if (bits[0] & bits[1] & bits[2])
			continue;
		if (!(bits[0] | bits[1] | bits[2]))
		{
			for (corner = 0; corner < 3; corner++)
			{
				const struct ge_vertex *b = &stream->base[triangle[corner]];
				struct shadow_vertex *o = &out[3 * kept_count + corner];

				shadow_corner(&b->x, &o->u, &o->v, &o->color);
				o->x = b->x; o->y = b->y; o->z = b->z;
			}
			kept_count++;
			continue;
		}
		/* cut up as the level was: the same corners in camera space, with the
		blob's coordinates riding along (as "base" texture coordinates) */
		for (corner = 0; corner < 3; corner++)
		{
			unsigned int color;

			shadow_corner(&stream->base[triangle[corner]].x, &corners[corner][3], &corners[corner][4], &color);
			corners[corner][5] = (float)(color & 0xFF);
			corners[corner][6] = (float)((color >> 8) & 0xFF);
		}
		cut_count += clip_triangle(corners, bits[0] | bits[1] | bits[2], &pieces_store);
	}
	if (!kept_count && !cut_count && !pieces_store.dropped)
		return;
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, 32, 32, 32, blob_texture);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	if (shadow_mode == 2)
	{
		float d[3] = { shadow.position[0] - level_eye[0], shadow.position[1] - level_eye[1], shadow.position[2] - level_eye[2] };
		float distance = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), lift, units;

		if (distance < projection_near)
			distance = projection_near;
		lift = 0.01f * distance;
		if (lift > 0.2f)
			lift = 0.2f;
		units = lift * 61439.0f * projection_near / (distance * distance);
		sceGuDepthFunc(GU_GEQUAL);
		sceGuDepthOffset(units < 3.0f ? 3 : units > 4096.0f ? 4096 : (int)units);
	}
	else
		sceGuDepthFunc(GU_EQUAL);
	sceGuDepthMask(GU_TRUE);
	ge_blend(1);
	/* the frame times (1 - blob x darkness) */
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_COLOR, 0, 0);
	/* far off, the fog leaves the frame alone */
	sceGuFog(fog_near, fog_far, 0xFF000000);
	level_model(stream->origin);
	if (kept_count && shadow_mode == 1)
	{
		/* the wall's draw repeated: its vertex array, the kept triangles'
		indices, the blob's coordinates from the positions by the texture
		matrix (u = 0.5 + 0.5/r forward.(p - position), v with left) */
		unsigned short *copy = list_take((int)kept_count * 3 * sizeof(unsigned short));

		if (copy)
		{
			ScePspFMatrix4 m;
			float k = 0.5f * shadow.inverse_radius;
			long t, n = 0;

			for (t = 0; t < triangle_count && n < kept_count; t++)
			{
				const unsigned short *triangle = indices + 3 * t;
				float c[3];
				int corner, bits = 0;

				for (corner = 0; corner < 3; corner++)
				{
					level_to_camera(&stream->base[triangle[corner]].x, c);
					bits |= outside(c);
				}
				if (bits)
					continue;
				copy[3 * n] = triangle[0]; copy[3 * n + 1] = triangle[1]; copy[3 * n + 2] = triangle[2];
				n++;
			}
			memset(&m, 0, sizeof(m));
			m.x.x = k * shadow.forward[0]; m.y.x = k * shadow.forward[1]; m.z.x = k * shadow.forward[2];
			m.w.x = 0.5f - k * (shadow.forward[0] * shadow.position[0] + shadow.forward[1] * shadow.position[1] + shadow.forward[2] * shadow.position[2]);
			m.x.y = k * shadow.left[0]; m.y.y = k * shadow.left[1]; m.z.y = k * shadow.left[2];
			m.w.y = 0.5f - k * (shadow.left[0] * shadow.position[0] + shadow.left[1] * shadow.position[1] + shadow.left[2] * shadow.position[2]);
			m.w.z = 1.0f;
			m.w.w = 1.0f;
			sceGumMatrixMode(GU_TEXTURE);
			sceGumLoadMatrix(&m);
			sceGumMatrixMode(GU_MODEL);
			sceGuTexMapMode(GU_TEXTURE_MATRIX, GU_POSITION, 0);
			sceGuColor(0xFF000000 | shadow.darkness);
			sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
				(int)n * 3, copy, stream->base);
			sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
			sceGuColor(0xFFFFFFFF);
		}
	}
	else if (kept_count)
	{
		struct shadow_vertex *copy = list_take((int)kept_count * 3 * sizeof(struct shadow_vertex));

		if (copy)
		{
			memcpy(copy, out, kept_count * 3 * sizeof(struct shadow_vertex));
			sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
				(int)kept_count * 3, NULL, copy);
		}
	}
	if (cut_count)
	{
		struct shadow_vertex *pieces = list_take(cut_count * sizeof(struct shadow_vertex));

		for (index = 0; pieces && index < cut_count; index++)
		{
			const float *c = cut_at(&pieces_store, index);
			unsigned int r = (unsigned int)c[5], g = (unsigned int)c[6];

			pieces[index].u = c[3];
			pieces[index].v = c[4];
			/* the darkness is grey or near it: blue follows green */
			pieces[index].color = 0xFF000000 | (g << 16) | (g << 8) | r;
			pieces[index].x = c[7];
			pieces[index].y = c[8];
			pieces[index].z = c[9];
		}
		if (pieces)
			sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
				cut_count, NULL, pieces);
	}
	world_model();
	sceGuFog(fog_near, fog_far, fog_color);
	ge_blend(0);
	sceGuDepthMask(GU_FALSE);
	sceGuDepthFunc(GU_GEQUAL);
	if (shadow_mode == 2)
		sceGuDepthOffset(0);
	sceGuDisable(GU_TEXTURE_2D);
	shadows_drawn++;
}

void psp_ge_shadow_draw(const void *vertices, int vertex_count, const unsigned short *indices, long triangle_count)
{
	GE_DEFER(defer_words(DEFER_SHADOW_DRAW, 4, DEFER_P(vertices), (unsigned int)vertex_count, DEFER_P(indices),
		(unsigned int)triangle_count, 0, 0, 0, 0)); /* build 112: gedefer */
	GE_BENCH_SKIP();
	ge_kind(KIND_SHADOW); /* build 103 */
	unsigned int start = ge_clock();
	float position[3];

	memcpy(position, shadow.position, sizeof(position));
	psp_ge_shadow_draw_body(vertices, vertex_count, indices, triangle_count);
	memcpy(shadow.position, position, sizeof(position));
	ge_entry_us[7] += ge_clock() - start;
}

void psp_ge_shadow_end(void)
{
	GE_DEFER(defer_words(DEFER_SHADOW_END, 0, 0, 0, 0, 0, 0, 0, 0, 0)); /* build 112: gedefer */
	shadow.open = 0;
}

/* ---------- dynamic lights (build 29)

The flashlight, muzzle flashes, plasma bolts and other moving lights. The
Xbox shapes each with a cube map (the "gel") and a falloff texture; the GE
lights each receiving level vertex on the CPU instead: a falloff with
distance, the light's cone (from its definition's cosines), and the angle to
the surface, then adds the level's base texture times that over the frame,
where the depth is equal. The game still picks the lights and the level
triangles each one reaches. */

static struct
{
	float position[3], forward[3], color[3];
	float radius, inverse_radius_squared, cosine_falloff, cosine_cutoff;
	int open;
	/* a gel's six face colours (+X, -X, +Y, -Y, +Z, -Z) and its frame
	(forward, left, up: the cube's +X, +Y, +Z), when psp_ge_light_gel gave
	them; gel 0 for a light lit by its colour alone. gel_cube: the gel
	itself, sampled at each vertex in place of the faces (psp_tex.c,
	psp_tex_gel_cube), NULL when the pack keeps none or "nogelcube" */
	int gel;
	float gel_faces[18], gel_frame[9];
	const void *gel_cube;
} light;
static unsigned long lights_drawn = 0;
/* build 119's probe: the light pass's triangles looked at, and those with a
corner in the light's reach (lit): what culling groups of triangles by box
would save */
static unsigned long light_triangles_looked = 0, light_triangles_lit = 0;
/* and the triangle loop's time, the rest of the draw's being its setup
(the stream, the bake's pieces, the texture) and its list (colours, bands) */
static unsigned long light_loop_us = 0, light_probe_draws = 0;
/* 119c: the draw's time before the loop (the stream, the bake's pieces, the
texture) and after it (colours, bands, the list) */
static unsigned long light_setup_us = 0, light_post_us = 0;
static unsigned int light_call_start = 0, light_post_start = 0;

void psp_ge_light_begin(const float *position, const float *forward, const float *color, float radius,
	float cosine_falloff, float cosine_cutoff)
{
	GE_DEFER(defer_light_begin(position, forward, color, radius, cosine_falloff, cosine_cutoff)); /* build 112: gedefer */
	GE_BENCH_SKIP();
	int k;
	static int pass_off = -1;

	/* "nolightpass": the debug menu's "not drawn" from keep.txt, for A/Bs */
	if (pass_off < 0)
	{
		pass_off = psp_keep("nolightpass");
		if (pass_off)
			light_mode = 3;
	}

	light.open = frame_open && radius > 0.0f && color[0] + color[1] + color[2] > 0.01f;
	for (k = 0; k < 3; k++)
	{
		light.position[k] = position[k];
		light.forward[k] = forward[k];
		light.color[k] = color[k];
	}
	light.radius = radius;
	light.inverse_radius_squared = radius > 0.0f ? 1.0f / (radius * radius) : 0.0f;
	light.cosine_cutoff = cosine_cutoff;
	light.cosine_falloff = cosine_falloff > cosine_cutoff ? cosine_falloff : cosine_cutoff + 0.001f;
	light.gel = 0;
	light.gel_cube = NULL;
}

/* the light just begun shaped by its gel (rasterizer.c, after the light's
begin): the Xbox looks its gel cube map up by the direction from the light
in the gel's turning frame, so a wall ahead of the light takes the +X face's
colour and one above it the +Z face's. Each vertex takes the faces' mean
colours, weighed by how far its direction leans toward each axis. NULL: by
the colour alone */
void psp_ge_light_gel(const float *gel)
{
	extern const void *psp_tex_gel_cube(unsigned int key);

	GE_DEFER(defer_light_gel(gel));
	GE_BENCH_SKIP();
	light.gel = gel != NULL;
	light.gel_cube = NULL;
	if (gel)
	{
		unsigned int cube_key;

		memcpy(light.gel_faces, gel, sizeof(light.gel_faces));
		memcpy(light.gel_frame, gel + 18, sizeof(light.gel_frame));
		memcpy(&cube_key, gel + 27, sizeof(cube_key));
		light.gel_cube = cube_key ? psp_tex_gel_cube(cube_key) : NULL;
	}
}

/* a vertex's gel colour (0-1 a channel): its direction from the light in
the gel's frame, looked up in the gel's cube where the pack keeps it, else
each axis weighed by its share of the squared length, on the face of that
axis's sign */
static void light_gel_at(const float *p, float *rgb)
{
	extern void psp_tex_gel_cube_at(const void *gel, float x, float y, float z, float *rgb);
	const float *f = light.gel_frame;
	float d[3], x, y, z, xx, yy, zz, inverse;
	const float *fx, *fy, *fz;

	d[0] = p[0] - light.position[0];
	d[1] = p[1] - light.position[1];
	d[2] = p[2] - light.position[2];
	x = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
	y = d[0] * f[3] + d[1] * f[4] + d[2] * f[5];
	z = d[0] * f[6] + d[1] * f[7] + d[2] * f[8];
	if (light.gel_cube)
	{
		psp_tex_gel_cube_at(light.gel_cube, x, y, z, rgb);
		return;
	}
	xx = x * x;
	yy = y * y;
	zz = z * z;
	inverse = xx + yy + zz > 1e-12f ? 1.0f / (xx + yy + zz) : 0.0f;
	xx *= inverse;
	yy *= inverse;
	zz *= inverse;
	fx = light.gel_faces + (x >= 0.0f ? 0 : 3);
	fy = light.gel_faces + (y >= 0.0f ? 6 : 9);
	fz = light.gel_faces + (z >= 0.0f ? 12 : 15);
	rgb[0] = xx * fx[0] + yy * fy[0] + zz * fz[0];
	rgb[1] = xx * fx[1] + yy * fy[1] + zz * fz[1];
	rgb[2] = xx * fx[2] + yy * fy[2] + zz * fz[2];
}

/* how much of the light reaches a vertex (0 to 1); cone: whether the
spotlight's cone is in it (build 36: a spotlight's cone comes from a
projected texture instead, and only the side behind the light is cut here) */
/* after b116: the level light pass's vertices for the draw in hand, by the
vertex's low bits: its reach and, once asked for, its place from the camera
(5 KB from bss; a level vertex sits in 3-6 triangles, each of which worked
both out again; an independent review) */
#define LIGHT_MEMO 256
struct light_memo_entry
{
	unsigned int tag; /* the draw's number << 16 | the vertex */
	float reach, camera[3];
	int camera_known;
};
static struct light_memo_entry light_memo[LIGHT_MEMO];
/* and a gel light's colour at each, apart so other lights don't carry it:
the tag it was worked out for */
static unsigned int light_memo_gel_tag[LIGHT_MEMO];
static float light_memo_gel[LIGHT_MEMO][3];
static unsigned int light_memo_draw = 0;
static int light_memo_check = -1; /* keep.txt's "lightmemocheck" */
static unsigned long light_memo_checks = 0, light_memo_mismatches = 0;

static float light_reach(const float *p, unsigned int packed_normal, int cone_in_vertex)
{
	float d[3], distance_squared, fade, inverse, cone, facing, normal[3];

	d[0] = light.position[0] - p[0];
	d[1] = light.position[1] - p[1];
	d[2] = light.position[2] - p[2];
	distance_squared = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
	fade = 1.0f - distance_squared * light.inverse_radius_squared;
	if (fade <= 0.0f)
		return 0.0f;
	fade *= fade;
	inverse = distance_squared > 1e-8f ? 1.0f / sqrtf(distance_squared) : 0.0f;
	if (light.cosine_cutoff > -0.99f)
	{
		cone = -(d[0] * light.forward[0] + d[1] * light.forward[1] + d[2] * light.forward[2]) * inverse;
		if (cone_in_vertex)
		{
			/* the cone: full inside the falloff angle, none outside the cutoff */
			if (cone <= light.cosine_cutoff)
				return 0.0f;
			if (cone < light.cosine_falloff)
				fade *= (cone - light.cosine_cutoff) / (light.cosine_falloff - light.cosine_cutoff);
		}
		else if (cone <= 0.0f)
			return 0.0f;
		else if (cone < 0.25f)
			fade *= cone * 4.0f;
	}
	unpack_normal(packed_normal, normal);
	facing = (normal[0] * d[0] + normal[1] * d[1] + normal[2] * d[2]) * inverse;
	if (facing <= 0.0f)
		return 0.0f;
	return fade * facing;
}

static unsigned int light_color(float reach)
{
	int r = (int)(light.color[0] * reach * 255.0f), g = (int)(light.color[1] * reach * 255.0f),
		b = (int)(light.color[2] * reach * 255.0f);

	return 0xFF000000 | (PSP_CLAMP_255(b) << 16) | (PSP_CLAMP_255(g) << 8) | PSP_CLAMP_255(r);
}

/* a gel light's colour from its reach per channel (a cut corner's 5, 6
and 10) */
static unsigned int light_color_gel(float red, float green, float blue)
{
	int r = (int)(light.color[0] * red * 255.0f), g = (int)(light.color[1] * green * 255.0f),
		b = (int)(light.color[2] * blue * 255.0f);

	return 0xFF000000 | (PSP_CLAMP_255(b) << 16) | (PSP_CLAMP_255(g) << 8) | PSP_CLAMP_255(r);
}

/* build 36: a spotlight's cone as a texture projected from the light, so its
round edge is drawn per pixel. Lit per vertex, a terrain triangle several
metres across took the cone's value at its corners only: the flashlight
showed as lit triangles (b35). The GE has one texture a pass, so the
surface's own texture can't also be multiplied in; the light carries the
texture's average colour instead (psp_tex_average), over the frame that
already shows the texture */
#define CONE_SIZE 32
#define CONE_SLOTS 4
/* build 89: the texels first and 16-aligned in EVERY slot. The aligned
attribute was on the array only: with the two floats first each slot was
1,032 bytes and slots 0 and 2 had their texels 8 bytes past a 16-byte
boundary, where the GE (which ignores a texture address's low 4 bits) read
the cone from 8 bytes early: the disc shifted a quarter of its width and the
clamped edge no longer black, bright bands across lit walls (code review) */
static struct
{
	unsigned char texels[CONE_SIZE * CONE_SIZE] __attribute__((aligned(16)));
	float falloff, cutoff;
} cones[CONE_SLOTS] __attribute__((aligned(16)));
/* the cone textures are 8-bit indices into a grey ramp */
static unsigned int cone_ramp[256] __attribute__((aligned(16)));
static int cones_used = 0, cone_next = 0;
static unsigned long light_cone_draws = 0, light_cone_skipped = 0;
/* the light's right and up directions (x.x/y.x/z.x and x.y/y.y/z.y) */
static ScePspFMatrix4 light_basis;
int psp_tex_average(unsigned int key, unsigned int *abgr);

static unsigned char *cone_texture(float falloff, float cutoff)
{
	int slot, x, y;
	float tangent;

	for (slot = 0; slot < cones_used; slot++)
		if (cones[slot].falloff == falloff && cones[slot].cutoff == cutoff)
			return cones[slot].texels;
	/* a new one; the oldest goes (four kinds of spotlight is plenty) */
	slot = cone_next;
	cone_next = (cone_next + 1) % CONE_SLOTS;
	if (cones_used < CONE_SLOTS)
		cones_used++;
	cones[slot].falloff = falloff;
	cones[slot].cutoff = cutoff;
	tangent = sqrtf(1.0f - cutoff * cutoff) / cutoff;
	for (y = 0; y < CONE_SIZE; y++)
		for (x = 0; x < CONE_SIZE; x++)
		{
			float sx = (x + 0.5f) / (CONE_SIZE / 2) - 1.0f, sy = (y + 0.5f) / (CONE_SIZE / 2) - 1.0f;
			float r = sqrtf(sx * sx + sy * sy) * tangent, cosine = 1.0f / sqrtf(1.0f + r * r), value;
			int v;

			value = (cosine - cutoff) / (falloff - cutoff);
			if (value < 0.0f || x == 0 || y == 0 || x == CONE_SIZE - 1 || y == CONE_SIZE - 1)
				value = 0.0f;
			if (value > 1.0f)
				value = 1.0f;
			v = (int)(value * 255.0f + 0.5f);
			cones[slot].texels[y * CONE_SIZE + x] = (unsigned char)v;
		}
	if (!cone_ramp[255])
	{
		for (x = 0; x < 256; x++)
			cone_ramp[x] = 0xFF000000 | (x << 16) | (x << 8) | x;
		ge_writeback(cone_ramp, sizeof(cone_ramp));
	}
	ge_writeback(cones[slot].texels, sizeof(cones[slot].texels));
	return cones[slot].texels;
}

/* the texture matrix taking a world position to the cone texture: s and t
across the cone, q the distance along the light (the GE divides by it) */
static void cone_matrix(ScePspFMatrix4 *m)
{
	float f[3] = { light.forward[0], light.forward[1], light.forward[2] }, r[3], u[3], a_s[3], a_t[3], length;
	float tangent = sqrtf(1.0f - light.cosine_cutoff * light.cosine_cutoff) / light.cosine_cutoff;
	const float *L = light.position;
	int k;

	/* any vector across the light */
	if (fabsf(f[2]) < 0.9f)
	{
		r[0] = f[1]; r[1] = -f[0]; r[2] = 0.0f;
	}
	else
	{
		r[0] = 0.0f; r[1] = f[2]; r[2] = -f[1];
	}
	length = sqrtf(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
	for (k = 0; k < 3; k++)
		r[k] /= length;
	u[0] = f[1] * r[2] - f[2] * r[1];
	u[1] = f[2] * r[0] - f[0] * r[2];
	u[2] = f[0] * r[1] - f[1] * r[0];
	light_basis.x.x = r[0]; light_basis.y.x = r[1]; light_basis.z.x = r[2];
	light_basis.x.y = u[0]; light_basis.y.y = u[1]; light_basis.z.y = u[2];
	for (k = 0; k < 3; k++)
	{
		a_s[k] = 0.5f * f[k] + 0.5f * r[k] / tangent;
		a_t[k] = 0.5f * f[k] + 0.5f * u[k] / tangent;
	}
	m->x.x = a_s[0]; m->x.y = a_t[0]; m->x.z = f[0]; m->x.w = 0.0f;
	m->y.x = a_s[1]; m->y.y = a_t[1]; m->y.z = f[1]; m->y.w = 0.0f;
	m->z.x = a_s[2]; m->z.y = a_t[2]; m->z.z = f[2]; m->z.w = 0.0f;
	m->w.x = -(a_s[0] * L[0] + a_s[1] * L[1] + a_s[2] * L[2]);
	m->w.y = -(a_t[0] * L[0] + a_t[1] * L[1] + a_t[2] * L[2]);
	m->w.z = -(f[0] * L[0] + f[1] * L[1] + f[2] * L[2]);
	m->w.w = 1.0f;
}

/* whether a triangle is wholly outside the pyramid round the spotlight's
cone: all its corners beyond one of the four sides, or behind the light */
static int outside_cone(const float *p0, const float *p1, const float *p2, float tangent)
{
	const float *p[3] = { p0, p1, p2 };
	ScePspFMatrix4 const *m = &light_basis;
	int k, bits = 0x1F;

	for (k = 0; k < 3 && bits; k++)
	{
		float d[3] = { p[k][0] - light.position[0], p[k][1] - light.position[1], p[k][2] - light.position[2] };
		float x = d[0] * m->x.x + d[1] * m->y.x + d[2] * m->z.x;
		float y = d[0] * m->x.y + d[1] * m->y.y + d[2] * m->z.y;
		float z = (d[0] * light.forward[0] + d[1] * light.forward[1] + d[2] * light.forward[2]) * tangent;
		int b = 0;

		if (x > z) b |= 1;
		if (x < -z) b |= 2;
		if (y > z) b |= 4;
		if (y < -z) b |= 8;
		if (z <= 0.0f) b |= 16;
		bits &= b;
	}
	return bits != 0;
}

/* the light pass's cut pieces (psp_ge_light_draw_body begins it) */
static struct cut_store light_pieces;

/* Mode 2 lifts the light toward the eye so that cut and split pieces, whose
corners are new, still land on the surface the base pass drew. A whole
triangle kept as the level drew it has the base pass's own corners, so it
lands on the very same depths (as the lightmap pass does) and needs no lift.
Lifted, a face seen edge-on put its light over the face in front of it along
the corner between them: a bright line or dashes, the full height of a
pillar. Those triangles are drawn without the lift. Keep word "lightliftall":
every triangle lifted */
static int light_exact_on = -1, light_emit_exact = 0;
static unsigned char light_exact_of[SCRATCH_TRIANGLES];
static unsigned long light_exact_triangles = 0;

/* one lit triangle (corners: camera space, texture coordinates, the light's
reach at 5, the world position at 7): kept, cut up, or skipped by the view */
static void light_emit(float corners[3][CORNER_FLOATS], struct shadow_vertex *out, long *kept_count, int *cut_count)
{
	int bits[3], corner;

	if (*kept_count >= SCRATCH_TRIANGLES)
		return;
	for (corner = 0; corner < 3; corner++)
		bits[corner] = outside(corners[corner]);
	if (bits[0] & bits[1] & bits[2])
		return;
	if (!(bits[0] | bits[1] | bits[2]))
	{
		for (corner = 0; corner < 3; corner++)
		{
			struct shadow_vertex *o = &out[3 * *kept_count + corner];

			o->u = corners[corner][3]; o->v = corners[corner][4];
			if (light.gel) /* no tint: a gel light is never a spotlight */
				o->color = light_color_gel(corners[corner][5], corners[corner][6], corners[corner][10]);
			else
				*(float *)&o->color = corners[corner][5]; /* coloured once the tint is known */
			o->x = corners[corner][7]; o->y = corners[corner][8]; o->z = corners[corner][9];
		}
		light_exact_of[*kept_count] = (unsigned char)light_emit_exact;
		(*kept_count)++;
		return;
	}
	*cut_count += clip_triangle(corners, bits[0] | bits[1] | bits[2], &light_pieces);
}

/* build 89: the light pass's nudge (mode 2), as b85's shadow test: a lift
of 1% of the distance, at most 0.2 units, in depth steps at that distance;
and the distance bands it is drawn in (squared distances 4, 16, 64, 256,
1024: bands 2, 4, 8, 16, 32 units and beyond) */
#define LIGHT_BANDS 6
static int light_nudge_units(float distance)
{
	float lift, units;

	if (distance < projection_near)
		distance = projection_near;
	lift = 0.01f * distance;
	if (lift > 0.2f)
		lift = 0.2f;
	units = lift * 61439.0f * projection_near / (distance * distance);
	return units < 3.0f ? 3 : units > 4096.0f ? 4096 : (int)units;
}

/* build 117 (b113 #13): the light's pieces cut at the near plane, each
corner moved toward the eye along its own line of sight (so it stays on its
pixel) by the lift light_nudge_units gives its own distance. b116 nudged
all of a draw's cut pieces by one depth offset, sized by the corner nearest
the eye; but a piece cut at the near plane reaches from there to 8 units
and more, and a nudge sized for 0.1 units lifts the floor 8 units off by
about 1 unit, past the wall in front of it: the floor's light seen through
the map room bunker's hallway wall (b108-b116 [M]). Measured along the
view, as the depth buffer measures; a corner never moves past the near
plane. Keep word "lightcutone": as before */
static int light_cut_one = -1;

/* Mode 2 draws a light's whole triangles in up to six distance bands, each
with one depth nudge sized for its nearest corner, and its cut pieces as one
more draw: up to seven draws a light and surface, each band a pass over
every kept triangle and a copy. "lightmerge" (keep.txt, off) lifts every
corner by its own distance instead, as the cut pieces already are, and draws
all of them at once. A band's nudge may lift a corner up to four times what
its distance asks, so the merged lift is smaller for most corners: the
light can sit a hair deeper into a surface it fights with. That is a look
change, so it is the maintainer's call after an A/B (d20, where lights are many) */
static int light_merge = -1;
static unsigned long light_merged_draws = 0;

/* light_lift_corner's lift for the merged draw, worked out as a share of
the depth so a corner costs one division: min(1% of the depth, 0.2 units),
kept between 3 and 4096 depth steps (one step at depth d is d * d * step,
step = 1 / (61439 * near)), and never past the near plane. It skips
light_lift_corner's rounding to whole depth steps, a sub-step difference */
static inline void light_lift_corner_merged(struct shadow_vertex *corner, float step)
{
	float dx = corner->x - level_eye[0], dy = corner->y - level_eye[1], dz = corner->z - level_eye[2];
	float depth = dx * camera_forward[0] + dy * camera_forward[1] + dz * camera_forward[2], inverse, share, low, high,
		most, scale;

	if (depth <= projection_near)
		return;
	inverse = 1.0f / depth;
	share = depth <= 20.0f ? 0.01f : 0.2f * inverse;
	low = 3.0f * step * depth;
	high = 4096.0f * step * depth;
	share = share < low ? low : share > high ? high : share;
	most = 1.0f - projection_near * inverse;
	if (share > most)
		share = most;
	scale = 1.0f - share;
	corner->x = level_eye[0] + dx * scale;
	corner->y = level_eye[1] + dy * scale;
	corner->z = level_eye[2] + dz * scale;
}

static void light_lift_corner(struct shadow_vertex *corner)
{
	float dx = corner->x - level_eye[0], dy = corner->y - level_eye[1], dz = corner->z - level_eye[2];
	float depth = dx * camera_forward[0] + dy * camera_forward[1] + dz * camera_forward[2], lift, scale;

	if (depth <= projection_near)
		return;
	/* light_nudge_units' depth steps at this depth, in world units */
	lift = (float)light_nudge_units(depth) * depth * depth / (61439.0f * projection_near);
	if (lift > depth - projection_near)
		lift = depth - projection_near;
	/* along the line of sight, the depth shrinks by the same share */
	scale = (depth - lift) / depth;
	corner->x = level_eye[0] + dx * scale;
	corner->y = level_eye[1] + dy * scale;
	corner->z = level_eye[2] + dz * scale;
}

static int light_band(float squared)
{
	return squared < 4.0f ? 0 : squared < 16.0f ? 1 : squared < 64.0f ? 2 : squared < 256.0f ? 3 :
		squared < 1024.0f ? 4 : 5;
}

/* ---------- build 103: lit triangles split near the light

b102 (The Library): the flashlight barely showed on floors and lit
walls only from very close. The Xbox measures the light's falloff per pixel
(a 32x32x32 attenuation texture indexed by the pixel's offset from the
light, 0.5/radius: rasterizer_xbox_environment.c); the GE pass measures it
at the triangle's three corners and blends between them, so a floor
triangle several metres across, lit in its middle, had its corners out of
reach or behind the light: dim, or skipped whole. A spotlight's triangle
with an edge longer than a fifth of the light's radius is now split into
four, again and again (four levels at most) where it can reach the light,
and each piece lit at its own corners. Keep word "nolightsplit": as before */
static int light_split_on = -1;

/* B7, a restoration, on by default whatever it costs (the project's call): a
spotlight's pool drawn with the surface's own texture, as the point lights
are, its cone worked out per corner. With one texture a pass the projected
cone took the texture's slot and the surface came in as its average colour
only, so the flashlight on a10's dark walls was a dim, flat pool while it
lit models fully. The pieces split finer near the light, so the pool's edge
stays round. "lightconetex": the projected cone as before */
static int light_base_tex = -1;

static int light_spot_base(void)
{
	if (light_base_tex < 0)
		light_base_tex = !psp_keep("lightconetex");
	return light_base_tex;
}

/* The split decided per edge, not per triangle: two triangles sharing an
edge split it alike, at the same midpoint (0.5 * (a + b) is the same float
from either end), so their pieces meet corner to corner. Decided per
triangle, a neighbour split one level less put its long edge against the
other's two halves: a T-junction, which the GE rasterises with hairline
gaps, dark along the seams of the flashlight's pool. Keep word
"lightsplitold": the per-triangle split */
static int light_split_edges = -1;
static unsigned long light_split_triangles = 0, light_split_pieces = 0;

static void light_emit_spot(float corners[3][CORNER_FLOATS], struct shadow_vertex *out, long *kept_count, int *cut_count)
{
	/* the projected cone mirrors behind the light, so the part of the
	triangle there is cut off (build 36), as psp_ge_light_draw_body does */
	float polygon[4][CORNER_FLOATS], q[3];
	int count = 0, k, corner;

	for (corner = 0; corner < 3; corner++)
		q[corner] = (corners[corner][7] - light.position[0]) * light.forward[0] +
			(corners[corner][8] - light.position[1]) * light.forward[1] +
			(corners[corner][9] - light.position[2]) * light.forward[2] - 0.05f;
	if (q[0] <= 0.0f && q[1] <= 0.0f && q[2] <= 0.0f)
		return;
	if (q[0] >= 0.0f && q[1] >= 0.0f && q[2] >= 0.0f)
	{
		light_emit(corners, out, kept_count, cut_count);
		return;
	}
	for (corner = 0; corner < 3; corner++)
	{
		int next = (corner + 1) % 3;

		if (q[corner] >= 0.0f)
			memcpy(polygon[count++], corners[corner], sizeof(polygon[0]));
		if ((q[corner] >= 0.0f) != (q[next] >= 0.0f))
		{
			float t = q[corner] / (q[corner] - q[next]);

			for (k = 0; k < CORNER_FLOATS; k++)
				polygon[count][k] = corners[corner][k] + (corners[next][k] - corners[corner][k]) * t;
			count++;
		}
	}
	for (k = 1; k + 1 < count; k++)
	{
		float piece[3][CORNER_FLOATS];

		memcpy(piece[0], polygon[0], sizeof(piece[0]));
		memcpy(piece[1], polygon[k], sizeof(piece[0]));
		memcpy(piece[2], polygon[k + 1], sizeof(piece[0]));
		light_emit(piece, out, kept_count, cut_count);
	}
}

static float distance_squared3(const float *a, const float *b)
{
	float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];

	return x * x + y * y + z * z;
}

static float light_split_tangent = 1.0f;

/* symmetric in a and b, bit for bit, so both triangles on an edge agree */
static int light_edge_splits(const float *a, const float *b)
{
	float near_a = distance_squared3(a, light.position), near_b = distance_squared3(b, light.position);
	float nearest = sqrtf(near_a < near_b ? near_a : near_b), split, least;

	if (light_spot_base())
	{
		/* the cone worked out per corner: pieces about a third of the pool's
		width where they are, so the pool has corners inside it to be bright
		and round (by distance alone the pool near the player was two pieces
		across and drew dim) */
		split = 0.3f * nearest * (light_split_tangent > 0.05f ? light_split_tangent : 0.05f);
		least = 0.03f * light.radius;
	}
	else
	{
		split = 0.5f * nearest;
		least = 0.2f * light.radius;
	}
	if (split < least)
		split = least;
	return distance_squared3(a, b) > split * split;
}

static void light_split(float p[3][3], float uv[3][2], const unsigned int normal[3], int depth,
	struct shadow_vertex *out, long *kept_count, int *cut_count)
{
	float longest = 0.0f, nearest = 1e30f, split;
	int corner;

	if (*kept_count >= SCRATCH_TRIANGLES)
		return;
	/* a piece wholly outside the cone's pyramid lights nothing (the first
	emulator test split 460 triangles a light, most of them outside it) */
	if (depth > 0 && outside_cone(p[0], p[1], p[2], light_split_tangent))
		return;
	for (corner = 0; corner < 3; corner++)
	{
		float edge = distance_squared3(p[corner], p[(corner + 1) % 3]);
		float reach = distance_squared3(p[corner], light.position);

		if (edge > longest)
			longest = edge;
		if (reach < nearest)
			nearest = reach;
	}
	longest = sqrtf(longest);
	nearest = sqrtf(nearest);
	/* no point of the triangle is nearer the light than its nearest corner
	less its longest edge: beyond the radius, nothing of it is lit */
	if (nearest - longest > light.radius)
		return;
	/* finer near the light, where the falloff changes fastest: pieces about
	a fifth of the radius across there, about half their distance further
	out; three levels at most (64 pieces) */
	split = 0.5f * nearest;
	if (split < 0.2f * light.radius)
		split = 0.2f * light.radius;
	if (light_split_edges < 0)
		light_split_edges = !psp_keep("lightsplitold");
	if (light_split_edges)
	{
		/* halve the longest edge that needs it, then the halves again: six
		halvings, about the old three levels of four (64 pieces at most) */
		int pick = -1;
		float pick_length = 0.0f;

		for (corner = 0; corner < 3 && depth < (light_spot_base() ? 9 : 6); corner++)
		{
			const float *a = p[corner], *b = p[(corner + 1) % 3];
			float length = distance_squared3(a, b);

			if (length > pick_length && light_edge_splits(a, b))
			{
				pick = corner;
				pick_length = length;
			}
		}
		if (pick >= 0)
		{
			int next = (pick + 1) % 3, other = (pick + 2) % 3, k;
			float half[2][3][3], half_uv[2][3][2], m[3], muv[2];
			unsigned int half_normal[3];

			for (k = 0; k < 3; k++)
				m[k] = 0.5f * (p[pick][k] + p[next][k]);
			muv[0] = 0.5f * (uv[pick][0] + uv[next][0]);
			muv[1] = 0.5f * (uv[pick][1] + uv[next][1]);
			/* (pick, m, other) and (m, next, other): the parent's winding */
			memcpy(half[0][0], p[pick], sizeof(m));
			memcpy(half[0][1], m, sizeof(m));
			memcpy(half[0][2], p[other], sizeof(m));
			memcpy(half[1][0], m, sizeof(m));
			memcpy(half[1][1], p[next], sizeof(m));
			memcpy(half[1][2], p[other], sizeof(m));
			memcpy(half_uv[0][0], uv[pick], sizeof(muv));
			memcpy(half_uv[0][1], muv, sizeof(muv));
			memcpy(half_uv[0][2], uv[other], sizeof(muv));
			memcpy(half_uv[1][0], muv, sizeof(muv));
			memcpy(half_uv[1][1], uv[next], sizeof(muv));
			memcpy(half_uv[1][2], uv[other], sizeof(muv));
			light_split_pieces++;
			/* level surfaces are flat: one normal does for the pieces */
			half_normal[0] = half_normal[1] = half_normal[2] = normal[pick];
			light_split(half[0], half_uv[0], half_normal, depth + 1, out, kept_count, cut_count);
			light_split(half[1], half_uv[1], half_normal, depth + 1, out, kept_count, cut_count);
			return;
		}
	}
	else if (depth < 3 && longest > split)
	{
		float m[3][3], muv[3][2], piece[3][3], piece_uv[3][2];
		unsigned int piece_normal[3];
		int k;

		for (corner = 0; corner < 3; corner++)
		{
			int next = (corner + 1) % 3;

			for (k = 0; k < 3; k++)
				m[corner][k] = 0.5f * (p[corner][k] + p[next][k]);
			muv[corner][0] = 0.5f * (uv[corner][0] + uv[next][0]);
			muv[corner][1] = 0.5f * (uv[corner][1] + uv[next][1]);
		}
		light_split_pieces += 3;
		/* the three corner pieces and the middle one; a midpoint takes the
		normal of the corner it starts from (level surfaces are flat) */
		for (corner = 0; corner < 3; corner++)
		{
			int previous = (corner + 2) % 3;

			memcpy(piece[0], p[corner], sizeof(piece[0]));
			memcpy(piece[1], m[corner], sizeof(piece[0]));
			memcpy(piece[2], m[previous], sizeof(piece[0]));
			memcpy(piece_uv[0], uv[corner], sizeof(piece_uv[0]));
			memcpy(piece_uv[1], muv[corner], sizeof(piece_uv[0]));
			memcpy(piece_uv[2], muv[previous], sizeof(piece_uv[0]));
			piece_normal[0] = piece_normal[1] = piece_normal[2] = normal[corner];
			light_split(piece, piece_uv, piece_normal, depth + 1, out, kept_count, cut_count);
		}
		piece_normal[0] = normal[0]; piece_normal[1] = normal[1]; piece_normal[2] = normal[2];
		light_split(m, muv, piece_normal, depth + 1, out, kept_count, cut_count);
		return;
	}
	{
		float corners[3][CORNER_FLOATS], reach[3];

		for (corner = 0; corner < 3; corner++)
			reach[corner] = light_reach(p[corner], normal[corner], light_spot_base());
		if (reach[0] <= 0.0f && reach[1] <= 0.0f && reach[2] <= 0.0f)
			return;
		memset(corners, 0, sizeof(corners));
		for (corner = 0; corner < 3; corner++)
		{
			level_to_camera_corner(p[corner], corners[corner]);
			corners[corner][3] = uv[corner][0];
			corners[corner][4] = uv[corner][1];
			corners[corner][5] = reach[corner];
		}
		light_emit_spot(corners, out, kept_count, cut_count);
	}
}

static void psp_ge_light_draw_body(const void *vertices, int vertex_count, const unsigned short *indices, long triangle_count,
	unsigned int base_key)
{
	struct level_stream *stream;
	struct shadow_vertex *out;
	long index, kept_count = 0, whole_count = 0;
	int cut_count = 0, alpha, spot = light.cosine_cutoff > 0.05f && light.cosine_cutoff < 0.999f;
	/* build 86, modes 1 and 2: the whole triangles' indices (mode 1), their
	reach summed for the flat colour; the nearest lit corner (mode 2) */
	unsigned short *whole = NULL;
	float reach_sum = 0.0f, gel_sum[3] = { 0.0f, 0.0f, 0.0f };
	unsigned int average = 0xFFFFFFFF;
	unsigned char *cone = NULL;
	ScePspFMatrix4 cone_projection;
	float tangent = 0.0f, tint[3] = { 1.0f, 1.0f, 1.0f }, saved[3], near_saved;
	int cone_tex;

	if (!light.open || !frame_open || triangle_count <= 0 || !vertices || !indices || list_full())
		return;
	if (light_mode == 3)
	{
		lights_skipped++;
		return;
	}
	cut_store_begin(&light_pieces, cut_corners, MAXIMUM_CUT_CORNERS, CUT_OTHER);
	stream = level_stream_find(vertices);
	if (!stream)
		return;
	/* the light from the copy's origin, as its positions are
	(psp_ge_light_draw puts it back) */
	level_frame(stream->origin);
	light.position[0] -= stream->origin[0];
	light.position[1] -= stream->origin[1];
	light.position[2] -= stream->origin[2];
	/* build 87: the pieces the level drew, where the stencil bake cut */
	if (stream->bake)
	{
		const unsigned short *swapped = bake_substitute(stream, indices, &triangle_count, 0);

		if (swapped != indices)
		{
			indices = swapped;
			vertex_count = stream->count;
		}
	}
	cone_tex = spot && !light_spot_base();
	if (spot)
	{
		if (!psp_tex_bind(base_key, &alpha))
			return;
		/* the projected cone's way: the base texture's average colour (it
		must be resident) */
		if (cone_tex)
		{
			if (!psp_tex_average(base_key, &average))
				return;
			tint[0] = (average & 0xFF) / 255.0f;
			tint[1] = ((average >> 8) & 0xFF) / 255.0f;
			tint[2] = ((average >> 16) & 0xFF) / 255.0f;
		}
		tangent = sqrtf(1.0f - light.cosine_cutoff * light.cosine_cutoff) / light.cosine_cutoff;
		cone_matrix(&cone_projection);
	}
	else if (!psp_tex_bind(base_key, &alpha))
		return;
	out = scratch_vertices;
	if (light_mode == 1)
		whole = list_take((int)triangle_count * 3 * sizeof(unsigned short));
	/* build 117: the light's own pieces cut a hair past the near plane, so
	a corner on the cut has room for its 1% lift toward the eye
	(light_lift_corner) without crossing the plane; at the plane itself it
	would get none, where a wall beside the player needs the most (an
	independent review). The sliver left out is 1% of the near plane's
	distance deep */
	if (light_cut_one < 0)
		light_cut_one = psp_keep("lightcutone");
	near_saved = near_clip;
	if (light_mode == 2 && !light_cut_one)
		near_clip = projection_near * 1.0102f;
	/* after b116: this draw's vertices remembered (light_memo) */
	if (light_memo_check < 0)
		light_memo_check = psp_keep("lightmemocheck");
	if (light_memo_check && light_memo_checks >= 200000)
	{
		platform_log("GE: light memo checked %lu times, %lu mismatches", light_memo_checks, light_memo_mismatches);
		light_memo_checks = light_memo_mismatches = 0;
	}
	if (++light_memo_draw > 0xFFFF)
	{
		memset(light_memo, 0, sizeof(light_memo));
		memset(light_memo_gel_tag, 0, sizeof(light_memo_gel_tag));
		light_memo_draw = 1;
	}
	unsigned int light_loop_start = probe_clock(); /* build 119's probe */

	unsigned long probe_looked = 0, probe_lit = 0; /* in registers; added once below */
#ifdef PSP_RECON
	/* the draw census's runs of 16 triangles: each run's nearest corner to the light */
	int census_runs = census_wanted();
	long census_run = -1, census_run_count = 0;
	float census_run_nearest = 0.0f, census_radius_squared = light.radius * light.radius;

	if (census_runs)
	{
		census_light_draws[spot]++;
		census_light_surface(vertices, indices, triangle_count);
	}
#endif

	if (probes_on)
		light_setup_us += light_loop_start - light_call_start;

	for (index = 0; index < triangle_count && kept_count < SCRATCH_TRIANGLES; index++)
	{
		const unsigned short *triangle = indices + 3 * index;
		/* after b116: no clear of all 33 floats a triangle; each corner's
		fields are written below before they're read, but 6 and 10 (an
		independent review) */
		float corners[3][CORNER_FLOATS], reach[3];
		int corner;
		const float *source[3];

		if (triangle[0] >= vertex_count || triangle[1] >= vertex_count || triangle[2] >= vertex_count)
		{
			near_clip = near_saved;
			return;
		}
		/* build 68: from the copies (position, and the packed normal kept
		beside them), not the map's vertices, which may be in the pack */
		for (corner = 0; corner < 3; corner++)
			source[corner] = &stream->base[triangle[corner]].x;
		if (spot && outside_cone(source[0], source[1], source[2], tangent))
		{
			light_cone_skipped++;
			continue;
		}
		if (spot)
		{
			/* build 103: a large triangle split near the light */
			if (light_split_on < 0)
				light_split_on = !psp_keep("nolightsplit");
			if (light_split_on)
			{
				float p[3][3], uv[3][2], longest = 0.0f;
				unsigned int normal[3];
				int split_it;

				for (corner = 0; corner < 3; corner++)
				{
					p[corner][0] = source[corner][0];
					p[corner][1] = source[corner][1];
					p[corner][2] = source[corner][2];
					uv[corner][0] = stream->base[triangle[corner]].u;
					uv[corner][1] = stream->base[triangle[corner]].v;
					normal[corner] = stream->normals[triangle[corner]];
				}
				for (corner = 0; corner < 3; corner++)
				{
					float edge = distance_squared3(p[corner], p[(corner + 1) % 3]);

					if (edge > longest)
						longest = edge;
				}
				/* the cone per corner: any edge longer than its piece
				size there goes to the split, small triangles included */
				light_split_tangent = tangent;
				split_it = longest > 0.04f * light.radius * light.radius;
				if (light_spot_base())
					for (corner = 0; corner < 3 && !split_it; corner++)
						split_it = light_edge_splits(p[corner], p[(corner + 1) % 3]);
				if (split_it)
				{
					float nearest = 1e30f;

					for (corner = 0; corner < 3; corner++)
					{
						float reach = distance_squared3(p[corner], light.position);

						if (reach < nearest)
							nearest = reach;
					}
					/* out of the light's reach altogether: as before */
					if (sqrtf(nearest) - sqrtf(longest) > light.radius)
						continue;
					light_split_triangles++;
					light_split_tangent = tangent;
					light_split(p, uv, normal, 0, out, &kept_count, &cut_count);
					continue;
				}
			}
		}
		for (corner = 0; corner < 3; corner++)
		{
			/* after b116: a vertex shared by several triangles (3-6 in a
			level's lists) worked out once a draw: its reach, and its place
			from the camera when a triangle first needs it (the same
			functions on the same inputs: the same floats) */
			unsigned int vertex = triangle[corner], tag = (light_memo_draw << 16) | vertex;
			struct light_memo_entry *memo = &light_memo[vertex & (LIGHT_MEMO - 1)];

			if (memo->tag != tag)
			{
				memo->tag = tag;
				memo->reach = light_reach(source[corner], stream->normals[vertex], !cone_tex);
				memo->camera_known = 0;
			}
			reach[corner] = memo->reach;
			if (light_memo_check)
			{
				/* "lightmemocheck": worked out again and compared, bit for bit */
				float again = light_reach(source[corner], stream->normals[vertex], !cone_tex);

				if (memcmp(&again, &memo->reach, sizeof(again)))
					light_memo_mismatches++;
				light_memo_checks++;
			}
		}
#ifdef PSP_RECON
		if (census_runs)
		{
			float nearest = distance_squared3(source[0], light.position), d;

			if ((d = distance_squared3(source[1], light.position)) < nearest)
				nearest = d;
			if ((d = distance_squared3(source[2], light.position)) < nearest)
				nearest = d;
			if ((index >> 4) != census_run)
			{
				if (census_run_count)
					census_light_run(census_run_count, census_run_nearest, census_radius_squared);
				census_run = index >> 4;
				census_run_count = 0;
				census_run_nearest = nearest;
			}
			else if (nearest < census_run_nearest)
				census_run_nearest = nearest;
			census_run_count++;
			census_light_looked[spot]++;
		}
#endif
		probe_looked++;
		if (reach[0] <= 0.0f && reach[1] <= 0.0f && reach[2] <= 0.0f)
			continue;
		probe_lit++;
		for (corner = 0; corner < 3; corner++)
		{
			struct light_memo_entry *memo = &light_memo[triangle[corner] & (LIGHT_MEMO - 1)];
			float direct[3];

			/* two corners of one triangle can share a slot (their low bits
			alike): the later one took it, so the earlier is worked out
			here instead (lightmemocheck found it on d20) */
			if (memo->tag != ((light_memo_draw << 16) | triangle[corner]))
			{
				level_to_camera(&stream->base[triangle[corner]].x, direct);
				memo = NULL;
			}
			else if (!memo->camera_known)
			{
				level_to_camera(&stream->base[triangle[corner]].x, memo->camera);
				memo->camera_known = 1;
			}
			else if (light_memo_check)
			{
				float again[3];

				level_to_camera(&stream->base[triangle[corner]].x, again);
				if (memcmp(again, memo->camera, sizeof(again)))
					light_memo_mismatches++;
				light_memo_checks++;
			}
			corners[corner][0] = memo ? memo->camera[0] : direct[0];
			corners[corner][1] = memo ? memo->camera[1] : direct[1];
			corners[corner][2] = memo ? memo->camera[2] : direct[2];
			corners[corner][7] = stream->base[triangle[corner]].x;
			corners[corner][8] = stream->base[triangle[corner]].y;
			corners[corner][9] = stream->base[triangle[corner]].z;
			corners[corner][3] = stream->base[triangle[corner]].u;
			corners[corner][4] = stream->base[triangle[corner]].v;
			corners[corner][5] = reach[corner];
			corners[corner][6] = 0.0f;
			corners[corner][10] = 0.0f;
		}
		if (light.gel)
		{
			/* reach per channel: 5, 6 and 10 are cut as one */
			for (corner = 0; corner < 3; corner++)
			{
				unsigned int vertex = triangle[corner], tag = (light_memo_draw << 16) | vertex;
				unsigned int slot = vertex & (LIGHT_MEMO - 1);
				float rgb[3];

				if (reach[corner] <= 0.0f)
				{
					rgb[0] = rgb[1] = rgb[2] = 0.0f;
				}
				else if (light_memo_gel_tag[slot] == tag)
				{
					rgb[0] = light_memo_gel[slot][0];
					rgb[1] = light_memo_gel[slot][1];
					rgb[2] = light_memo_gel[slot][2];
				}
				else
				{
					light_gel_at(source[corner], rgb);
					light_memo_gel_tag[slot] = tag;
					light_memo_gel[slot][0] = rgb[0];
					light_memo_gel[slot][1] = rgb[1];
					light_memo_gel[slot][2] = rgb[2];
				}
				corners[corner][5] = reach[corner] * rgb[0];
				corners[corner][6] = reach[corner] * rgb[1];
				corners[corner][10] = reach[corner] * rgb[2];
			}
		}
		if (spot)
		{
			/* the projected cone mirrors behind the light, so the part of
			the triangle there is cut off (build 36) */
			float polygon[4][CORNER_FLOATS] = { { 0.0f } }, q[3];
			int count = 0, k;

			for (corner = 0; corner < 3; corner++)
				q[corner] = (corners[corner][7] - light.position[0]) * light.forward[0] +
					(corners[corner][8] - light.position[1]) * light.forward[1] +
					(corners[corner][9] - light.position[2]) * light.forward[2] - 0.05f;
			if (q[0] <= 0.0f && q[1] <= 0.0f && q[2] <= 0.0f)
				continue;
			if (q[0] < 0.0f || q[1] < 0.0f || q[2] < 0.0f)
			{
				for (corner = 0; corner < 3; corner++)
				{
					int next = (corner + 1) % 3;

					if (q[corner] >= 0.0f)
						memcpy(polygon[count++], corners[corner], sizeof(polygon[0]));
					if ((q[corner] >= 0.0f) != (q[next] >= 0.0f))
					{
						float t = q[corner] / (q[corner] - q[next]);

						for (k = 0; k < CORNER_FLOATS; k++)
							polygon[count][k] = corners[corner][k] + (corners[next][k] - corners[corner][k]) * t;
						count++;
					}
				}
				for (k = 1; k + 1 < count; k++)
				{
					memcpy(corners[0], polygon[0], sizeof(polygon[0]));
					memcpy(corners[1], polygon[k], sizeof(polygon[0]));
					memcpy(corners[2], polygon[k + 1], sizeof(polygon[0]));
					light_emit(corners, out, &kept_count, &cut_count);
				}
				continue;
			}
		}
		if (whole)
		{
			/* mode 1: a triangle wholly in view is drawn as the level drew it */
			if (!(outside(corners[0]) | outside(corners[1]) | outside(corners[2])))
			{
				whole[3 * whole_count] = triangle[0];
				whole[3 * whole_count + 1] = triangle[1];
				whole[3 * whole_count + 2] = triangle[2];
				whole_count++;
				reach_sum += reach[0] + reach[1] + reach[2];
				if (light.gel)
				{
					gel_sum[0] += corners[0][5] + corners[1][5] + corners[2][5];
					gel_sum[1] += corners[0][6] + corners[1][6] + corners[2][6];
					gel_sum[2] += corners[0][10] + corners[1][10] + corners[2][10];
				}
				continue;
			}
		}
		if (light_exact_on < 0)
			light_exact_on = !psp_keep("lightliftall");
		light_emit_exact = light_exact_on;
		light_emit(corners, out, &kept_count, &cut_count);
		light_emit_exact = 0;
	}
#ifdef PSP_RECON
	if (census_run_count)
		census_light_run(census_run_count, census_run_nearest, census_radius_squared);
#endif
	if (probes_on)
	{
		light_post_start = probe_clock();
		light_loop_us += light_post_start - light_loop_start;
		light_probe_draws++;
		light_triangles_looked += probe_looked;
		light_triangles_lit += probe_lit;
	}
	near_clip = near_saved;
	if (!kept_count && !cut_count && !whole_count && !light_pieces.dropped)
		return;
	/* the light's colour, times the surface's average colour for a projected cone */
	for (index = 0; index < 3; index++)
	{
		saved[index] = light.color[index];
		light.color[index] *= tint[index];
	}
	if (!light.gel)
		for (index = 0; index < kept_count * 3; index++)
			out[index].color = light_color(*(float *)&out[index].color);
	sceGuEnable(GU_TEXTURE_2D);
	if (cone_tex)
	{
		cone = cone_texture(light.cosine_falloff, light.cosine_cutoff);
		sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
		sceGuClutLoad(256 / 8, cone_ramp);
		sceGuTexMode(GU_PSM_T8, 0, 0, 0);
		sceGuTexImage(0, CONE_SIZE, CONE_SIZE, CONE_SIZE, cone);
		sceGuTexFilter(GU_LINEAR, GU_LINEAR);
		sceGuTexWrap(GU_CLAMP, GU_CLAMP);
		sceGuTexMapMode(GU_TEXTURE_MATRIX, 0, 0);
		sceGuTexProjMapMode(GU_POSITION);
		/* straight to the GE: pspgum's own texture matrix slot isn't sent */
		sceGuSetMatrix(GU_TEXTURE, &cone_projection);
		light_cone_draws++;
	}
	else
		sceGuTexWrap(GU_REPEAT, GU_REPEAT);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
	/* mode 2: GEQUAL, the nudge set per batch below (build 89) */
	sceGuDepthFunc(light_mode == 2 ? GU_GEQUAL : GU_EQUAL);
	sceGuDepthMask(GU_TRUE);
	ge_blend(1);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
	sceGuFog(fog_near, fog_far, 0xFF000000);
	level_model(stream->origin);
	if (whole_count)
	{
		/* mode 1: the level's vertex array (positions and base texture
		coordinates; a spotlight's cone comes from the texture matrix as
		before), so the GE gets the very inputs the level's pass had. No
		colour per corner in that format: the average reach, flat */
		sceGuColor(light.gel ? light_color_gel(gel_sum[0] / (float)(whole_count * 3),
			gel_sum[1] / (float)(whole_count * 3), gel_sum[2] / (float)(whole_count * 3)) :
			light_color(reach_sum / (float)(whole_count * 3)));
		sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
			(int)whole_count * 3, whole, stream->base);
		sceGuColor(0xFFFFFFFF);
		light_index_triangles += whole_count;
	}
	if (light_merge < 0)
		light_merge = psp_keep("lightmerge");
	if (light_merge && light_mode == 2 && !light_cut_one && (kept_count || cut_count))
	{
		long total = kept_count * 3 + cut_count;
		struct shadow_vertex *all = list_take((int)total * (int)sizeof(struct shadow_vertex));
		float step = 1.0f / (61439.0f * projection_near);

		if (all)
		{
			memcpy(all, out, kept_count * 3 * sizeof(struct shadow_vertex));
			for (index = 0; index < kept_count * 3; index++)
				light_lift_corner_merged(&all[index], step);
			for (index = 0; index < cut_count; index++)
			{
				const float *c = cut_at(&light_pieces, index);
				struct shadow_vertex *v = &all[kept_count * 3 + index];

				v->u = c[3];
				v->v = c[4];
				v->color = light.gel ? light_color_gel(c[5], c[6], c[10]) : light_color(c[5]);
				v->x = c[7];
				v->y = c[8];
				v->z = c[9];
				light_lift_corner_merged(v, step);
			}
			sceGuDepthOffset(0);
			sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
				(int)total, NULL, all);
			light_merged_draws++;
		}
		kept_count = 0;
		cut_count = 0;
	}
	if (kept_count && light_mode == 2)
	{
		/* build 89: in distance bands, each with the nudge its own nearest
		corner needs. b86-b88 sized one nudge for the whole draw from its
		nearest corner, even one behind the eye or off screen: a floor corner
		under the camera gave hundreds of depth steps, which lift a wall 5-10
		units off by 0.5-2 units, so the light was painted over whatever
		stood that close in front of it (review). Within a band (distances
		doubling) the lift is at most 4 times the one asked for */
		unsigned char band_of[SCRATCH_TRIANGLES];
		/* and one more band, LIGHT_BANDS: the exact triangles, no lift */
		long band_count[LIGHT_BANDS + 1], t;
		float band_near[LIGHT_BANDS + 1];
		int b;

		for (b = 0; b <= LIGHT_BANDS; b++)
		{
			band_count[b] = 0;
			band_near[b] = 1e30f;
		}
		for (t = 0; t < kept_count; t++)
		{
			float nearest = 1e30f;
			int c;

			for (c = 0; c < 3; c++)
			{
				const struct shadow_vertex *v = &out[3 * t + c];
				float dx = v->x - level_eye[0], dy = v->y - level_eye[1], dz = v->z - level_eye[2];
				float d2 = dx * dx + dy * dy + dz * dz;

				if (d2 < nearest)
					nearest = d2;
			}
			b = light_exact_of[t] ? LIGHT_BANDS : light_band(nearest);
			band_of[t] = (unsigned char)b;
			band_count[b]++;
			if (nearest < band_near[b])
				band_near[b] = nearest;
		}
		for (b = 0; b <= LIGHT_BANDS; b++)
		{
			struct shadow_vertex *copy;
			long n = 0;

			if (!band_count[b])
				continue;
			copy = list_take((int)band_count[b] * 3 * sizeof(struct shadow_vertex));
			if (!copy)
				break;
			for (t = 0; t < kept_count; t++)
				if (band_of[t] == b)
					memcpy(copy + 3 * n++, out + 3 * t, 3 * sizeof(struct shadow_vertex));
			if (b == LIGHT_BANDS)
				light_exact_triangles += n;
			sceGuDepthOffset(b == LIGHT_BANDS ? 0 : light_nudge_units(sqrtf(band_near[b])));
			sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
				(int)n * 3, NULL, copy);
			light_nudged_draws++;
		}
	}
	else if (kept_count)
	{
		struct shadow_vertex *copy = list_take((int)kept_count * 3 * sizeof(struct shadow_vertex));

		if (copy)
		{
			memcpy(copy, out, kept_count * 3 * sizeof(struct shadow_vertex));
			sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
				(int)kept_count * 3, NULL, copy);
		}
	}
	if (cut_count)
	{
		struct shadow_vertex *pieces = list_take(cut_count * sizeof(struct shadow_vertex));
		float nearest = 1e30f;

		if (light_cut_one < 0)
			light_cut_one = psp_keep("lightcutone");
		for (index = 0; pieces && index < cut_count; index++)
		{
			const float *c = cut_at(&light_pieces, index);

			pieces[index].u = c[3];
			pieces[index].v = c[4];
			pieces[index].color = light.gel ? light_color_gel(c[5], c[6], c[10]) : light_color(c[5]);
			pieces[index].x = c[7];
			pieces[index].y = c[8];
			pieces[index].z = c[9];
			if (light_mode == 2 && !light_cut_one)
				light_lift_corner(&pieces[index]);
			else if (light_mode == 2)
			{
				float dx = c[7] - level_eye[0], dy = c[8] - level_eye[1], dz = c[9] - level_eye[2];
				float d2 = dx * dx + dy * dy + dz * dz;

				if (d2 < nearest)
					nearest = d2;
			}
		}
		if (pieces && light_mode == 2)
		{
			/* build 117: the corners carry their own lift; no nudge */
			sceGuDepthOffset(light_cut_one ? light_nudge_units(sqrtf(nearest)) : 0);
			light_nudged_draws++;
		}
		if (pieces)
			sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_3D,
				cut_count, NULL, pieces);
	}
	world_model();
	for (index = 0; index < 3; index++)
		light.color[index] = saved[index];
	if (cone_tex)
		sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
	sceGuFog(fog_near, fog_far, fog_color);
	ge_blend(0);
	sceGuDepthMask(GU_FALSE);
	sceGuDepthFunc(GU_GEQUAL);
	if (light_mode == 2)
		sceGuDepthOffset(0);
	sceGuDisable(GU_TEXTURE_2D);
	lights_drawn++;
}

void psp_ge_light_draw(const void *vertices, int vertex_count, const unsigned short *indices, long triangle_count,
	unsigned int base_key)
{
	GE_PRETOUCH(base_key, TXPT_LIGHT);
	GE_DEFER(defer_words(DEFER_LIGHT_DRAW, 5, DEFER_P(vertices), (unsigned int)vertex_count, DEFER_P(indices),
		(unsigned int)triangle_count, base_key, 0, 0, 0)); /* build 112: gedefer */
	GE_BENCH_SKIP();
	ge_kind(KIND_LIGHT); /* build 103 */
	unsigned int start = ge_clock(), end;

	float position[3];

	light_call_start = start; /* 119c's probe */
	light_post_start = 0;
	memcpy(position, light.position, sizeof(position));
	psp_ge_light_draw_body(vertices, vertex_count, indices, triangle_count, base_key);
	memcpy(light.position, position, sizeof(position));
	end = ge_clock();
	if (light_post_start)
		light_post_us += end - light_post_start;
	ge_entry_us[6] += end - start;
}

/* ---------- the screen flash (build 23): a colour over the whole picture,
lightening (taking damage, pickups), darkening or tinting it, at the
game's intensity (rasterizer_xbox_screen_effect.c's sums). Build 116 (b113
#9): max, min and invert as the Xbox draws them (the GE has its MAX and MIN
blends and the factors invert needs); builds 23-115 drew max as a lighten
and min as a darken, so a needle's pink flash went solid over the picture
where the Xbox's only lifts what is darker than it. Keep word
"flashnearest": as before */

static unsigned long flashes_drawn = 0;
static int flash_nearest = -1;

/* a flash colour channel as a byte, clamped */
static unsigned int flash_byte(float value)
{
	return (unsigned int)PSP_CLAMP_255((int)(value * 255.0f + 0.5f));
}

void psp_ge_screen_flash(short type, float intensity, float const *argb)
{
	GE_DEFER(defer_screen_flash(type, intensity, argb)); /* build 112: gedefer */
	struct hud_vertex *out;
	float a, r, g, b;
	unsigned int color, fix;

	if (!frame_open || type <= 0 || intensity <= 0.0f || list_full())
		return;
	transparent_flush();
	a = argb[0] * intensity; r = argb[1] * intensity; g = argb[2] * intensity; b = argb[3] * intensity;
	color = ((unsigned int)PSP_CLAMP_255((int)(a * 255.0f)) << 24) | (PSP_CLAMP_255((int)(b * 255.0f)) << 16) |
		(PSP_CLAMP_255((int)(g * 255.0f)) << 8) | PSP_CLAMP_255((int)(r * 255.0f));
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_FOG);
	sceGuDisable(GU_TEXTURE_2D);
	ge_blend(1);
	if (flash_nearest < 0)
		flash_nearest = psp_keep("flashnearest");
	if (!flash_nearest && (type == 3 || type == 4 || type == 5))
	{
		/* the Xbox's source colour for each (its combiner's sums of the
		flash colour f and its alpha fa, each channel clamped) */
		float f[3] = { r, g, b }, source[3];
		int k;

		for (k = 0; k < 3; k++)
			source[k] = type == 3 ? f[k] - a : type == 4 ? f[k] + a : f[k] * (2.0f * f[k] - 1.0f) + a;
		color = 0xFF000000 | (flash_byte(source[2]) << 16) | (flash_byte(source[1]) << 8) | flash_byte(source[0]);
		if (type == 3) /* max: the brighter of the picture and the colour */
			sceGuBlendFunc(GU_MAX, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
		else if (type == 4) /* min: the darker */
			sceGuBlendFunc(GU_MIN, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
		else /* invert: the colour times (1 - the picture), plus the picture times (1 - f) */
			sceGuBlendFunc(GU_ADD, GU_ONE_MINUS_DST_COLOR, GU_FIX, 0,
				(flash_byte(1.0f - b) << 16) | (flash_byte(1.0f - g) << 8) | flash_byte(1.0f - r));
	}
	else switch (type)
	{
	case 2: /* darken: the colour taken away */
	case 4: /* min */
		sceGuBlendFunc(GU_REVERSE_SUBTRACT, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
		break;
	case 6: /* tint: the picture times (1 - (1 - colour) x intensity), plus the alpha */
		fix = (PSP_CLAMP_255((int)((1.0f - (1.0f - argb[3]) * intensity) * 255.0f)) << 16) |
			(PSP_CLAMP_255((int)((1.0f - (1.0f - argb[2]) * intensity) * 255.0f)) << 8) |
			PSP_CLAMP_255((int)((1.0f - (1.0f - argb[1]) * intensity) * 255.0f));
		color = (color & 0xFF000000) | ((color >> 24) * 0x010101);
		sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, fix);
		break;
	default: /* lighten: the colour added, the picture kept by one minus its alpha */
		sceGuBlendFunc(GU_ADD, GU_FIX, GU_ONE_MINUS_SRC_ALPHA, 0xFFFFFF, 0);
		break;
	}
	out = list_take(2 * sizeof(struct hud_vertex));
	if (!out)
		return;
	memset(out, 0, 2 * sizeof(struct hud_vertex));
	out[1].x = SCREEN_WIDTH;
	out[1].y = SCREEN_HEIGHT;
	out[0].color = out[1].color = color;
	sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, NULL, out);
	ge_blend(0);
	sceGuEnable(GU_FOG);
	sceGuEnable(GU_DEPTH_TEST);
	flashes_drawn++;
}

/* ---------- decals (build 23)

Bullet holes, blood, scorch marks and the level's painted markings: the
game's decal vertices (position, then 16-bit texture coordinates), four to a
quad, one colour and an intensity a decal. The Xbox's combiners fade a decal
toward "no change" as its intensity drops; the GE does the same sums with
its blend texture function (fragment x (1 - texel) + constant x texel) and
the frame buffer's blending:
  multiply          frame x (1 - i c (1 - t))
  double multiply   frame x 2 (0.5 - i c (0.5 - t))
  alpha blend, add  t c, by alpha t.a i / added times i
Decals lie on the surfaces they mark, so they are drawn a little nearer
(the GE's depth offset) and without writing depth. */

static unsigned long decals_drawn = 0;

struct decal_vertex_ge
{
	short u, v;
	float x, y, z;
};

static int decal_pull = -1, decal_big = 0, decal_all = -1, decal_slope = -1, decal_batch = 0;
static int decal_square = -1;
static unsigned long decals_small = 0, decals_offscreen = 0;
/* build 106: decal calls with no quad on the screen, left before the bind */
static unsigned long decals_wholly_left_out = 0;
/* "decalnocull": quads off the screen go through the work too (to time it) */
static int decal_cull = -1;

#define DECAL_MINIMUM_PIXELS 2.0f /* build 58: 2 (was 3: a little too far out, b57) */


/* build 90: a permanent decal (the level's painted symbols) drawn on the
level's own triangles under it, as the base pass drew them this frame
(frame_level), with depth EQUAL: the texture is put on them by the GE's
texture matrix from each position (the decal quad's own mapping, worked out
from three of its corners), and the decal texture's clamped edge (black for
an added symbol) leaves the rest of each triangle as it was. A decal over
several planes (wrapped round an edge) is drawn per plane. 0 if nothing
under it was drawn this frame: the caller draws it the usual way */
/* build 106: off unless the debug menu turns it on. In every log since
(The Library on the PSP in b105, b30 and The Library in the emulator) it
drew none on the level's triangles ("0 drawn on the level's own
triangles"): each permanent decal fell back to the usual draw after the
scan, which cost 0.8 ms a frame at b30's beach in the emulator */
static int decal_project = 0;
static unsigned long decals_projected = 0, decal_projected_triangles = 0, decals_project_fallback = 0;
static unsigned short project_indices[3 * 1024];
/* build 91: what lies under each decal plane, remembered. b90 tested every
triangle the level drew this frame against every plane, each frame: 3-4 ms
a frame at blue base and 15 ms in front of red base (b90 benchrun). The
level's triangles under a permanent decal never change, so the hits are kept
per plane (the union over the frames that scanned) and drawn from the list;
a plane is scanned again every DECAL_RESCAN frames (every DECAL_RESCAN_EMPTY
while it has nothing), so a symbol coming into view is complete within half
a second. Each
hit keeps its stream's pack address, so a stream let go and rebuilt for
other vertices isn't drawn from. The near-plane pieces are still found each
frame (they change with the eye) */
#define DECAL_CACHE_PLANES 32
#define DECAL_CACHE_HITS 96
#define DECAL_RESCAN 16
/* a plane with nothing under it yet (out of view, or not drawn): looked at
again every 4 frames */
#define DECAL_RESCAN_EMPTY 4
struct decal_cache_hit
{
	const struct level_stream *stream;
	const void *source;
	unsigned short tri[3];
};
static struct decal_cache_slot
{
	unsigned int key;
	unsigned int scanned;
	int count;
	struct decal_cache_hit hit[DECAL_CACHE_HITS];
} decal_cache[DECAL_CACHE_PLANES];
static int decal_cache_next = 0;
static unsigned long decal_cache_scans = 0, decal_cache_uses = 0, decal_project_us = 0;
/* build 106: the decals' timers, on one call in DECAL_TIMED_ONE_IN and
counted that many times over. The clock is a system call, and b30's beach
draws hundreds of decals a frame: timing each one was 6% of the game
thread in the emulator's PC samples (tools/pcprofile.py) */
#define DECAL_TIMED_ONE_IN 16
static int decal_timed = 0;
static inline unsigned int decal_clock(void)
{
	return decal_timed ? ge_clock() : 0;
}
/* build 119's probe: a timed call's body by piece (setup, the off-screen
loop, the texture and the list's room, the corners, the state and the draw) */
static unsigned int decal_probe_mark = 0;
static unsigned long decal_probe_us[5];
#define DECAL_PROBE(piece) do { if (decal_timed && probes_on) { unsigned int now_ = ge_clock(); \
	decal_probe_us[piece] += (now_ - decal_probe_mark) * DECAL_TIMED_ONE_IN; decal_probe_mark = now_; } } while (0)

static unsigned int decal_plane_key(const float *n, float d, const float *low, const float *high)
{
	unsigned int h = 2166136261u;
	int k;

	for (k = 0; k < 3; k++)
	{
		h = (h ^ (unsigned int)(int)(n[k] * 1000.0f)) * 16777619u;
		h = (h ^ (unsigned int)(int)(low[k] * 100.0f)) * 16777619u;
		h = (h ^ (unsigned int)(int)(high[k] * 100.0f)) * 16777619u;
	}
	h = (h ^ (unsigned int)(int)(d * 100.0f)) * 16777619u;
	return h ? h : 1;
}

static struct decal_cache_slot *decal_cache_find(unsigned int key)
{
	int i;
	struct decal_cache_slot *slot;

	for (i = 0; i < DECAL_CACHE_PLANES; i++)
		if (decal_cache[i].key == key)
			return &decal_cache[i];
	slot = &decal_cache[decal_cache_next];
	decal_cache_next = (decal_cache_next + 1) % DECAL_CACHE_PLANES;
	slot->key = key;
	slot->count = 0;
	slot->scanned = ge_hot.stream_frame - DECAL_RESCAN;
	return slot;
}

struct decal_plane
{
	float n[3], d, gu[3], gv[3], cu, cv, low[3], high[3];
};

static int decal_project_hit(const float *a, const float *b, const float *c, const struct decal_plane *g)
{
	float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
	float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
	float length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
	int axis;

	for (axis = 0; axis < 3; axis++)
	{
		float lo = a[axis], hi = a[axis];

		if (b[axis] < lo) lo = b[axis];
		if (c[axis] < lo) lo = c[axis];
		if (b[axis] > hi) hi = b[axis];
		if (c[axis] > hi) hi = c[axis];
		if (hi < g->low[axis] || lo > g->high[axis])
			return 0;
	}
	if (length <= 0.0f || fabsf(n[0] * g->n[0] + n[1] * g->n[1] + n[2] * g->n[2]) < 0.95f * length)
		return 0;
	return fabsf(a[0] * g->n[0] + a[1] * g->n[1] + a[2] * g->n[2] - g->d) < 0.08f &&
		fabsf(b[0] * g->n[0] + b[1] * g->n[1] + b[2] * g->n[2] - g->d) < 0.08f &&
		fabsf(c[0] * g->n[0] + c[1] * g->n[1] + c[2] * g->n[2] - g->d) < 0.08f;
}

/* the origin decals are given from: the level's, so they sit on the level's
numbers */
static const float *decal_origin(void)
{
	return level_local > 0 && level_origin_set ? level_origin : world_origin;
}

/* a plane's texture matrix (world to the decal's coordinates) and the model
matrix, for drawing a level copy's own positions */
static void decal_project_copy(const struct decal_plane *g, const float *origin)
{
	ScePspFMatrix4 m;

	memset(&m, 0, sizeof(m));
	m.x.x = g->gu[0]; m.y.x = g->gu[1]; m.z.x = g->gu[2];
	m.w.x = g->cu + g->gu[0] * origin[0] + g->gu[1] * origin[1] + g->gu[2] * origin[2];
	m.x.y = g->gv[0]; m.y.y = g->gv[1]; m.z.y = g->gv[2];
	m.w.y = g->cv + g->gv[0] * origin[0] + g->gv[1] * origin[1] + g->gv[2] * origin[2];
	m.w.z = 1.0f;
	m.w.w = 1.0f;
	/* straight to the GE (pspgum's texture matrix slot isn't sent; as the
	spotlight's cone) */
	sceGuSetMatrix(GU_TEXTURE, &m);
	level_model(origin);
}

/* decal_project_hit for a copy's positions */
static int decal_project_hit_copy(const float *origin, const float *a, const float *b, const float *c,
	const struct decal_plane *g)
{
	float w[3][3];
	int k;

	for (k = 0; k < 3; k++)
	{
		w[0][k] = a[k] + origin[k];
		w[1][k] = b[k] + origin[k];
		w[2][k] = c[k] + origin[k];
	}
	return decal_project_hit(w[0], w[1], w[2], g);
}

static int decal_project_draw(const unsigned char *in, int quad_count)
{
	struct decal_plane planes[8];
	int plane_count = 0, quad, p, r, triangles = 0, scan;
	struct decal_cache_slot *slot;

	if (frame_level_stamp != ge_hot.stream_frame || !frame_level_count)
		return 0;
	for (quad = 0; quad < quad_count; quad++)
	{
		const float *v[4];
		float uv[4][2], e1[3], e2[3], n[3], length, a11, a12, a22, det;
		int k;
		struct decal_plane g;

		for (k = 0; k < 4; k++)
		{
			const unsigned char *x = in + 16 * (4 * quad + k);

			v[k] = (const float *)x;
			uv[k][0] = *(const unsigned short *)(x + 12) * (1.0f / 32768.0f);
			uv[k][1] = *(const unsigned short *)(x + 14) * (1.0f / 32768.0f);
		}
		for (k = 0; k < 3; k++)
		{
			e1[k] = v[1][k] - v[0][k];
			e2[k] = v[3][k] - v[0][k];
		}
		n[0] = e1[1] * e2[2] - e1[2] * e2[1];
		n[1] = e1[2] * e2[0] - e1[0] * e2[2];
		n[2] = e1[0] * e2[1] - e1[1] * e2[0];
		length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		a11 = e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2];
		a12 = e1[0] * e2[0] + e1[1] * e2[1] + e1[2] * e2[2];
		a22 = e2[0] * e2[0] + e2[1] * e2[1] + e2[2] * e2[2];
		det = a11 * a22 - a12 * a12;
		if (length < 1e-6f || det < 1e-12f)
			continue;
		for (k = 0; k < 3; k++)
			g.n[k] = n[k] / length;
		g.d = g.n[0] * v[0][0] + g.n[1] * v[0][1] + g.n[2] * v[0][2];
		{
			/* the gradient of u (and v) in the quad's plane: g = a e1 + b e2
			with g.e1 = du1 and g.e2 = du2 */
			float du1 = uv[1][0] - uv[0][0], du2 = uv[3][0] - uv[0][0];
			float dv1 = uv[1][1] - uv[0][1], dv2 = uv[3][1] - uv[0][1];
			float au = (du1 * a22 - du2 * a12) / det, bu = (du2 * a11 - du1 * a12) / det;
			float av = (dv1 * a22 - dv2 * a12) / det, bv = (dv2 * a11 - dv1 * a12) / det;

			for (k = 0; k < 3; k++)
			{
				g.gu[k] = au * e1[k] + bu * e2[k];
				g.gv[k] = av * e1[k] + bv * e2[k];
			}
			g.cu = uv[0][0] - (g.gu[0] * v[0][0] + g.gu[1] * v[0][1] + g.gu[2] * v[0][2]);
			g.cv = uv[0][1] - (g.gv[0] * v[0][0] + g.gv[1] * v[0][1] + g.gv[2] * v[0][2]);
		}
		for (k = 0; k < 3; k++)
		{
			int c;

			g.low[k] = g.high[k] = v[0][k];
			for (c = 1; c < 4; c++)
			{
				if (v[c][k] < g.low[k]) g.low[k] = v[c][k];
				if (v[c][k] > g.high[k]) g.high[k] = v[c][k];
			}
		}
		/* the same plane as an earlier quad: one draw, its box grown */
		for (p = 0; p < plane_count; p++)
			if (planes[p].n[0] * g.n[0] + planes[p].n[1] * g.n[1] + planes[p].n[2] * g.n[2] > 0.999f &&
				fabsf(planes[p].d - g.d) < 0.01f)
				break;
		if (p < plane_count)
		{
			for (k = 0; k < 3; k++)
			{
				if (g.low[k] < planes[p].low[k]) planes[p].low[k] = g.low[k];
				if (g.high[k] > planes[p].high[k]) planes[p].high[k] = g.high[k];
			}
		}
		else if (plane_count < 8)
			planes[plane_count++] = g;
	}
	if (!plane_count)
		return 0;
	sceGuDepthFunc(GU_EQUAL);
	sceGuDepthOffset(0);
	if (level_cull)
	{
		sceGuFrontFace(level_cull == 1 ? LEVEL_FRONT : (LEVEL_FRONT == GU_CW ? GU_CCW : GU_CW));
		sceGuEnable(GU_CULL_FACE);
	}
	sceGuTexMapMode(GU_TEXTURE_MATRIX, 0, 0);
	sceGuTexProjMapMode(GU_POSITION);
	for (p = 0; p < plane_count; p++)
	{
		struct decal_plane *g = &planes[p];
		int k;

		for (k = 0; k < 3; k++)
		{
			g->low[k] -= 0.05f;
			g->high[k] += 0.05f;
		}
		slot = decal_cache_find(decal_plane_key(g->n, g->d, g->low, g->high));
		scan = (int)(ge_hot.stream_frame - slot->scanned) >= (slot->count ? DECAL_RESCAN : DECAL_RESCAN_EMPTY);
		if (scan)
		{
			slot->scanned = ge_hot.stream_frame;
			decal_cache_scans++;
		}
		else
			decal_cache_uses++;
		/* from memory: the remembered hits, a draw per stream, for streams
		drawn this frame with no near-plane pieces (those are scanned below) */
		if (!scan)
		{
			int h = 0;

			while (h < slot->count)
			{
				const struct level_stream *stream = slot->hit[h].stream;
				int count = 0, drawn = 0, q;

				for (q = 0; q < frame_level_count && !drawn; q++)
					if (frame_level[q].stream == stream)
						drawn = 1;
				/* the hits are stored stream by stream (one scan's order). A
				remembered triangle with a corner outside the near plane or the
				guard band was cut up or left out by the base pass this frame
				(the classifier's own test): its pieces are found below */
				if (drawn)
					level_frame(stream->origin);
				for (; h < slot->count && slot->hit[h].stream == stream; h++)
					if (drawn && slot->hit[h].source == stream->source)
					{
						int c, out = 0;

						for (c = 0; c < 3 && !out; c++)
						{
							float camera[3];

							level_to_camera(&stream->base[slot->hit[h].tri[c]].x, camera);
							out = outside(camera);
						}
						if (out)
							continue;
						project_indices[3 * count] = slot->hit[h].tri[0];
						project_indices[3 * count + 1] = slot->hit[h].tri[1];
						project_indices[3 * count + 2] = slot->hit[h].tri[2];
						count++;
					}
				if (count)
				{
					unsigned short *list = list_take((count * 3 * sizeof(unsigned short) + 3) & ~3);

					if (list)
					{
						memcpy(list, project_indices, count * 3 * sizeof(unsigned short));
						decal_project_copy(g, stream->origin);
						sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
							count * 3, list, stream->base);
						triangles += count;
					}
				}
			}
		}
		for (r = 0; r < frame_level_count; r++)
		{
			const struct frame_level_draw *f = &frame_level[r];
			const struct level_stream *stream = f->stream;
			int count = 0, t, h, scan_here;

			if (stream->high[0] < g->low[0] || stream->low[0] > g->high[0] || stream->high[1] < g->low[1] ||
				stream->low[1] > g->high[1] || stream->high[2] < g->low[2] || stream->low[2] > g->high[2])
				continue;
			/* a draw with triangles cut at the near plane is scanned: a
			remembered triangle may be one of them this frame, drawn as its
			pieces (whole, its EQUAL draw would stripe and its pieces add on
			top) */
			scan_here = scan;
			for (t = 0; scan_here && t < f->kept && count < 1024; t++)
			{
				const unsigned short *tri = f->indices + 3 * t;

				if (decal_project_hit_copy(stream->origin, &stream->base[tri[0]].x, &stream->base[tri[1]].x,
					&stream->base[tri[2]].x, g))
				{
					project_indices[3 * count] = tri[0];
					project_indices[3 * count + 1] = tri[1];
					project_indices[3 * count + 2] = tri[2];
					count++;
					/* remembered (once) */
					for (h = 0; h < slot->count; h++)
						if (slot->hit[h].stream == stream && slot->hit[h].tri[0] == tri[0] &&
							slot->hit[h].tri[1] == tri[1] && slot->hit[h].tri[2] == tri[2])
							break;
					if (h == slot->count && slot->count < DECAL_CACHE_HITS)
					{
						slot->hit[h].stream = stream;
						slot->hit[h].source = stream->source;
						slot->hit[h].tri[0] = tri[0];
						slot->hit[h].tri[1] = tri[1];
						slot->hit[h].tri[2] = tri[2];
						slot->count++;
					}
				}
			}
			if (count)
			{
				unsigned short *list = list_take((count * 3 * sizeof(unsigned short) + 3) & ~3);

				if (list)
				{
					memcpy(list, project_indices, count * 3 * sizeof(unsigned short));
					decal_project_copy(g, stream->origin);
					sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
						count * 3, list, stream->base);
					triangles += count;
				}
			}
			/* the corners cut at the near plane, as the base pass drew them */
			count = 0;
			for (t = 0; f->cut && t + 2 < f->cut_count && count < 1024; t += 3)
				if (decal_project_hit_copy(stream->origin, &f->cut[t].x, &f->cut[t + 1].x, &f->cut[t + 2].x, g))
				{
					project_indices[3 * count] = (unsigned short)t;
					project_indices[3 * count + 1] = (unsigned short)(t + 1);
					project_indices[3 * count + 2] = (unsigned short)(t + 2);
					count++;
				}
			if (count)
			{
				unsigned short *list = list_take((count * 3 * sizeof(unsigned short) + 3) & ~3);

				if (list)
				{
					memcpy(list, project_indices, count * 3 * sizeof(unsigned short));
					decal_project_copy(g, stream->origin);
					sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D,
						count * 3, list, f->cut);
					triangles += count;
				}
			}
		}
	}
	sceGuTexMapMode(GU_TEXTURE_COORDS, 0, 0);
	sceGuDisable(GU_CULL_FACE);
	sceGuDepthFunc(GU_GEQUAL);
	/* the caller's own draw, if it falls back, is given from decal_origin */
	level_model(decal_origin());
	if (triangles)
	{
		decals_projected++;
		decal_projected_triangles += (unsigned long)triangles;
	}
	return triangles;
}

static void psp_ge_decal_body(void const *vertices, int quad_count, unsigned int key, short blend, unsigned long color,
	int intensity, int alpha_tested, int permanent)
{
	const unsigned char *in = vertices;
	struct decal_vertex_ge *out;
	static const int fan[6] = { 0, 1, 2, 0, 2, 3 };
	float i = intensity * (1.0f / 255.0f), c[3];
	int quad, corner, alpha = 0, k;
	unsigned int fragment = 0xFFFFFFFF, constant = 0xFFFFFFFF, fog = fog_color;
	/* build 76: each quad's own slope part is applied as its own pull (one
	depth offset for the draw took its largest quad's: a stencil seen
	edge-on beside the eye gave every decal of its batch, far ones too,
	hundreds of steps; far off that is tens of units); the largest is kept
	for "decalbatch" */
	float decal_units = 0.0f;
	int original_quads = quad_count;
	unsigned int project_start;
	int project_done;
	/* build 106: the first quad kept, as the game gave it (its texture level
	is worked out from it before the pull moves its corners) */
	float level_corners[3][3];
	short level_uv[3][2];
	const float *origin = decal_origin();

	if (!frame_open || !vertices || quad_count <= 0 || quad_count > 256 || intensity <= 0 || list_full())
		return;
	c[0] = ((color >> 16) & 0xFF) * (1.0f / 255.0f);
	c[1] = ((color >> 8) & 0xFF) * (1.0f / 255.0f);
	c[2] = (color & 0xFF) * (1.0f / 255.0f);
	if (decal_pull < 0)
	{
		decal_pull = !psp_keep("decaloffset");
		/* build 55, a test: a nudge four times larger plus 1% of the
		distance. If bullet holes still flicker with it, the cause isn't
		the depth test */
		decal_big = psp_keep("decalbig");
		decal_batch = psp_keep("decalbatch");
	}
	/* build 52: a decal lies on its surface and must win the depth test
	against it. A fixed depth offset (48 steps, build 36) is the wrong size
	at both ends of the 16-bit depth range: near the eye 48 steps are under
	a centimetre (the decal and the ground took turns as the player walked up to
	them), far off they are metres (wall stencils showed through objects in
	front). Each corner now moves toward the eye along its own line of sight
	by about two depth steps at its distance (a step is z^2 / (near x
	61439)): the decal stays exactly where it was on the screen, and is
	always just in front of its surface. "decaloffset": the old way */
	{
		/* build 57: decals smaller than DECAL_MINIMUM_PIXELS across on the
		screen are left out. A distant bullet hole covers a pixel or two, and
		at 272 lines it lit a pixel one frame and none the next as the view
		moved: the blinking the tester saw at a distance (b50-b56; a larger depth
		nudge changed nothing, so it wasn't the depth test). "decalall": all */
		/* build 106: and quads wholly outside the view are left out before
		any of the work below (b30's beach: 6-11 ms a frame in decals by the
		end of the fight, in the emulator, most of it bullet holes behind the
		camera or to the sides); the pull is worked out once a corner, not
		once for each of the two triangles' copies of it. The kept quads come
		out as before.
		Build 106 too: in two passes, which quads are kept first, then the
		texture bound and the list taken for those alone. A decal wholly off
		the screen (most of the 400 calls a frame at b30's beach) no longer
		binds its texture (a texture fetch for nothing, if it wasn't in VRAM)
		or takes display list room for all its quads, which it then left
		unused: decals were 131 KB of b30's 282 KB list a frame */
		static unsigned char kept_quad[256];
		static float kept_units[256];
		int kept = 0, k_quad;
		float half_y = fov_tan_half(), half_x = half_y * (float)SCREEN_WIDTH / SCREEN_HEIGHT;
		float pixels_per_unit = (SCREEN_HEIGHT * 0.5f) / half_y;
		/* a sphere is wholly outside a side plane when its centre is beyond
		it by more than its radius: the planes' normals, unnormalised, have
		these lengths */
		float side_x = sqrtf(1.0f + half_x * half_x), side_y = sqrtf(1.0f + half_y * half_y);
		float per_step = 1.0f / (projection_near * 61439.0f);

		if (decal_all < 0)
			decal_all = psp_keep("decalall");
		if (decal_cull < 0)
			decal_cull = !psp_keep("decalnocull");
		if (decal_slope < 0)
			decal_slope = !psp_keep("decalnoslope");
		DECAL_PROBE(0);
		for (quad = 0; quad < quad_count; quad++)
		{
			const float *p[4];
			float centre[3], camera[3], radius2 = 0.0f, radius, units = 0.0f;

			for (corner = 0; corner < 4; corner++)
				p[corner] = (const float *)(in + 16 * (4 * quad + corner));
			centre[0] = (p[0][0] + p[2][0]) * 0.5f;
			centre[1] = (p[0][1] + p[2][1]) * 0.5f;
			centre[2] = (p[0][2] + p[2][2]) * 0.5f;
			to_camera(centre, camera);
			if (!decal_all)
			{
				float dx = p[2][0] - p[0][0], dy = p[2][1] - p[0][1], dz = p[2][2] - p[0][2];

				if (camera[2] > 0.0f &&
					sqrtf(dx * dx + dy * dy + dz * dz) * pixels_per_unit < DECAL_MINIMUM_PIXELS * camera[2])
				{
					decals_small++;
					continue;
				}
			}
			for (corner = 0; corner < 4; corner++)
			{
				float dx = p[corner][0] - centre[0], dy = p[corner][1] - centre[1], dz = p[corner][2] - centre[2];
				float d2 = dx * dx + dy * dy + dz * dz;

				if (d2 > radius2)
					radius2 = d2;
			}
			radius = sqrtf(radius2) * 1.01f + 0.01f;
			if (!decal_cull)
				;
			else if (camera[2] < -radius ||
				camera[0] - half_x * camera[2] > radius * side_x || -camera[0] - half_x * camera[2] > radius * side_x ||
				camera[1] - half_y * camera[2] > radius * side_y || -camera[1] - half_y * camera[2] > radius * side_y)
			{
				decals_offscreen++;
				continue;
			}
			/* build 65: the quad's own depth slope (as the level's painted
			symbols since b60), the largest of the decal's quads */
			if (decal_slope)
			{
				float q[3][3];

				for (corner = 0; corner < 3; corner++)
					to_camera(p[corner], q[corner]);
				units = slope_units_capped(slope_units(q[0], q[1], q[2]), q[0], q[1], q[2], 0.0f) + nudge_floor;
				if (units > decal_units)
					decal_units = units;
			}
			if (!kept)
				for (corner = 0; corner < 3; corner++)
				{
					memcpy(level_corners[corner], p[corner], 12);
					memcpy(level_uv[corner], (const unsigned char *)p[corner] + 12, 4);
				}
			kept_quad[kept] = (unsigned char)quad;
			kept_units[kept] = units;
			kept++;
		}
		DECAL_PROBE(1);
		if (!kept)
		{
			decals_wholly_left_out++;
			return;
		}
		if (!psp_tex_bind(key, &alpha))
			return;
#ifdef PSP_DEBUG_TOOLS
		if (sdump_wanted())
		{
			float box[4], c[3];
			char text[64];
			int q;

			sdump_box_start(box);
			for (q = 0; q < original_quads * 4; q++)
			{
				to_camera((const float *)(in + 16 * q), c);
				sdump_box_add(box, c);
			}
			if (sdump_box_text(box, text, sizeof(text)))
				platform_log("sdump: decal %s map %s blend %d alpha tested %d map alpha %d permanent %d quads %d",
					text, sdump_map_name(key), blend, alpha_tested, alpha, permanent, original_quads);
		}
#endif
		out = list_take(kept * 6 * sizeof(struct decal_vertex_ge));
		if (!out)
			return;
		DECAL_PROBE(2);
		for (k_quad = 0; k_quad < kept; k_quad++)
		{
			const float *p[4];
			float pulled[4][3];

			for (corner = 0; corner < 4; corner++)
				p[corner] = (const float *)(in + 16 * (4 * kept_quad[k_quad] + corner));
			/* the pull: each corner toward the eye along its line of sight
			(build 52's comment above) */
			for (corner = 0; corner < 4; corner++)
			{
				float d[3] = { p[corner][0] - camera_eye[0], p[corner][1] - camera_eye[1], p[corner][2] - camera_eye[2] };

				memcpy(pulled[corner], p[corner], 12);
				if (decal_pull)
				{
					float length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
					float steps = 2.0f + (decal_batch ? 0.0f : kept_units[k_quad]);
					float shift = decal_big ? 8.0f * length * length * per_step + 0.01f * length + 0.01f :
						steps * length * length * per_step + 0.003f;

					if (length > 2.0f * shift)
					{
						float scale = 1.0f - shift / length;

						pulled[corner][0] = camera_eye[0] + d[0] * scale;
						pulled[corner][1] = camera_eye[1] + d[1] * scale;
						pulled[corner][2] = camera_eye[2] + d[2] * scale;
					}
				}
			}
			for (corner = 0; corner < 6; corner++)
			{
				struct decal_vertex_ge *o = &out[6 * k_quad + corner];

				o->x = pulled[fan[corner]][0] - origin[0];
				o->y = pulled[fan[corner]][1] - origin[1];
				o->z = pulled[fan[corner]][2] - origin[2];
				memcpy(&o->u, (const unsigned char *)p[fan[corner]] + 12, 4);
			}
		}
		quad_count = kept;
		DECAL_PROBE(3);
	}
	{
		/* build 58: the texture level by distance, as for the level (a hole
		on the ground swam with the ground's own texture): from the first
		quad kept, as the game gave it */
		const float *a = level_corners[0], *b = level_corners[1], *c = level_corners[2];
		float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
		float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
		float world_area = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
		float uv_area = fabsf((float)(level_uv[1][0] - level_uv[0][0]) * (float)(level_uv[2][1] - level_uv[0][1]) -
			(float)(level_uv[2][0] - level_uv[0][0]) * (float)(level_uv[1][1] - level_uv[0][1])) *
			(1.0f / 32768.0f) * (1.0f / 32768.0f);
		int width = 0, height = 0;

		psp_tex_size(key, &width, &height);
		texture_levels_by_distance(world_area > 0.0f ? sqrtf(uv_area / world_area) : 0.0f, width, height, 1.0f, tex_bias);
	}
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuDepthMask(GU_TRUE);
	/* build 36: 48 (was 32): bullet holes still sank into walls now and then (b35).
	build 65: with the pull, the slope part on top: a scorch or bullet hole on
	ground seen at a slant lost the depth test across whole triangles
	(b64 shot 40, a scorch cut off along a ground seam; the floor's bullet
	holes flickering while the walls' didn't, b56). "decalnoslope": the pull
	alone */
	/* build 76: the slope part is in each quad's pull now; "decalbatch": the
	b65-b75 single offset for the draw */
	sceGuDepthOffset(decal_pull ? (decal_batch ? (decal_units > 4096.0f ? 4096 : (int)decal_units) : 0) : 48);
	ge_blend(1);
	switch (blend)
	{
	case 1: /* multiply */
	case 2: /* double multiply */
	{
		int r[3], e[3], half = blend == 2;

		for (k = 0; k < 3; k++)
		{
			float f = half ? 0.5f - 0.5f * i * c[k] : 1.0f - i * c[k];
			float n = half ? 0.5f + 0.5f * i * c[k] : 1.0f;

			r[k] = PSP_CLAMP_255((int)(f * 255.0f));
			e[k] = PSP_CLAMP_255((int)(n * 255.0f));
		}
		fragment = 0xFF000000 | (r[2] << 16) | (r[1] << 8) | r[0];
		constant = 0xFF000000 | (e[2] << 16) | (e[1] << 8) | e[0];
		/* an alpha-tested decal (a10's floor markings) is cut out by its
		map's alpha on the Xbox (rasterizer_xbox_decals.c: the alpha test
		reads r0.a, the texel's); with the texel's alpha left out the test
		saw the fragment's 0xFF and drew the whole quad as a square. The
		multiply blends read no alpha, so the colour is unchanged.
		"decalsquare": as before */
		if (decal_square < 0)
			decal_square = psp_keep("decalsquare");
		sceGuTexFunc(GU_TFX_BLEND, alpha_tested && alpha && !decal_square ? GU_TCC_RGBA : GU_TCC_RGB);
		sceGuTexEnvColor(constant);
		/* the frame times twice this, or this */
		if (half)
			sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_SRC_COLOR, 0, 0);
		else
			sceGuBlendFunc(GU_ADD, GU_DST_COLOR, GU_FIX, 0, 0);
		/* far off, fog leaves the frame as it is */
		fog = half ? 0xFF808080 : 0xFFFFFFFF;
		break;
	}
	default:
	{
		int r = PSP_CLAMP_255((int)(c[0] * 255.0f)), g = PSP_CLAMP_255((int)(c[1] * 255.0f)),
			b = PSP_CLAMP_255((int)(c[2] * 255.0f)), a = PSP_CLAMP_255(intensity);

		if (blend == 3 || blend == 7)
		{
			/* added light: scaled by the intensity, and no fog colour on top */
			r = r * intensity / 255; g = g * intensity / 255; b = b * intensity / 255;
			fog = 0xFF000000;
		}
		fragment = ((unsigned int)a << 24) | (b << 16) | (g << 8) | r;
		sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
		if (blend == 3)
			sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
		else
			hud_blend(blend);
		break;
	}
	}
	if (alpha_tested && alpha)
	{
		sceGuAlphaFunc(GU_GREATER, 0x7F, 0xFF);
		sceGuEnable(GU_ALPHA_TEST);
	}
	sceGuFog(fog_near, fog_far, fog);
	sceGuColor(fragment);
	level_model(origin);
	if (permanent && decal_project && level_compact <= 0 &&
		(project_start = decal_clock(), project_done = decal_project_draw(in, original_quads),
		decal_project_us += (decal_clock() - project_start) * DECAL_TIMED_ONE_IN, project_done))
		;
	else
	{
		if (permanent && decal_project)
			decals_project_fallback++;
		sceGumDrawArray(GU_TRIANGLES, GU_TEXTURE_16BIT | GU_VERTEX_32BITF | GU_TRANSFORM_3D, quad_count * 6, NULL, out);
	}
	world_model();
	sceGuTexLevelMode(tex_level_mode, tex_bias);
	sceGuFog(fog_near, fog_far, fog_color);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuTexEnvColor(0xFFFFFFFF);
	ge_blend(0);
	sceGuDepthOffset(0);
	sceGuDepthMask(GU_FALSE);
	sceGuDisable(GU_TEXTURE_2D);
	decals_drawn++;
	DECAL_PROBE(4);
}

void psp_ge_decal(void const *vertices, int quad_count, unsigned int key, short blend, unsigned long color,
	int intensity, int alpha_tested, int permanent)
{
	GE_PRETOUCH(key, TXPT_DECAL);
	GE_DEFER(defer_decal(vertices, quad_count, key, blend, color, intensity, alpha_tested, permanent)); /* build 112: gedefer */
	GE_BENCH_SKIP();
	static unsigned int decal_calls = 0;

	ge_kind(KIND_DECAL); /* build 103 */
	/* build 106: one call in DECAL_TIMED_ONE_IN timed (decal_clock) */
	decal_timed = (decal_calls++ % DECAL_TIMED_ONE_IN) == 0;
	if (decal_timed)
	{
		unsigned int start = decal_clock(), took;

		decal_probe_mark = start; /* build 119's probe */

		psp_ge_decal_body(vertices, quad_count, key, blend, color, intensity, alpha_tested, permanent);
		took = (decal_clock() - start) * DECAL_TIMED_ONE_IN;
		ge_entry_us[5] += took;
		ge_part_us[10] += took;
		decal_timed = 0;
	}
	else
		psp_ge_decal_body(vertices, quad_count, key, blend, color, intensity, alpha_tested, permanent);
	ge_part_calls[10]++;
}

/* ---------- the motion sensor (build 23)

The Xbox draws the sensor into a small target of its own: the blips added
on black, the sweep on top (its colour added, what was there kept by its
alpha), everything outside the round mask cut away, and the target then
added to the HUD over the sensor's background. The GE does the same with a
64 x 64 target at the start of VRAM's texture space (16 KB, kept out of the
texture cache; psp_tex.c). The sensor's square runs from -1 to 1. */

#define BLIP_TARGET 0x000CC000
#define BLIP_SIZE 64
#define BLIP_BYTES (BLIP_SIZE * BLIP_SIZE * 4)
static int blips_open = 0;
static unsigned long blips_drawn = 0;

/* "blipdepth" (docs/review_vrbroom.md): the target drawn in the depth
buffer's first 16 KB (the top 16 rows' depth) instead, so the 16 KB at
BLIP_TARGET join the room the textures and the copies share (psp_tex.c).
The depth buffer is idle there: the sensor is drawn in the HUD's turn, after
the world's last depth test, every HUD draw has the test off, the target is
drawn black from scratch before its blips, and the next frame's clear wipes
it before the world tests against it.
"blipdepthcheck": the target in the same place, BLIP_TARGET kept, and drawn
again there after the composite, so the frame's end can compare the two:
whether the GE wrote anything of the 3D pass's depth over the target after
it was drawn. The emulator has no GE caches; that question is the PSP's */
#define BLIP_TARGET_DEPTH (2 * FRAME_BYTES)
#define BLIP_CHECK_MOST 64
#define BLIP_CHECK_FILL 0x5AA5C33Cu /* what the check leaves at BLIP_TARGET: still there means no sensor that frame */
static unsigned int blip_target = BLIP_TARGET;
static int blip_check = 0;
/* the frame's blips, for the check's second drawing (on the CPU that draws
the first) */
static struct { float x, y, radius, red, green, blue; unsigned int key; } blip_record[BLIP_CHECK_MOST];
static int blip_record_count = 0, blip_record_lost = 0;
static unsigned long blip_check_frames = 0, blip_check_differ = 0, blip_check_words = 0, blip_check_lost = 0;
/* the first difference: its byte in the target, the frame, and the two words */
static unsigned int blip_check_first = 0xFFFFFFFFu, blip_check_first_depth = 0, blip_check_first_reference = 0;
static unsigned long blip_check_first_frame = 0;
/* draws of the world's kinds after the target was drawn in the depth
buffer, in that frame: none is expected (ge_kind) */
static int blip_depth_live = 0;
static unsigned long blip_depth_world_after = 0;

/* set as the GE starts; psp_tex.c asks whether the 16 KB are free */
static void blip_place(void)
{
	blip_check = psp_keep("blipdepthcheck") != 0;
	blip_target = blip_check || psp_keep("blipdepth") ? BLIP_TARGET_DEPTH : BLIP_TARGET;
	if (blip_check)
	{
		volatile unsigned int *reference = (volatile unsigned int *)(0x44000000 + BLIP_TARGET);
		int k;

		for (k = 0; k < BLIP_BYTES / 4; k++)
			reference[k] = BLIP_CHECK_FILL;
	}
	if (blip_check)
		platform_log("GE: blipdepthcheck: the motion sensor's target in the depth buffer at %06x, drawn at %06x too and compared at each frame's end",
			blip_target, BLIP_TARGET);
	else if (blip_target != BLIP_TARGET)
		platform_log("GE: blipdepth: the motion sensor's target in the depth buffer at %06x", blip_target);
}

int psp_ge_blip_room_freed(void)
{
	return psp_keep("blipdepth") && !psp_keep("blipdepthcheck");
}

struct blip_vertex
{
	float u, v;
	unsigned int color;
	float x, y, z;
};

/* a quad on the target: the corners of the sensor's square, x right and y up */
static void blip_quad(const float *uv, const float *xy, unsigned int color)
{
	struct blip_vertex *out = list_take(4 * sizeof(struct blip_vertex));
	int corner;

	if (!out)
		return;

	for (corner = 0; corner < 4; corner++)
	{
		out[corner].u = uv[2 * corner];
		out[corner].v = uv[2 * corner + 1];
		out[corner].color = color;
		out[corner].x = (xy[2 * corner] + 1.0f) * (BLIP_SIZE / 2);
		out[corner].y = (1.0f - xy[2 * corner + 1]) * (BLIP_SIZE / 2);
		out[corner].z = 0.0f;
	}
	sceGuDrawArray(GU_TRIANGLE_FAN, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 4, NULL, out);
}

/* texture coordinates in the 2D mode are in texels */
static int blip_texture(unsigned int key, float *uv)
{
	int width, height, alpha, corner;

	if (!key || !psp_tex_size(key, &width, &height) || !psp_tex_bind(key, &alpha))
		return 0;
	psp_tex_hud_note(key);
	for (corner = 0; corner < 4; corner++)
	{
		uv[2 * corner] *= width;
		uv[2 * corner + 1] *= height;
	}
	return 1;
}

static unsigned int blip_color(float red, float green, float blue)
{
	int r = (int)(red * 255.0f), g = (int)(green * 255.0f), b = (int)(blue * 255.0f);

	return 0xFF000000 | (PSP_CLAMP_255(b) << 16) | (PSP_CLAMP_255(g) << 8) | PSP_CLAMP_255(r);
}

/* the target made the GE's frame, and black */
static void blip_target_open(unsigned int target)
{
	static const float everywhere[8] = { -1.0f, 1.0f, 1.0f, 1.0f, 1.0f, -1.0f, -1.0f, -1.0f };
	static const float nothing[8] = { 0 };

	sceGuDrawBufferList(GU_PSM_8888, (void *)target, BLIP_SIZE);
	sceGuOffset(2048 - BLIP_SIZE / 2, 2048 - BLIP_SIZE / 2);
	sceGuViewport(2048, 2048, BLIP_SIZE, BLIP_SIZE);
	sceGuScissor(0, 0, BLIP_SIZE, BLIP_SIZE);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_FOG);
	ge_blend(0);
	sceGuDisable(GU_TEXTURE_2D);
	/* black, drawn rather than cleared (a clear covers the whole screen's
	size, past the target's end) */
	blip_quad(nothing, everywhere, 0xFF000000);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	ge_blend(1);
}

void psp_ge_blips_begin(void)
{
	GE_DEFER(defer_words(DEFER_BLIPS_BEGIN, 0, 0, 0, 0, 0, 0, 0, 0, 0));
	ge_kind(KIND_HUD);
	list_hud_phase();

	if (!frame_open || list_full())
		return;
	/* the world's see-through parts belong to the frame, not the target */
	transparent_flush();
	blip_target_open(blip_target);
	blip_depth_live = blip_target != BLIP_TARGET;
	blip_record_count = blip_record_lost = 0;
	blips_open = 1;
}

static int blip_draw(float x, float y, float radius, float red, float green, float blue, unsigned int key)
{
	float uv[8] = { 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
	float xy[8] = { x - radius, y + radius, x + radius, y + radius, x + radius, y - radius, x - radius, y - radius };

	if (list_full() || !blip_texture(key, uv))
		return 0;
	/* added light */
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
	blip_quad(uv, xy, blip_color(red, green, blue));
	return 1;
}

void psp_ge_blip(float x, float y, float radius, float red, float green, float blue, unsigned int key)
{
	GE_PRETOUCH_HUD(key, TXPT_BLIP);
	GE_DEFER(defer_words(DEFER_BLIP, 7, defer_bits(x), defer_bits(y), defer_bits(radius), defer_bits(red),
		defer_bits(green), defer_bits(blue), key, 0));
	GE_BENCH_SKIP();
	ge_kind(KIND_HUD);

	if (!blips_open || !blip_draw(x, y, radius, red, green, blue, key))
		return;
	blips_drawn++;
	if (blip_check)
	{
		if (blip_record_count < BLIP_CHECK_MOST)
		{
			blip_record[blip_record_count].x = x;
			blip_record[blip_record_count].y = y;
			blip_record[blip_record_count].radius = radius;
			blip_record[blip_record_count].red = red;
			blip_record[blip_record_count].green = green;
			blip_record[blip_record_count].blue = blue;
			blip_record[blip_record_count].key = key;
			blip_record_count++;
		}
		else
			blip_record_lost = 1;
	}
}

/* the sweep and the mask over the target's blips */
static void blip_target_close(float scale, unsigned int sweep_key, unsigned int mask_key)
{
	static const float square[8] = { -1.015625f, 1.046875f, 1.046875f, 1.046875f, 1.046875f, -1.015625f, -1.015625f, -1.015625f };

	{
		/* the sweep: its colour added, the blips under it kept by its alpha */
		float low = 0.5f - scale * 0.5f, high = 0.5f + scale * 0.5f;
		float uv[8] = { high, low, low, low, low, high, high, high };

		if (!list_full() && blip_texture(sweep_key, uv))
		{
			sceGuBlendFunc(GU_ADD, GU_FIX, GU_SRC_ALPHA, 0xFFFFFF, 0);
			blip_quad(uv, square, blip_color(0.4588f, 0.7294f, 1.0f));
		}
	}
	{
		/* the mask: what is outside the round sensor goes */
		float uv[8] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f };

		if (!list_full() && blip_texture(mask_key, uv))
		{
			sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
			sceGuBlendFunc(GU_ADD, GU_FIX, GU_SRC_ALPHA, 0, 0);
			blip_quad(uv, square, 0xFFFFFFFF);
		}
	}
}

static void blip_frame_back(void)
{
	sceGuDrawBufferList(GU_PSM_5650, draw_buffer, BUFFER_WIDTH);
	sceGuOffset(2048 - SCREEN_WIDTH / 2, 2048 - SCREEN_HEIGHT / 2);
	sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
	sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
}

/* blipdepthcheck: the frame's target drawn again at BLIP_TARGET, after the
composite has read the one in the depth buffer (the picture is blipdepth's) */
static void blip_check_draw(float scale, unsigned int sweep_key, unsigned int mask_key)
{
	int k;

	if (blip_record_lost || list_full())
	{
		blip_check_lost++;
		return;
	}
	blip_target_open(BLIP_TARGET);
	for (k = 0; k < blip_record_count; k++)
		blip_draw(blip_record[k].x, blip_record[k].y, blip_record[k].radius, blip_record[k].red, blip_record[k].green,
			blip_record[k].blue, blip_record[k].key);
	blip_target_close(scale, sweep_key, mask_key);
	blip_frame_back();
	/* the texture state the composite left, so the HUD's draws after it find
	what they find without the check */
	sceGuTexMode(GU_PSM_8888, 0, 0, 0);
	sceGuTexImage(0, BLIP_SIZE, BLIP_SIZE, BLIP_SIZE, (void *)(0x04000000 + blip_target));
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
}

/* blipdepthcheck, at the frame's end with the GE done: the two targets
compared word for word through the uncached view, and BLIP_TARGET filled so
a frame without the sensor shows as one */
static void blip_check_compare(void)
{
	volatile const unsigned int *depth = (volatile const unsigned int *)(0x44000000 + BLIP_TARGET_DEPTH);
	volatile unsigned int *reference = (volatile unsigned int *)(0x44000000 + BLIP_TARGET);
	unsigned int k, differ = 0;

	if (reference[0] == BLIP_CHECK_FILL && reference[BLIP_BYTES / 8] == BLIP_CHECK_FILL &&
		reference[BLIP_BYTES / 4 - 1] == BLIP_CHECK_FILL)
		return;
	/* colour only: the target's alpha bytes are the GE's stencil, which
	these draws leave as they were (the depth's bytes in one, the fill in the
	other); the composite reads the colour alone (GU_TCC_RGB, fixed blend) */
	for (k = 0; k < BLIP_BYTES / 4; k++)
		if ((depth[k] ^ reference[k]) & 0x00FFFFFFu)
		{
			if (!differ && !blip_check_differ)
			{
				blip_check_first = k * 4;
				blip_check_first_depth = depth[k];
				blip_check_first_reference = reference[k];
				blip_check_first_frame = frames;
			}
			differ++;
		}
	blip_check_frames++;
	if (differ)
	{
		blip_check_differ++;
		blip_check_words += differ;
	}
	for (k = 0; k < BLIP_BYTES / 4; k++)
		reference[k] = BLIP_CHECK_FILL;
}

void psp_ge_blips_end(float center_x, float center_y, float scale, unsigned int sweep_key, unsigned int mask_key)
{
	GE_PRETOUCH_HUD(sweep_key, TXPT_BLIPS_END);
	GE_PRETOUCH_HUD(mask_key, TXPT_BLIPS_END);
	GE_DEFER(defer_words(DEFER_BLIPS_END, 5, defer_bits(center_x), defer_bits(center_y), defer_bits(scale), sweep_key,
		mask_key, 0, 0, 0));
	ge_kind(KIND_HUD);

	if (!blips_open)
		return;
	blips_open = 0;
	blip_target_close(scale, sweep_key, mask_key);
	/* back to the frame */
	blip_frame_back();
	if (!list_full())
	{
		/* the target, added over the sensor (42 of the game's pixels either
		side of its centre) */
		struct hud_vertex *out = list_take(2 * sizeof(struct hud_vertex));
		const float radius = 42.0f;

		if (out)
		{

		sceGuTexFlush();
		sceGuTexMode(GU_PSM_8888, 0, 0, 0);
		sceGuTexImage(0, BLIP_SIZE, BLIP_SIZE, BLIP_SIZE, (void *)(0x04000000 + blip_target));
		sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
		sceGuTexFilter(GU_LINEAR, GU_LINEAR);
		sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, 0xFFFFFF, 0xFFFFFF);
		out[0].u = 0.0f; out[0].v = 0.0f;
		out[1].u = BLIP_SIZE; out[1].v = BLIP_SIZE;
		out[0].color = out[1].color = 0xFFFFFFFF;
		out[0].x = (center_x - radius) * (SCREEN_WIDTH / GAME_SCREEN_WIDTH) * hud_zoom;
		out[0].y = (center_y - radius) * (SCREEN_HEIGHT / GAME_SCREEN_HEIGHT) * hud_zoom;
		out[1].x = (center_x + radius) * (SCREEN_WIDTH / GAME_SCREEN_WIDTH) * hud_zoom;
		out[1].y = (center_y + radius) * (SCREEN_HEIGHT / GAME_SCREEN_HEIGHT) * hud_zoom;
		out[0].z = out[1].z = 0.0f;
		sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, NULL, out);
		}
	}
	if (blip_check)
		blip_check_draw(scale, sweep_key, mask_key);
	ge_blend(0);
	sceGuDisable(GU_TEXTURE_2D);
	sceGuEnable(GU_FOG);
	sceGuEnable(GU_DEPTH_TEST);
}

/* ---------- text (build 23)

The game draws text a character at a time from a 128 x 128 picture of
recently used characters it keeps itself (rasterizer_text.c): 16-bit,
4 bits each of alpha and colour, the way the GE reads GU_PSM_4444 too, in
plain rows in main memory. A character's pixels are written just before
it is drawn, so each draw first writes those rows out of the CPU's cache
and has the GE forget what it read of the picture before. */

static int text_open = 0;
static unsigned short *text_pixels;
static int text_width, text_height;
static float text_offset_x, text_offset_y;
static unsigned long text_characters = 0;
/* build 44: the picture changes only when the game adds a character to it
(rasterizer_text.c's cache_hardware_format_character, hooked by
psp_fixups.py), a few times a minute; until build 43 every drawn character
wrote its rows back and had the GE forget the picture: ~750 cache calls a
frame for a 60-character HUD */
static int text_changed = 1;
static unsigned long text_changes = 0;

void psp_ge_text_changed(void const *bitmap, int x, int y, int width, int height)
{
	GE_DEFER_DRAIN(); /* build 112: gedefer */
	show2_settle(SHOW2_OTHER); /* geshow2: the frame being shown may still sample the old glyphs */
	const unsigned char *data = bitmap;
	int bitmap_width = *(const short *)(data + 4), bitmap_height = *(const short *)(data + 6), row;
	unsigned short *pixels = *(unsigned short *const *)(data + 44);

	if (!pixels || bitmap_width <= 0 || x < 0 || y < 0 || width <= 0)
		return;
	if (x + width > bitmap_width)
		width = bitmap_width - x;
	if (y + height > bitmap_height)
		height = bitmap_height - y;
	/* whole cache lines: the row's first and last lines are shared with
	the characters either side, which the game wrote earlier */
	for (row = 0; row < height; row++)
	{
		unsigned int start = (unsigned int)(pixels + (y + row) * bitmap_width + x) & ~63u;
		unsigned int end = ((unsigned int)(pixels + (y + row) * bitmap_width + x + width) + 63) & ~63u;

		ge_writeback((void *)start, end - start);
	}
	text_changed = 1;
	text_changes++;
}

int psp_ge_text_begin(void const *bitmap, short blend, float offset_x, float offset_y)
{
	/* build 112: gedefer. The engine needs the answer now (whether to send
	the characters), so it is worked out here from what the main CPU knows:
	the frame and the font's picture. Whether the list has room is the
	carrying out's to judge: if it hasn't, the text stays closed then and
	the characters written down after this are dropped there, as the engine
	would have left them out. The same list either way */
	if (DEFER_RECORDING() && !defer_paused)
	{
		const unsigned char *picture = bitmap;
		/* geinflight: the frame's start may be written down and not yet
		carried out (frame_open is then still shut from the last frame's
		show); it will be open when this is carried out */
		int open = frame_open || (inflight_window_written && !inflight_text_old);
		int answer = open && picture && *(void *const *)(picture + 44) && *(const short *)(picture + 12) == 9 &&
			*(const short *)(picture + 4) > 0 && *(const short *)(picture + 6) > 0 &&
			*(const short *)(picture + 4) <= 512 && *(const short *)(picture + 6) <= 512;

		defer_words(DEFER_TEXT_BEGIN, 4, DEFER_P(bitmap), (unsigned int)blend, defer_bits(offset_x), defer_bits(offset_y),
			0, 0, 0, 0);
		if (answer && !frame_open)
			inflight_text_early++;
		return answer;
	}
	ge_kind(KIND_HUD); /* build 103 */
	list_hud_phase();
	/* the game's bitmap_data: width +4, height +6, format +12, pixels +44 */
	const unsigned char *data = bitmap;
	int width = *(const short *)(data + 4), height = *(const short *)(data + 6), format = *(const short *)(data + 12);
	void *pixels = *(void *const *)(data + 44);

	if (!frame_open || !pixels || format != 9 /* A4R4G4B4 */ || width <= 0 || height <= 0 || width > 512 ||
		height > 512 || list_full())
		return 0;
	transparent_flush();
	if (text_pixels != pixels)
		text_changed = 1;
	text_width = width;
	text_height = height;
	text_offset_x = offset_x;
	text_offset_y = offset_y;
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_FOG);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexMode(GU_PSM_4444, 0, 0, 0);
	sceGuTexImage(0, width, height, width, pixels);
	sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	hud_blend(blend);
	text_pixels = pixels;
	text_open = 1;
	return 1;
}

/* four corners: position, texture coordinates in texels, colour (ARGB) */
static void text_character(void const *vertices);

void psp_ge_text_character(void const *vertices)
{
	GE_DEFER(defer_text_character(vertices)); /* build 112: gedefer */
	GE_BENCH_SKIP();
	ge_kind(KIND_HUD); /* build 103 */
	unsigned int start = ge_clock();

	text_character(vertices);
	text_us += ge_clock() - start;
}

static void text_character(void const *vertices)
{
	struct game_screen_vertex const *in = vertices;
	struct hud_vertex *out;
	int corner;

	if (!text_open || list_full())
		return;
	if (text_changed)
	{
		/* the GE's own 8 KB texture cache may hold the old picture */
		sceGuTexFlush();
		text_changed = 0;
	}
	out = list_take(4 * sizeof(struct hud_vertex));
	if (!out)
		return;
	for (corner = 0; corner < 4; corner++)
	{
		out[corner].u = in[corner].u;
		out[corner].v = in[corner].v;
		out[corner].color = argb_to_abgr(in[corner].argb, 1.0f, 1.0f, 1.0f, 1.0f);
		out[corner].x = (in[corner].x + text_offset_x) * ui_scale_x * hud_zoom + ui_offset_x;
		out[corner].y = (in[corner].y + text_offset_y) * ui_scale_y * hud_zoom + ui_offset_y;
		out[corner].z = 0.0f;
	}
	sceGuDrawArray(GU_TRIANGLE_FAN, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 4, NULL, out);
	text_characters++;
}

void psp_ge_text_end(void)
{
	GE_DEFER(defer_words(DEFER_TEXT_END, 0, 0, 0, 0, 0, 0, 0, 0, 0)); /* build 112: gedefer */
	if (!text_open)
		return;
	text_open = 0;
	ge_blend(0);
	sceGuDisable(GU_TEXTURE_2D);
	sceGuEnable(GU_FOG);
	sceGuEnable(GU_DEPTH_TEST);
}

void psp_tex_frame_end(void);
void psp_geo_frame_end(void);

/* the screenshot button (build 34; select + right in play): the frame just
drawn, read from VRAM before it is shown, as a 480 x 272 BMP in
ms0:/halo/shots. The PSP's own pixels, with no camera in the way */
volatile int psp_screenshot_requested = 0;

/* build 92: each shot stamped with the build, the run's seconds (the T of
the overlay and the log's [t ...]) and the frame, burned into the bottom
centre and put in the file name (shot004_t108.bmp). Not the PSP's clock:
the test PSP's is unreliable. A 5 x 7 font of the characters it needs */
static const unsigned char stamp_font[][7] = {
	{ 14, 17, 19, 21, 25, 17, 14 }, { 4, 12, 4, 4, 4, 4, 14 }, { 14, 17, 1, 2, 4, 8, 31 },
	{ 31, 2, 4, 2, 1, 17, 14 }, { 2, 6, 10, 18, 31, 2, 2 }, { 31, 16, 30, 1, 1, 17, 14 },
	{ 6, 8, 16, 30, 17, 17, 14 }, { 31, 1, 2, 4, 8, 8, 8 }, { 14, 17, 17, 14, 17, 17, 14 },
	{ 14, 17, 17, 15, 1, 2, 12 },
	{ 16, 16, 22, 25, 17, 17, 30 },	/* b */
	{ 8, 8, 28, 8, 8, 9, 6 },	/* t */
	{ 6, 9, 8, 28, 8, 8, 8 },	/* f */
	{ 0, 0, 0, 0, 0, 12, 12 },	/* . */
	{ 0, 0, 0, 0, 0, 0, 0 },	/* space */
	{ 0, 0, 30, 17, 30, 16, 16 },	/* p */
	{ 0, 4, 4, 31, 4, 4, 0 },	/* + */
};
#define STAMP_HEIGHT 11

static int stamp_glyph(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	return c == 'b' ? 10 : c == 't' ? 11 : c == 'f' ? 12 : c == '.' ? 13 : c == 'p' ? 15 : c == '+' ? 16 : 14;
}

/* 1: white text, 2: the black box round it, 0: the picture */
static unsigned char stamp_mask[STAMP_HEIGHT][SCREEN_WIDTH];

static void stamp_build(const char *text)
{
	int length = (int)strlen(text), width = length * 6 + 3, left = (SCREEN_WIDTH - width) / 2, i, y, x;

	memset(stamp_mask, 0, sizeof(stamp_mask));
	for (y = 0; y < STAMP_HEIGHT; y++)
		for (x = left; x < left + width && x < SCREEN_WIDTH; x++)
			stamp_mask[y][x] = 2;
	for (i = 0; i < length; i++)
	{
		const unsigned char *glyph = stamp_font[stamp_glyph(text[i])];

		for (y = 0; y < 7; y++)
			for (x = 0; x < 5; x++)
				if (glyph[y] & (16 >> x))
					stamp_mask[2 + y][left + 2 + 6 * i + x] = 1;
	}
}

static void screenshot(void);

/* ---------- the loading screen (psp_loading.c drives it; psp_loading.h)

The Xbox's loading picture (progress_bar.c, progress_bar_render), as far as
it can be drawn without its loading.tga:
- each frame starts black and the frame on the screen is drawn back over it
  through a 4-tap blur, a little larger than the screen (texels 3 to 637 of
  640 across a quad 1/128 wider and taller than the screen on each side) and
  at 0.9, so the last picture zooms out and fades, a trail; above 0.9 of the
  progress the trail's weight falls to 0 by the end;
- over that, loading.tga times a soft band (a 128 x 16 mask, made in code)
  that sweeps from the left to the right with the progress, at the
  brightness sin(progress pi)^0.9, added. loading.tga is the disc's
  (maps\loading.tga, which the Xbox copies to its utility drive at start-up);
  the bake doesn't carry it yet, so that layer isn't drawn.
On the PSP the blur's taps are 0.1 texel apart, so one bilinear tap stands
for them [U]. The trail's source is whichever buffer is on the screen: the
loading frames swap themselves, so each reads the one before; in the settle
(the level's first view drawn with no tick) the veil draws the same trail
opaque over the scene, so the view stays hidden and the screen ends on black,
as the Xbox's did. "loadingplain": black frames, no trail; "loadshotsN": a
screenshot every Nth loading frame (tests). */
#include "psp_loading.h"

int psp_keep_number(const char *prefix, int fallback);
static int loading_on = -1, loading_shots = -1;
static unsigned long loading_frames = 0;

static unsigned int loading_front(void)
{
	/* the buffer on the screen: the one not being drawn */
	return (unsigned int)draw_buffer == 0 ? FRAME_BYTES : 0;
}

/* the trail: the screen's picture, zoomed out a little and weighted, over
nothing (src x weight + dst x 0), so it is opaque and needs no clear */
static void loading_trail(float progress, int from_screen)
{
	struct trail_vertex { float u, v, x, y, z; } *v;
	float weight = progress < 0.9f ? 0.9f : 0.9f - (progress - 0.9f) * 9.0f;
	unsigned int fixed;
	int strip, strips = SCREEN_WIDTH / 32;

	if (loading_on < 0)
	{
		loading_on = !psp_keep("loadingplain");
		loading_shots = psp_keep_number("loadshots", 0);
	}
	if (weight < 0.0f || !from_screen || !loading_on)
		weight = 0.0f;
	fixed = (unsigned int)(weight * 255.0f + 0.5f);
	v = sceGuGetMemory(strips * 2 * sizeof(*v));
	/* the Xbox's mapping, in the PSP's pixels: x' = 2.25 + (x + 1.875) x
	634/645 and y' = (y + 1.0625) x 480/483.75 (its 640 x 480 scaled to 480
	x 272), so the picture grows 1.7% across and 0.8% down a frame */
	for (strip = 0; strip < strips; strip++)
	{
		float x0 = (float)(strip * 32), x1 = x0 + 32.0f;

		v[2 * strip].u = 2.25f + (x0 + 1.875f) * (634.0f / 645.0f);
		v[2 * strip].v = 1.0625f * (480.0f / 483.75f);
		v[2 * strip].x = x0;
		v[2 * strip].y = 0.0f;
		v[2 * strip].z = 0.0f;
		v[2 * strip + 1].u = 2.25f + (x1 + 1.875f) * (634.0f / 645.0f);
		v[2 * strip + 1].v = (SCREEN_HEIGHT + 1.0625f) * (480.0f / 483.75f);
		v[2 * strip + 1].x = x1;
		v[2 * strip + 1].y = (float)SCREEN_HEIGHT;
		v[2 * strip + 1].z = 0.0f;
	}
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_TRUE);
	sceGuDisable(GU_FOG);
	sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuTexMode(GU_PSM_5650, 0, 0, 0);
	sceGuTexImage(0, 512, 512, BUFFER_WIDTH, (void *)((unsigned int)sceGeEdramGetAddr() + loading_front()));
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGB);
	sceGuTexWrap(GU_CLAMP, GU_CLAMP);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
	sceGuTexLevelMode(GU_TEXTURE_CONST, 0.0f);
	sceGuTexFlush();
	ge_blend(1);
	/* undithered: the trail feeds on itself, and a dither would build up */
	dither_set(0);
	sceGuBlendFunc(GU_ADD, GU_FIX, GU_FIX, fixed | (fixed << 8) | (fixed << 16), 0);
	sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, strips * 2, NULL, v);
	/* as the frame's drawing expects them (buffer_state(0)) */
	sceGuTexLevelMode(tex_level_mode, tex_bias);
	ge_blend(0);
	sceGuEnable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_FALSE);
	sceGuEnable(GU_FOG);
	loading_frames++;
}


/* What the Xbox's picture can't show without the bake's loading.tga: the
progress, as a thin bar low on the screen, and at boot a line of text
(view->notice). From the menu the menu has faded to black before the load,
so without them a load is a black screen. Each is drawn over a black patch
a little larger than itself: the trail carries the frame before, and without
the patch the bar and the text would trail out behind themselves (the trail
grows 1.7% a frame about the screen's centre: 2 pixels a frame at the bar's
ends). With the picture its sweeping band is the Xbox's own progress, so the
bar stays off: loading_with_picture is for the renderer's loadingband to set
when a load's picture is read. "loadingnobar": no bar. */
static int loading_with_picture = 0;
static int loading_bar_on = -1;
/* pspdebug's 8 x 8 font, linked already for the on-screen log */
extern unsigned char msx[];

#define LOADING_BAR_WIDTH 160
#define LOADING_BAR_Y 252
#define LOADING_NOTICE_Y 234
#define LOADING_NOTICE_MOST (SCREEN_WIDTH / 8)
/* the boot screen's display list (psp_ge_boot_start, below): 64 KB of
benchrun's VRAM region, where a frame with loadingband's band takes ~33 KB */
#define BOOT_LIST_AT VRAM_BENCH_START

static void loading_patch(int x0, int y0, int x1, int y1, unsigned int abgr)
{
	struct patch_vertex { unsigned int color; short x, y, z, pad; } *v = sceGuGetMemory(2 * sizeof(*v));

	v[0].color = v[1].color = abgr;
	v[0].x = (short)x0;
	v[0].y = (short)y0;
	v[1].x = (short)x1;
	v[1].y = (short)y1;
	v[0].z = v[1].z = v[0].pad = v[1].pad = 0;
	/* the real call: list memory has no VRAM copy to look up, and a test's
	draw skips mustn't take the screen's text away */
	(sceGuDrawArray)(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_16BIT | GU_TRANSFORM_2D, 2, NULL, v);
}

/* the text as a 512 x 8 4-bit texture in the list (2 KB, made each frame:
no VRAM or heap kept for it), drawn 1:1 and centred. In 4-bit texels a word
is 8 texels, the first in the lowest bits, so a glyph's row is one word */
static void loading_notice(const char *text, int y)
{
	struct notice_vertex { float u, v, x, y, z; } *v;
	unsigned int *clut, *texels;
	int length = (int)strlen(text), left, row, k, bit;

	if (length > LOADING_NOTICE_MOST)
		length = LOADING_NOTICE_MOST;
	if (length <= 0)
		return;
	left = (SCREEN_WIDTH - length * 8) / 2;
	clut = (unsigned int *)(((unsigned int)sceGuGetMemory(64 + 64 + 2048) + 63) & ~63u);
	texels = clut + 16;
	clut[0] = 0x00000000;
	for (k = 1; k < 16; k++)
		clut[k] = 0xFFFFFFFF;
	for (row = 0; row < 8; row++)
		for (k = 0; k < 64; k++)
		{
			/* the font's bit 7 is a glyph's left column */
			unsigned int glyph_row = k < length ? msx[(unsigned char)text[k] * 8 + row] : 0, word = 0;

			for (bit = 0; bit < 8; bit++)
				if (glyph_row & (128u >> bit))
					word |= 1u << (4 * bit);
			texels[row * 64 + k] = word;
		}
	loading_patch(left - 6, y - 4, left + length * 8 + 6, y + 12, 0xFF000000);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuClutMode(GU_PSM_8888, 0, 0x0F, 0);
	sceGuClutLoad(2, clut);
	sceGuTexMode(GU_PSM_T4, 0, 0, 0);
	sceGuTexImage(0, 512, 8, 512, texels);
	sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
	sceGuTexFilter(GU_NEAREST, GU_NEAREST);
	sceGuTexFlush();
	sceGuEnable(GU_ALPHA_TEST);
	sceGuAlphaFunc(GU_GREATER, 0, 0xFF);
	v = sceGuGetMemory(2 * sizeof(*v));
	v[0].u = 0.0f;
	v[0].v = 0.0f;
	v[0].x = (float)left;
	v[0].y = (float)y;
	v[1].u = (float)(length * 8);
	v[1].v = 8.0f;
	v[1].x = (float)(left + length * 8);
	v[1].y = (float)(y + 8);
	v[0].z = v[1].z = 0.0f;
	(sceGuDrawArray)(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, NULL, v);
	sceGuDisable(GU_ALPHA_TEST);
	sceGuTexFilter(GU_LINEAR, GU_LINEAR);
}

/* after loading_trail, in the same state it leaves */
static void loading_overlay(const struct psp_loading_view *view)
{
	int bar, x0 = (SCREEN_WIDTH - LOADING_BAR_WIDTH) / 2;

	if (loading_bar_on < 0)
		loading_bar_on = !psp_keep("loadingnobar");
	bar = loading_bar_on && !loading_with_picture;
	if (!bar && !view->notice)
		return;
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_TRUE);
	sceGuDisable(GU_FOG);
	sceGuDisable(GU_TEXTURE_2D);
	if (bar)
	{
		float progress = view->progress < 0.0f ? 0.0f : view->progress > 1.0f ? 1.0f : view->progress;

		loading_patch(x0 - 6, LOADING_BAR_Y - 4, x0 + LOADING_BAR_WIDTH + 6, LOADING_BAR_Y + 6, 0xFF000000);
		loading_patch(x0, LOADING_BAR_Y, x0 + LOADING_BAR_WIDTH, LOADING_BAR_Y + 2, 0xFF404040);
		loading_patch(x0, LOADING_BAR_Y, x0 + (int)(progress * LOADING_BAR_WIDTH + 0.5f), LOADING_BAR_Y + 2, 0xFFC0C0C0);
	}
	if (view->notice)
		loading_notice(view->notice, LOADING_NOTICE_Y);
	sceGuEnable(GU_TEXTURE_2D);
	sceGuEnable(GU_DEPTH_TEST);
	sceGuDepthMask(GU_FALSE);
	sceGuEnable(GU_FOG);
}

/* the frame on the screen as the load begins: the buffer itself, which
stays put while the loading frames draw into the other one and read it */
const void *psp_ge_loading_capture(void)
{
	if ((!ge_ready && !boot_up) || frame_open)
		return NULL;
	loading_frames = 0;
	return (const void *)((unsigned int)sceGeEdramGetAddr() + loading_front());
}

/* one whole frame of the load (phase 0), drawn and shown by itself: no game
frame is open while the load runs. Before the game's first frame it is the
boot screen's, on its own list (psp_ge_boot_start) */
void psp_ge_loading_frame(const struct psp_loading_view *view)
{
	if ((!ge_ready && !boot_up) || frame_open || !view || boot_frozen)
		return;
	show2_list_drop();
	sceGuStart(GU_DIRECT, ge_ready ? (void *)display_list : (void *)(0x04000000 + BOOT_LIST_AT));
	/* no picture to start from (a cold start): from black */
	loading_trail(view->progress, view->captured != NULL);
	loading_overlay(view);
	stall_force = 1;
	sceGuFinish();
	sceGuSync(0, 0);
	stall_force = 0;
	if (boot_frozen)
		return;
	if (loading_shots > 0 && loading_frames % (unsigned long)loading_shots == 1)
		screenshot();
	draw_buffer = sceGuSwapBuffers();
}

/* the settle (phase 1): over the level's first view, inside its open frame
and before its swap; the world's see-through draws go first, so none lands
on top */
void psp_ge_loading_veil(const struct psp_loading_view *view)
{
	static int veil_old = -1, veil_settle = -1;
	int settle;

	if (!view || PSP_ME_WORK())
		return;
	/* geinflight: the frame's calls are written down and carried out later,
	so frame_open is the last frame's, and with gemeon the Media Engine may be
	writing that frame's list right now. Drawn from here the veil would land
	in a list another CPU is writing, or past its finish: the main menu's
	settle, whose veil comes after the frame's end, hung the GE that way. The
	veil is left out instead, as it already was wherever frame_open had shut.
	"veilold": drawn as before */
	if (veil_old < 0)
	{
		veil_old = psp_keep("veilold") != 0;
		veil_settle = !psp_keep("settleveiloff");
	}
	/* B1: so every settle frame was shown raw, the level's first view before
	its textures were in. What the frame has written down is carried out
	first (the ME waited for), and the veil then goes into a list only the
	main CPU is writing, as after a big model's drain. With nothing written
	(a frame still waiting or out) the test below still leaves it out.
	"settleveiloff": as before */
	settle = veil_settle && !veil_old && INFLIGHT_RECORDING();
	if (settle)
		GE_DEFER_DRAIN();
	if (!frame_open)
		return;
	if (!veil_old && (inflight_pending || inflight_me_out || (INFLIGHT_RECORDING() && !settle)))
		return;
	/* gedefer: the frame's calls written down so far go first, so the
	veil lands over them */
	if (!veil_old)
		GE_DEFER_DRAIN();
	list_hud_phase();
	loading_trail(view->progress, 1);
	loading_overlay(view);
	if (loading_shots > 0 && loading_frames % (unsigned long)loading_shots == 1)
		psp_screenshot_requested = 1;
}

/* ---------- the boot screen (psp_loading.c drives it)

Until the main menu's first frame the GE drew nothing, and the screen showed
the log scrolling (psp_platform.c; "bootlog" keeps it so). Now the GE starts
as soon as keep.txt is read, and the loading screen above shows the start-up:
a cold start's black frames, the progress, and the hint psp_loading.c gives.
The full display list can't be taken this early: it comes from the heap or
user RAM, and taken first thing it lands where the tag window must go (build
105). So the boot screen has a list of its own in VRAM, in benchrun's
region, which nothing uses until the first frame: the texture cache's VRAM
fills only once the GE has the screen (psp_tex_screen_free), and the models'
copies only as frames draw. The GE reads a list in VRAM written through the
uncached view (pspgu's), so nothing needs writing back. The first frame's
ge_initialize ends the boot screen's thread and carries on from here. */

int psp_ge_boot_start(void)
{
	extern void psp_screen_log_freeze(void);

	if (ge_ready || boot_up)
		return boot_up;
	/* the log's lines go on to log.txt; none to the screen */
	psp_screen_log_freeze();
	sceGuInit();
	sceGuStart(GU_DIRECT, (void *)(0x04000000 + BOOT_LIST_AT));
	sceGuDrawBuffer(GU_PSM_5650, (void *)0, BUFFER_WIDTH);
	sceGuDispBuffer(SCREEN_WIDTH, SCREEN_HEIGHT, (void *)FRAME_BYTES, BUFFER_WIDTH);
	sceGuDepthBuffer((void *)(2 * FRAME_BYTES), BUFFER_WIDTH);
	sceGuOffset(2048 - SCREEN_WIDTH / 2, 2048 - SCREEN_HEIGHT / 2);
	sceGuViewport(2048, 2048, SCREEN_WIDTH, SCREEN_HEIGHT);
	sceGuScissor(0, 0, SCREEN_WIDTH, SCREEN_HEIGHT);
	sceGuEnable(GU_SCISSOR_TEST);
	sceGuDisable(GU_DEPTH_TEST);
	sceGuDisable(GU_CULL_FACE);
	sceGuDisable(GU_LIGHTING);
	stall_force = 1;
	sceGuFinish();
	sceGuSync(0, 0);
	stall_force = 0;
	/* both frame buffers black before either shows (the uncached view) */
	memset((void *)0x44000000, 0, 2 * FRAME_BYTES);
	draw_buffer = (void *)0;
	sceDisplayWaitVblankStart();
	sceGuDisplay(GU_TRUE);
	boot_up = 1;
	platform_log("GE: the boot screen from here (bootlog: the log on the screen instead); its list in VRAM at %06x",
		BOOT_LIST_AT);
	return 1;
}

/* the crash handler's report is drawn into both frame buffers: the boot
screen stops drawing over it (psp_crash.c) */
void psp_ge_boot_freeze(void)
{
	boot_frozen = 1;
}

int psp_build_number(void);

static void screenshot(void)
{
	static unsigned char rows[4 * SCREEN_WIDTH * 3];
	const unsigned short *frame = (const unsigned short *)(0x44000000 + (unsigned int)(shot_buffer ? shot_buffer : draw_buffer));
	unsigned char header[54];
	unsigned int image = SCREEN_WIDTH * 3 * SCREEN_HEIGHT, size = 54 + image;
	char path[64], text[48];
	SceUID file, folder;
	SceIoDirent entry;
	int number = 0, y, x, row = 0;
	float seconds = psp_debug_seconds();

	/* the next number after the highest shotNNN* already there */
	sceIoMkdir(PSP_DATA_ROOT "/shots", 0777);
	folder = sceIoDopen(PSP_DATA_ROOT "/shots");
	if (folder >= 0)
	{
		memset(&entry, 0, sizeof(entry));
		while (sceIoDread(folder, &entry) > 0)
		{
			if (strncmp(entry.d_name, "shot", 4) == 0 && entry.d_name[4] >= '0' && entry.d_name[4] <= '9')
			{
				int n = atoi(entry.d_name + 4);

				if (n + 1 > number)
					number = n + 1;
			}
			memset(&entry, 0, sizeof(entry));
		}
		sceIoDclose(folder);
	}
	snprintf(path, sizeof(path), PSP_DATA_ROOT "/shots/shot%03d_t%d.bmp", number, (int)seconds);
	{
		const char *psp_preset_label(void);
		const char *preset = psp_preset_label();

		if (preset[0])
			snprintf(text, sizeof(text), "b%d  p%s  t %.1f  f %lu", psp_build_number(), preset, seconds, frames);
		else
			snprintf(text, sizeof(text), "b%d  t %.1f  f %lu", psp_build_number(), seconds, frames);
	}
	stamp_build(text);
	file = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
	if (file < 0)
	{
		platform_log("screenshot: can't write %s", path);
		return;
	}
	memset(header, 0, sizeof(header));
	header[0] = 'B'; header[1] = 'M';
	memcpy(header + 2, &size, 4);
	header[10] = 54; header[14] = 40;
	x = SCREEN_WIDTH; memcpy(header + 18, &x, 4);
	y = SCREEN_HEIGHT; memcpy(header + 22, &y, 4);
	header[26] = 1; header[28] = 24;
	memcpy(header + 34, &image, 4);
	sceIoWrite(file, header, sizeof(header));
	/* BMP rows run bottom up, blue green red */
	for (y = SCREEN_HEIGHT - 1; y >= 0; y--)
	{
		unsigned char *out = rows + row * SCREEN_WIDTH * 3;
		int stamp_row = y - (SCREEN_HEIGHT - STAMP_HEIGHT);

		for (x = 0; x < SCREEN_WIDTH; x++)
		{
			unsigned int v = frame[y * BUFFER_WIDTH + x];
			int m = stamp_row >= 0 ? stamp_mask[stamp_row][x] : 0;

			if (m)
			{
				out[3 * x] = out[3 * x + 1] = out[3 * x + 2] = m == 1 ? 255 : 0;
				continue;
			}
			out[3 * x] = (unsigned char)(((v >> 11) & 31) * 255 / 31);
			out[3 * x + 1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
			out[3 * x + 2] = (unsigned char)((v & 31) * 255 / 31);
		}
		if (++row == 4 || y == 0)
		{
			sceIoWrite(file, rows, row * SCREEN_WIDTH * 3);
			row = 0;
		}
	}
	sceIoClose(file);
	/* where from (build 35): the debug camera can then go back there
	(camera.txt: position, forward, up, field of view) */
	platform_log("[t %.1f] screenshot: %s (frame %lu) from %.2f %.2f %.2f looking %.2f %.2f %.2f, near plane %.4f", seconds, path,
		frames, camera_eye[0], camera_eye[1], camera_eye[2], camera_forward[0], camera_forward[1], camera_forward[2],
		projection_near);
}

/* build 74: the frame end's parts over each heartbeat window, for the log's
frameend: line (psp_platform.c). b72/b73's runs: the step holding the
frame end took 0.8 s a frame, growing by ~15 ms each frame, with no Memory
Stick reads; the per-300-frame report never came at 1 fps */
static PSP_MAIN_SIDE unsigned long win_frames = 0, win_flush_us = 0, win_writeback_us = 0, win_ge_us = 0, win_ge_worst = 0,
	win_swap_us = 0, win_after_us = 0, win_list_bytes = 0, win_triangles = 0, win_draws = 0, win_transparent = 0;
void psp_ge_frame_window(char *text, int size)
{
	/* (no GE_DEFER_DRAIN: the heartbeat's thread asks for this line, and
	waiting calls run only on the game's thread) */
	unsigned long n = win_frames ? win_frames : 1;

	snprintf(text, size, "frameend: %lu frames; a frame: see-through flush %.1f ms, cache write-back %.1f, waiting for the GE %.1f (worst %.1f), swap %.1f, caches %.1f; display list %lu KB, %lu level draws, %lu triangles, %lu see-through draws",
		win_frames, win_flush_us / 1000.0 / n, win_writeback_us / 1000.0 / n, win_ge_us / 1000.0 / n, win_ge_worst / 1000.0,
		win_swap_us / 1000.0 / n, win_after_us / 1000.0 / n, win_list_bytes / 1024 / n, win_draws / n, win_triangles / n,
		win_transparent / n);
	if (win_other_targets)
	{
		size_t used = strlen(text);

		snprintf(text + used, size > (int)used ? size - used : 0,
			"; %lu windows for another target than the screen (drawn on it)", win_other_targets);
		win_other_targets = 0;
	}
	{
		size_t used = strlen(text);

		gebusy_window(text + used, size - (int)used, win_frames); /* build 113 */
	}
	if (show_lat_on > 0 && win_lat_frames)
	{
		size_t used = strlen(text);

		snprintf(text + used, size > (int)used ? size - used : 0, "; showlat %.1f ms frame to screen (longest %.1f)",
			win_lat_sum / 1000.0f / win_lat_frames, win_lat_most / 1000.0f);
	}
	win_lat_frames = win_lat_sum = win_lat_most = 0;
	win_frames = win_flush_us = win_writeback_us = win_ge_us = win_ge_worst = win_swap_us = win_after_us = 0;
	win_list_bytes = win_triangles = win_draws = win_transparent = 0;
}

void psp_ge_frame_end(void)
{
	rso_stamp++;
	/* geinflight: the frame waits, written down whole, for the next frame's
	first call */
	if (INFLIGHT_RECORDING())
	{
		defer_words(DEFER_FRAME_END, 0, 0, 0, 0, 0, 0, 0, 0, 0);
		inflight_pending = 1;
		window_main_done = 0;
		inflight_window_written = 0;
		chunk_out = chunk_frame;
		if (chunk_frame)
			chunk_publish(1);
		else
			inflight_hand_over();
		chunk_stopped = 0;
		psp_sleepguard_notice();
		chunk_frame = psp_sleep_frame ? 0 : chunk_wanted();
		chunk_published = 0;
		psp_tex_chunking = chunk_frame;
		psp_tex_chunk_missed = 0;
		if (chunk_frame)
			chunk_frames++;
		return;
	}
	/* (a frame not in flight goes in no pieces) */
	if (chunk_frame)
		chunk_publish(2);
	psp_tex_chunking = 0;
	GE_DEFER_DRAIN(); /* build 112: gedefer */
	/* the ring's one producer, between frames: where the ME can be restarted
	after a sleep (a slot job still out puts it off to a later frame end) */
	psp_me_wake_poll();
	if (!frame_open)
	{
		psp_sleepguard_notice();
		return;
	}
	psp_me_ring_test(); /* "meringtest" (psp_me.c); off: one test a frame */
	psp_me_batch_test(); /* "mebatchtest" (psp_me.c); likewise */
	show2_settle(SHOW2_OTHER);
	frame_end_list();
	frame_end_show();
	/* B41: the list finished and the GE given the frame: a sleep put off is
	carried out here */
	psp_sleepguard_frame();
}

/* B41: the records' room given back with the volatile partition, by the game
thread itself between frames (no record is being written) */
void psp_ge_inflight_room_before_sleep(void)
{
	if (inflight_room)
		inflight_room_away = 1;
}

/* B41, after a sleep the game thread readied itself (psp_sleepguard.c): the
records' room checked as a thread waiting at its record would have (it
never waited: it was the one that slept) */
void psp_ge_inflight_room_after_sleep(void)
{
	inflight_room_away = 0;
	if (inflight_room)
		inflight_room_check(0);
}

/* the frame's end in two parts, as its window's begin: the list's (its last
draws and its finish: the Media Engine's turn under geinflight) and the main
CPU's (the GE waited for, the picture shown, the frame's reads and logs) */
/* the list part's figures for the heartbeat's window: written where the list
ends (on the Media Engine under geinflight) and added in by the main CPU
when the frame is shown, since the heartbeat's thread clears the window's
own counters whenever it reports them */
static struct { unsigned long transparent, flush_us, draws, models, triangles, list_bytes; } list_part;

static int frame_end_list(void)
{
	unsigned int flush_mark;

	if (!frame_open)
		return 0;
	flush_mark = ge_clock();
	list_part.transparent = transparent_count;
	transparent_flush();
	taa_resolve();
	ge_kinds_frame_end(); /* build 103 */
	CENSUS(census_frame());
	list_part.flush_us = ge_clock() - flush_mark;
	list_part.draws = draws_this_frame;
	list_part.models = model_draws_this_frame;
	list_part.triangles = triangles_this_frame;
	/* the display list's high-water mark: it has no overflow check of its own
	(build 19) */
	{
		int used = sceGuCheckList();

		list_part.list_bytes = used;

		if (used > list_peak)
		{
			list_peak = used;
			if (used > display_list_size * 7 / 8)
				ge_log("GE: display list %d of %d bytes used", used, display_list_size);
		}
	}
	/* the frame's data to RAM before the list's last stall moves (on the
	Media Engine its job's end writes its cache back) */
	if (!PSP_ME_WORK())
	{
		unsigned int mark = ge_clock();

		sceKernelDcacheWritebackAll();
		frame_end_us[0] += ge_clock() - mark;
		win_writeback_us += ge_clock() - mark;
	}
	/* pspgu's finish holds off interrupts, a system call the Media Engine
	can't make: in its work the list is finished by the main CPU, when the
	frame is shown. The GE meanwhile has the frame's every draw and waits at
	the stall, so the picture doesn't wait for it */
	if (inflight_on && PSP_ME_WORK())
	{
		/* gebusy2: the list's last draws (the transparent flush, the TAA
		resolve) go to the GE with a SIGNAL of their own now, or they would
		wait for the main CPU's finish untimed */
		gebusy_me_tail();
		inflight_finish_left = 1;
	}
	else
	{
		/* the finish's stall update always reaches the GE */
		stall_force = 1;
		sceGuFinish();
		stall_force = 0;
	}
	return 1;
}

/* the runs' spheres the Media Engine's work asked for (runs_want), made while
the parts it drew are still resident: before the frame's end lets anything go */
static void runs_wanted_make(void)
{
	int k;

	for (k = 0; k < runs_wanted_count; k++)
	{
		struct runs_want const *w = &runs_wanted[k];

		if (w->batch)
		{
			if (w->batch->cluster_count < 0)
				near_clusters_make(w->batch, w->skin, w->vertices);
		}
		else
		{
			part_k14 = converted_k(w->vertices);
			(void)rigid_runs_for(w->vertices, w->indices, w->index_count, w->strip);
			part_k14 = 0.0f;
		}
		runs_wanted_made++;
	}
	runs_wanted_count = 0;
}

/* the frame's picture: the GE's end waited for, the checks that read the
finished frame, the overlay drawn into it, and the swap. On the game thread,
or under geshow2 on the forwarding thread */
static void show_picture(int show3)
{
	unsigned int mark = sceKernelGetSystemTimeLow(), waited;
	void *const buffer = show3 ? show3_buffer : draw_buffer;

	/* build 35: how long the CPU waits here for the GE to finish the
	frame's list. Long waits: the GE is the limit (fill, passes); short:
	the CPU is, and drawing overlapped with the next frame's work would
	gain little. geshow3: the next list is in the GE's queue already,
	held at its start, so the wait is for this list's finish only */
	gebusy_wait(1, 1);
	if (show3)
		show3_wait_finish(show3_asked & 0xFFFF);
	else
		sceGuSync(0, 0);
	gebusy_wait(0, 1);
	waited = sceKernelGetSystemTimeLow() - mark;
	ge_wait_us += waited;
	win_ge_us += waited;
	if (!show3)
		vram_busy_calibrate(waited); /* (its sprites are the replay's) */
	{
		/* build 87: this frame's renderer CPU and GE wait, for the
		heartbeat's window (psp_ge_budget) */
		unsigned long sum = 0;
		int k;

		for (k = 0; k < GE_ENTRIES; k++)
			sum += ge_entry_us[k];
		hb_renderer_us += sum >= entry_snapshot ? sum - entry_snapshot : sum;
		entry_snapshot = sum;
		hb_wait_us += waited;
		hb_frames++;
	}
	if (waited > win_ge_worst)
		win_ge_worst = waited;
	if (waited > ge_wait_worst)
		ge_wait_worst = waited;
	/* blipdepthcheck: the GE has the whole frame done (geshow3: the replay
	of the next frame may be writing both; the window's start clears the
	flag anyway) */
	if (!show3)
	{
		if (blip_check)
			blip_check_compare();
		blip_depth_live = 0;
	}
	{
		/* "framecrc": each finished picture hashed (FNV-1a over its
		480 x 272 pixels, before the debug overlay) and logged with the
		game time. Two runs that play the same (fixedticks fixedseed) then
		show whether a change draws exactly the same picture
		(tools/framecmp.py). Recon only: it reads VRAM every frame */
		static PSP_MAIN_SIDE int frame_crc = -1;

		if (frame_crc < 0)
			frame_crc = psp_keep("framecrc") != 0;
		if (frame_crc)
		{
			extern long psp_game_time_peek(void);
			const unsigned int *pixels = (const unsigned int *)(0x44000000 + (unsigned int)buffer);
			unsigned int hash = 2166136261u, tile[16];
			int row, pair, k;

			/* the whole picture's hash, then a 4 x 4 grid of tiles'
			(120 x 68 pixels each), so a difference can be placed */
			for (k = 0; k < 16; k++)
				tile[k] = 2166136261u;
			for (row = 0; row < SCREEN_HEIGHT; row++)
				for (pair = 0; pair < SCREEN_WIDTH / 2; pair++)
				{
					unsigned int word = pixels[row * (BUFFER_WIDTH / 2) + pair];
					unsigned int *t = &tile[(row / (SCREEN_HEIGHT / 4)) * 4 + pair / (SCREEN_WIDTH / 8)];

					hash = (hash ^ word) * 16777619u;
					*t = (*t ^ word) * 16777619u;
				}
			/* the real log, not the ME's: the forwarding thread shows the
			frame while the simulated ME's job marks the work the ME's */
			(platform_log)("framecrc %lu %ld %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x %08x",
				show_frame_number, psp_game_time_peek(), hash, tile[0], tile[1], tile[2], tile[3], tile[4], tile[5], tile[6], tile[7],
				tile[8], tile[9], tile[10], tile[11], tile[12], tile[13], tile[14], tile[15]);
		}
	}
	{
		/* build 77: the debug menu and overlay, by the CPU into the
		finished frame (psp_debug.c), after the screenshot so shots never
		show it ("menushot": before, for testing the menu itself) */
		extern void psp_debug_draw(void *vram);
		static PSP_MAIN_SIDE int menu_shot = -1;

		if (menu_shot < 0)
			menu_shot = psp_keep("menushot");
		if (menu_shot)
			psp_debug_draw((void *)(0x44000000 + (unsigned int)buffer));
		if (show_shot)
		{
			show_shot = 0;
			shot_buffer = buffer;
			screenshot();
			shot_buffer = NULL;
		}
		if (!menu_shot)
			psp_debug_draw((void *)(0x44000000 + (unsigned int)buffer));
	}
	mark = sceKernelGetSystemTimeLow();
	if (show3)
		psp_display_release(); /* (the swap's pointers moved at the ask) */
	else
		draw_buffer = sceGuSwapBuffers();
	frame_end_us[1] += sceKernelGetSystemTimeLow() - mark;
	/* build 118: how long each frame stayed on the screen, in display
	refreshes (60 a second): frames are shown at refreshes, so a frame
	that misses 33.3 ms shows for 50 (20 fps for that frame), not 34.
	The heartbeat's 5 s averages hide these single-frame hitches */
	{
		unsigned int now = sceKernelGetSystemTimeLow();

		if (swap_last)
		{
			unsigned int shown = now - swap_last;
			int refreshes = shown < 25000 ? 0 : shown < 41667 ? 1 : shown < 58333 ? 2 : shown < 75000 ? 3 : 4;

			swap_shown[refreshes]++;
			if (shown > swap_longest)
				swap_longest = shown;
			/* build 118: mebench (a turn's first frame left out: it
			started under the last way) */
			if (me_bench > 0 && me_bench_settled)
			{
				me_bench_frames[me_bench_way]++;
				me_bench_us[me_bench_way] += shown;
			}
		}
		swap_last = now;
		/* showlat: from the frame's window start to this swap */
		if (show_lat_on > 0 && show_begin_us)
		{
			unsigned long latency = now - show_begin_us;

			show_lat_frames++;
			show_lat_sum += latency;
			if (latency > show_lat_most)
				show_lat_most = latency;
			win_lat_frames++;
			win_lat_sum += latency;
			if (latency > win_lat_most)
				win_lat_most = latency;
		}
	}
	win_swap_us += sceKernelGetSystemTimeLow() - mark;
}

/* every level draw and model part of every frame shown so far, for the
settle's warm test (psp_loading.c): a settle that has drawn nothing has
warmed nothing (the main menu's ring is models alone) */
static PSP_MAIN_SIDE unsigned long draws_shown_total = 0;

unsigned long psp_ge_draws_shown(void)
{
	return draws_shown_total;
}

static void frame_end_show(void)
{
	runs_wanted_make();
	win_frames++;
	win_transparent += list_part.transparent;
	win_flush_us += list_part.flush_us;
	win_draws += list_part.draws;
	draws_shown_total += list_part.draws + list_part.models;
	win_triangles += list_part.triangles;
	win_list_bytes += list_part.list_bytes;
	window_main_done = 0;
	show3_now = 0;
	if (inflight_finish_left)
	{
		inflight_finish_left = 0;
		stall_force = 1;
		/* geshow3: the finish carries the frame's id (never 0), which the
		forwarding thread waits for instead of the whole queue */
		if (show2_now && show3_on && !list_in_vram)
		{
			if (!(++show3_asked & 0xFFFF))
				show3_asked++;
			sceGuFinishId(show3_asked & 0xFFFF);
			show3_now = 1;
		}
		else
			sceGuFinish();
		stall_force = 0;
	}
	vm_frame_end(); /* build 116: vrammodels' copies for the next frame */
	{
		unsigned int mark;

		show_frame_number = frames;
		show_begin_us = frame_begin_us;
		/* taken now: under geshow2 the next frame's start may ask its own
		shot before this one is shown */
		show_shot = psp_screenshot_requested;
		psp_screenshot_requested = 0;
		/* geshow2: the GE's tail waited out and the frame shown by the
		forwarding thread, while this thread goes on; the rest here is the
		game thread's (the tables the record path reads) */
		if (show3_now)
			show3_ask();
		else if (show2_now && !show3_on)
			show2_ask();
		else
		{
			show2_settle(SHOW2_OTHER);
			show_picture(0);
			show3_out = 0;
		}
		cut_count_frame_end();
		frame_open = 0;
		mark = sceKernelGetSystemTimeLow();
		psp_tex_frame_end();
		/* build 68: the geometry cache's reads, and the level copies' budget */
		psp_geo_frame_end();
		/* build 112: the copies the Media Engine's work wanted, built now */
		me_log_write();
		{
			int k;

			for (k = 0; k < streams_wanted_count; k++)
			{
				stream_asked_texture = streams_wanted[k].texture;
				level_stream_get(streams_wanted[k].vertices, streams_wanted[k].lightmap_vertices, streams_wanted[k].count);
			}
			streams_wanted_count = 0;
			/* the see-through level triangles it wanted asked */
			for (k = 0; k < walls_wanted_count; k++)
				overlay_on_a_wall(walls_wanted[k].stream, walls_wanted[k].indices, walls_wanted[k].triangle_count,
					walls_wanted[k].triangle);
			walls_wanted_count = 0;
			/* and the model buffers it wanted converted */
			for (k = 0; k < converts_wanted_count; k++)
			{
				struct skin_part *plan;

				convert_float_only = converts_wanted[k].float_only;
				model_convert(converts_wanted[k].vertices, converts_wanted[k].count, converts_wanted[k].uncompressed,
					converts_wanted[k].indices, converts_wanted[k].index_count, converts_wanted[k].strip,
					converts_wanted[k].skin ? &plan : NULL);
			}
			converts_wanted_count = 0;
		}
		if (psp_geo_open_count())
			level_streams_trim();
		arena_frame_end(); /* build 111: compaction, the heap's copies brought back */
		level_copies_follow();
		if (stream_room > 0)
			stream_room_frame_end(); /* after the move: a copy built now takes level_origin */
		ge_hot.stream_frame++;
		frame_end_us[2] += sceKernelGetSystemTimeLow() - mark;
		win_after_us += sceKernelGetSystemTimeLow() - mark;
	}
	if (++frames == 1 || frames % 300 == 0)
	{
		platform_log("GE: sky drawn %lu times, %lu solid parts; %lu see-through draws, %lu dropped; %lu sprites; %lu HUD quads, %lu characters, %lu blips; skinned parts %lu (%lu batches, %lu KB, %lu left rigid); level see-through %lu; decals %lu; flashes %lu; shadows %lu begun, %lu requests, %lu draws; light draws %lu (%lu with a projected cone, %lu triangles outside cones skipped, %lu split near the light, %lu pieces added); bending triangles cut at the near plane %lu (%lu pieces); two-sided parts %lu; models at the near plane %lu (%lu triangles left out)",
			sky_draws, sky_parts, transparent_drawn, transparent_dropped, sprites_queued, hud_quads, text_characters,
			blips_drawn, skinned_parts, skin_batches, skin_bytes / 1024, skin_failures, level_transparent_draws, decals_drawn, flashes_drawn, shadow_begins, shadow_requests, shadows_drawn, lights_drawn, light_cone_draws, light_cone_skipped, light_split_triangles, light_split_pieces, skin_cut_triangles, skin_cut_pieces, two_sided_draws, near_models, near_triangles_dropped);
		platform_log("GE: cut pieces lost to a full cut buffer %lu (since the start)", cut_pieces_lost);
		if (blip_target != BLIP_TARGET)
		{
			char kinds[KINDS * 12 + 1];
			int length = 0, k;

			for (k = 0; k < KINDS; k++)
				if (blip_depth_after_kind[k])
					length += snprintf(kinds + length, sizeof(kinds) - length, " %c%lu", kind_letters[k],
						blip_depth_after_kind[k]);
			kinds[length] = 0;
			platform_log("GE: blipdepth: after the target, by kind:%s; %lu with the depth test on (since the start)",
				length ? kinds : " none", blip_depth_after_tested);
		}
		if (blip_target != BLIP_TARGET)
			platform_log("GE: blipdepth: %lu world draws after the target in the depth buffer; check: %lu frames compared, %lu differed (%lu words; the first at byte %u in frame %lu, depth %08x for %08x), %lu not drawn again (since the start)",
				blip_depth_world_after, blip_check_frames, blip_check_differ, blip_check_words, blip_check_first,
				blip_check_first_frame, blip_check_first_depth, blip_check_first_reference, blip_check_lost);
		if (glyphs_drawn)
			platform_log("GE: button glyphs drawn %lu (since the start)", glyphs_drawn);
		cut_count_report(frames == 1 ? 1 : 300);
		/* build 87: its own line (the one above passed the log's line length) */
		/* build 89: the counters over these 300 frames (they were never
		reset, so a bench window's line carried every earlier window's;
		review), and the ME's part on a line of its own (512 characters) */
		platform_log("GE: light pass mode %d: %lu triangles as the level drew them, %lu nudged draws, %lu skipped, %lu unlifted; stencil bake (mode %d): %lu wall triangles drawn as %lu pieces, %lu stencil layer draws (%lu triangles), %lu stencil triangles on the old path; level vertices %s: %lu materials at 16 bits, %lu too wide, %lu 16-bit passes (last 300 frames)",
			light_mode, light_index_triangles, light_nudged_draws, lights_skipped, light_exact_triangles, stencil_mode, bake_substituted,
			bake_pieces_drawn, bake_layer_draws, bake_layer_triangles, bake_old_path_triangles,
			level_compact > 0 ? "16-bit (levelcompact)" : "float", compact_streams, compact_refused, compact_draws);
		if (level_local > 0)
			platform_log("GE: the level's origin (%.0f %.0f %.0f), moved %lu times; copies moved to it %lu (%lu vertices, %lu us), left for being out of exact reach %lu (last 300 frames)",
				level_origin[0], level_origin[1], level_origin[2], level_origin_moves, level_copies_moved,
				level_vertices_moved, level_move_us, ge_hot.level_copies_left);
		else if (level_local == 0)
			platform_log("GE: the level's origin: the world's (levellocaloff)");
		level_origin_moves = level_copies_moved = level_vertices_moved = level_move_us = ge_hot.level_copies_left = 0;
		{
			/* build 119: the light pass's surface lists sorted (psp_fixups.py, structure_render.c#surfacesort) */
			extern unsigned long psp_surface_sorts, psp_surface_sorts_in_order, psp_surface_sort_mismatches,
				psp_surface_sorts_by_bits;

			platform_log("GE: surface lists sorted %lu (%lu through the surface bits, %lu already in order)%s (last 300 frames)",
				psp_surface_sorts, psp_surface_sorts_by_bits, psp_surface_sorts_in_order, psp_keep("surfacesortcheck") ? (psp_surface_sort_mismatches ?
				"; surfacesortcheck: SOME LISTS CAME OUT DIFFERENT" : "; surfacesortcheck: all the same") : "");
			if (psp_surface_sort_mismatches)
				platform_log("GE: surfacesortcheck: %lu lists came out different", psp_surface_sort_mismatches);
			psp_surface_sorts = psp_surface_sorts_in_order = psp_surface_sort_mismatches = psp_surface_sorts_by_bits = 0;
		}
		{
			/* lightplancensus (psp_fixups.py, structure_render.c): lights whose plan held from the frame before */
			extern void psp_light_plan_report(void);

			psp_light_plan_report();
		}
		/* build 119's probe */
		if (probes_on)
			platform_log("GE: light pass triangles: %lu looked at, %lu with a corner in reach; the triangle loop %lu us a frame over %lu draws; before the loop %lu us, after it %lu us (last 300 frames)",
			light_triangles_looked, light_triangles_lit, light_loop_us / 300, light_probe_draws, light_setup_us / 300, light_post_us / 300);
		light_triangles_looked = light_triangles_lit = light_loop_us = light_probe_draws = light_setup_us = light_post_us = 0;
		/* build 116: the restorations' own counters */
		/* build 118: the frames' time on screen (above) */
		platform_log("GE: frames shown for 1 display refresh (60 fps) %lu, 2 (30 fps) %lu, 3 (20 fps) %lu, 4 (15 fps) %lu, 5 or more %lu; the longest %u ms (last 300 frames)",
			swap_shown[0], swap_shown[1], swap_shown[2], swap_shown[3], swap_shown[4], swap_longest / 1000);
		memset(swap_shown, 0, sizeof(swap_shown));
		swap_longest = 0;
		if (show_lat_on > 0)
		{
			platform_log("GE: showlat: frame to screen (window start to swap) %.1f ms a frame, the longest %.1f ms, over %lu frames (last 300 frames)",
				show_lat_frames ? show_lat_sum / 1000.0f / show_lat_frames : 0.0f, show_lat_most / 1000.0f, show_lat_frames);
			show_lat_frames = show_lat_sum = show_lat_most = 0;
		}
		if (show2_on > 0)
		{
			unsigned long waits = 0, us = 0;
			int why;

			for (why = 0; why < SHOW2_WHYS; why++)
			{
				waits += show2_waited[why];
				us += show2_wait_us[why];
			}
			platform_log("GE: geshow2: %lu frames shown by the forwarding thread; the game thread waited for a show %lu times, %.1f ms a frame (at a hand-over %lu, %lu us; a call run at once %lu, %lu us; a compaction %lu; copies moved %lu; other %lu; at the ME's end %lu, %lu us; for list room %lu); level copies spared for the frame shown %lu; index lists copied %lu (%lu KB); the triangle ring's waits %lu (in other) (last 300 frames)",
				show2_shows, waits, us / 1000.0f / 300, show2_waited[SHOW2_HAND_OVER], show2_wait_us[SHOW2_HAND_OVER],
				show2_waited[SHOW2_DRAIN], show2_wait_us[SHOW2_DRAIN], show2_waited[SHOW2_COMPACT], show2_waited[SHOW2_FOLLOW],
				show2_waited[SHOW2_OTHER], show2_waited[SHOW3_ME_END], show2_wait_us[SHOW3_ME_END], show2_waited[SHOW3_ROOM],
				show2_spared, show2_index_copies, show2_index_bytes / 1024, show2_ring_waits);
			if (show3_on)
			{
				char text[320];
				size_t used = 0;
				unsigned long reused = 0;
				int kind;

				for (kind = 0; kind < SHOW3_KINDS; kind++)
				{
					reused += show3_reused[kind];
					used += (size_t)snprintf(text + used, used < sizeof(text) ? sizeof(text) - used : 0, "%s %s %lu of %lu",
						kind ? "," : "", show3_kind_name[kind], show3_reused[kind], show3_checked[kind]);
				}
				platform_log("GE: geshow3: lists in a part of the display list %lu, in all of it %lu (the largest lately %u KB); stall addresses held for a show %lu; parts grown up %lu, by a jump %lu; draws skipped for room in a part %lu (last 300 frames); finishes not reported %lu (since the start)",
					show3_rooms_shared, show3_rooms_whole, show3_list_peak / 1024, show3_held, show3_grown, show3_jumped, show3_room_skips, show3_given_up);
				if (show3_check)
					platform_log("GE: geshow3check: %lu reuses of what the GE reads before its frame was shown (since the start):%s",
						reused, text);
				show3_rooms_shared = show3_rooms_whole = show3_held = show3_room_skips = show3_grown = show3_jumped = 0;
			}
			show2_shows = show2_spared = show2_index_copies = show2_index_bytes = show2_ring_waits = 0;
			memset(show2_waited, 0, sizeof(show2_waited));
			memset(show2_wait_us, 0, sizeof(show2_wait_us));
		}
		if (me_bench > 0)
		{
			int way;

			platform_log("GE: mebench, last 300 frames: on the main CPU %lu frames, %.1f ms a frame; on the ME serially %lu, %.1f ms; overlapped %lu, %.1f ms",
				me_bench_frames[0], me_bench_frames[0] ? me_bench_us[0] / 1000.0f / me_bench_frames[0] : 0.0f,
				me_bench_frames[1], me_bench_frames[1] ? me_bench_us[1] / 1000.0f / me_bench_frames[1] : 0.0f,
				me_bench_frames[2], me_bench_frames[2] ? me_bench_us[2] / 1000.0f / me_bench_frames[2] : 0.0f);
			for (way = 0; way < 3; way++)
				me_bench_frames[way] = me_bench_us[way] = 0;
		}
		platform_log("GE: build 116 restorations, last 300 frames: see-through parts faded by angle %lu (%lu of them the level's); windows in planar fog %lu; meter parts %lu (%lu left out: map not loaded yet or no palette); see-through draws doubled %lu, added %lu, flat %lu (rounded %lu), tinted by angle %lu",
			seethrough_angle_draws, level_angle_draws, planar_fog_windows, meter_parts, meter_parts_dropped,
			seethrough_doubled_draws, seethrough_added_draws, seethrough_flat_draws, seethrough_rounded_draws,
			seethrough_angle_tinted_draws);
		platform_log("GE: see-through depth as the Xbox's, last 300 frames: level draws without a nudge %lu; draws with the map clamped %lu; added water faded by the fog %lu; level draws front faces only %lu",
			seethrough_exact_draws, seethrough_clamped_draws, seethrough_fog_faded_draws, level_culled_draws);
		seethrough_exact_draws = seethrough_clamped_draws = seethrough_fog_faded_draws = level_culled_draws = 0;
		{
			extern void psp_transparent_layers_counts(unsigned long *queued, unsigned long *unmapped);
			extern unsigned long psp_transparent_noise_count(void);
			unsigned long layers_queued, layers_unmapped;

			psp_transparent_layers_counts(&layers_queued, &layers_unmapped);
			platform_log("GE: models as the Xbox draws them, last 300 frames: see-through parts one-sided %lu (seethroughtwosided: both sides); extra layers queued %lu, %lu with no map (nolayers: none); camouflaged parts shimmering %lu (camoshimmer); generic shaders' second map passes %lu (genericnoise); self-occluding objects' depth first %lu times, %lu parts (noselfocclude: none); glass draws tinting the frame behind %lu (glassnotint: none)",
				seethrough_culled_draws, layers_queued, layers_unmapped, camo_shimmer_parts, psp_transparent_noise_count(),
				self_occlude_runs, self_occlude_parts, glass_tinted_draws);
		}
		self_occlude_runs = self_occlude_parts = 0;
		glass_tinted_draws = 0;
		seethrough_culled_draws = 0;
		camo_shimmer_parts = 0;
		seethrough_angle_draws = 0;
		level_angle_draws = 0;
		seethrough_doubled_draws = 0;
		seethrough_added_draws = 0;
		seethrough_flat_draws = 0;
		seethrough_rounded_draws = 0;
		seethrough_angle_tinted_draws = 0;
		if (seethrough_lit_draws || model_point_parts)
			platform_log("GE: after build 116, last 300 frames: glass parts lit by their model's light %lu; model parts lit by point lights %lu",
				seethrough_lit_draws, model_point_parts);
		seethrough_lit_draws = 0;
		model_point_parts = 0;
		/* build 118: transparent_cut's sphere test (the words read here too:
		until a part has been cut they are still -1) */
		if (tcut_full < 0)
			tcut_full = psp_keep("tcutfull");
		if (tcut_check < 0)
			tcut_check = psp_keep("tcutcheck");
		if (tcut_full)
			platform_log("GE: see-through parts to cut, last 300 frames: %lu, each checked triangle by triangle (tcutfull)",
				tcut_parts);
		else if (tcut_check)
			platform_log("GE: see-through parts to cut, last 300 frames: %lu; wholly in view by their model's sphere %lu, no sphere %lu; tcutcheck: of the %lu the sphere called in view, checked triangle by triangle anyway, %lu had a triangle outside",
				tcut_parts, tcut_inside, tcut_no_sphere, tcut_inside, tcut_mismatches);
		else
			platform_log("GE: see-through parts to cut, last 300 frames: %lu; wholly in view by their model's sphere %lu (left whole, no triangle checked), no sphere %lu",
				tcut_parts, tcut_inside, tcut_no_sphere);
		tcut_parts = tcut_no_sphere = tcut_inside = tcut_mismatches = 0;
		/* build 118: the see-through parts' node scans (transparent_part_multi) */
		platform_log("GE: see-through parts' node scans, last 300 frames: %lu scanned, %lu remembered%s%lu",
			multi_scans, multi_remembered, multi_check ? "; multicheck: remembered answers that differed " : "; ", multi_mismatches);
		multi_scans = multi_remembered = multi_mismatches = 0;
		if (foliage_two_pass > 0)
			platform_log("GE: foliagetwopass: alpha-tested parts' soft second passes, last 300 frames: %lu", foliage_second_passes);
		foliage_second_passes = 0;
		if (seethrough_vfpu > 0)
			platform_log("GE: seethroughvfpu: see-through vertices placed on the VFPU, last 300 frames: %lu", seethrough_vfpu_vertices);
		seethrough_vfpu_vertices = 0;
		planar_fog_windows = 0;
		meter_parts = meter_parts_dropped = 0;
		if (far_cut_triangles || far_cut_boxes || far_cut_dropped)
			platform_log("GE: farcut, last 300 frames: %lu level triangles cut at the far clip (%.1f), %lu left out wholly past it, %lu draws classified only for it",
				far_cut_triangles, far_cut_at, far_cut_dropped, far_cut_boxes);
		far_cut_triangles = far_cut_boxes = far_cut_dropped = 0;
		if (submerged_windows)
			platform_log("GE: ocean fog, last 300 frames: windows with planar fog %lu; level draws below the plane with the eye above %lu, triangles %lu, vertices %lu, %lu us on the CPU%s; left out (list full) %lu; cut-piece vertices %lu%s",
				submerged_windows, submerged_draws, submerged_triangles, submerged_vertices, submerged_us,
				submerged_old ? " (oceanfogold: counted, not drawn)" : "", submerged_dropped,
				submerged_cut_vertices, submerged_cut_old > 0 ? " (submergedcutold: left out)" : "");
		submerged_windows = submerged_draws = submerged_triangles = submerged_vertices = submerged_us = submerged_dropped = 0;
		submerged_cut_vertices = 0;
		if (light_merge > 0)
			platform_log("GE: lightmerge: merged light draws, last 300 frames: %lu", light_merged_draws);
		light_merged_draws = 0;
		light_index_triangles = light_nudged_draws = lights_skipped = light_exact_triangles = 0;
		bake_substituted = bake_pieces_drawn = bake_layer_draws = bake_layer_triangles = bake_old_path_triangles = 0;
		compact_draws = 0;
		/* build 90 */
		platform_log("GE: permanent decals (%s): %lu drawn on the level's own triangles (%lu triangles), %lu the usual way (last 300 frames); planes scanned %lu, from memory %lu; %lu us a frame",
			decal_project ? "on the level's triangles" : "nudged, as before", decals_projected, decal_projected_triangles,
			decals_project_fallback, decal_cache_scans, decal_cache_uses, decal_project_us / 300);
		decals_projected = decal_projected_triangles = decals_project_fallback = 0;
		decal_cache_scans = decal_cache_uses = decal_project_us = 0;
		/* build 113 */
		if (contrails_queued || contrails_out_of_view)
			platform_log("GE: contrails (contrails): %lu queued (%lu faded by their alpha), %lu segments; %lu pairs of corners folded away (behind the "
				"near plane or past the GE's range), %lu contrails wholly out of view (last 300 frames)", contrails_queued,
				contrails_faded, contrail_segments, contrail_pairs_folded, contrails_out_of_view);
		contrails_queued = contrail_segments = contrail_pairs_folded = contrails_out_of_view = contrails_faded = 0;
		if (zoom_masks || zoom_masks_missing)
			platform_log("GE: zoom masks (zoommask): %lu drawn, %lu not in the texture pack (last 300 frames)", zoom_masks,
				zoom_masks_missing);
		zoom_masks = zoom_masks_missing = 0;
		if (me_classify_jobs || me_classify_failures)
			platform_log("GE: ME classify (meclassify, measure only): %lu jobs, %lu vertices, the main CPU waited %lu us in all, %lu answers differing, %lu failed",
				me_classify_jobs, me_classify_vertices, me_classify_wait_us, me_classify_mismatches, me_classify_failures);
		/* build 87: the like-for-like of the two measure jobs (ME vs main CPU,
		the same function on the same data, both starting with the data out of
		their caches) */
		if (me_classify_jobs || me_angle_jobs || me_angle_failures)
		{
			platform_log("GE: ME measure (meclassify): classify %lu jobs, %lu vertices: the ME's code %lu us (%lu ns a vertex), the main CPU's %lu us (%lu ns a vertex), answers differing %lu; slant buckets %lu jobs, %lu triangles: the ME's code %lu us (%lu ns a triangle), the main CPU's %lu us (%lu ns a triangle), answers differing %lu, %lu failed; the main CPU waited %lu us for both",
				me_classify_jobs, me_classify_vertices, me_classify_me_us,
				me_classify_vertices ? (unsigned long)(1000ull * me_classify_me_us / me_classify_vertices) : 0,
				me_classify_sc_us, me_classify_vertices ? (unsigned long)(1000ull * me_classify_sc_us / me_classify_vertices) : 0,
				me_classify_sc_mismatches, me_angle_jobs, me_angle_triangles, me_angle_me_us,
				me_angle_triangles ? (unsigned long)(1000ull * me_angle_me_us / me_angle_triangles) : 0,
				me_angle_sc_us, me_angle_triangles ? (unsigned long)(1000ull * me_angle_sc_us / me_angle_triangles) : 0,
				me_angle_mismatches, me_angle_failures, me_classify_wait_us + me_angle_wait_us);
			me_classify_jobs = me_classify_vertices = me_classify_me_us = me_classify_sc_us = me_classify_sc_mismatches = 0;
			me_classify_wait_us = me_classify_mismatches = me_classify_failures = 0;
			me_angle_jobs = me_angle_triangles = me_angle_me_us = me_angle_sc_us = me_angle_mismatches = 0;
			me_angle_failures = me_angle_wait_us = 0;
		}
		/* build 89: in two lines; the one line passed the log's 512
		characters and the peaks (the cut-corner one is the only sign that
		MAXIMUM_CUT_CORNERS ran out) never reached the log (review) */
		platform_log("GE: frame %lu, %lu level draws (%lu textured, %lu flat so far), %lu triangles; %lu cut up near the camera, %lu skipped; %lu model parts (%lu buffers converted, %lu again after the cache moved them); level streams %lu KB (%lu let go); detail passes %lu, level glow passes %lu; peaks: list %d of %d bytes, cut corners %d of %d; looking (%.2f %.2f %.2f)",
			frames, draws_this_frame, textured_draws, flat_draws, triangles_this_frame, clipped_this_frame, culled_this_frame,
			model_draws_this_frame, converted_count, reconverted_count, stream_bytes / 1024, streams_let_go, detail_passes, level_glow_passes,
			list_peak, display_list_size, clip_peak, MAXIMUM_CUT_CORNERS, camera_forward[0], camera_forward[1], camera_forward[2]);
		if (model14 > 0)
			platform_log("GE: model14: %lu parts (%lu vertices, %lu KB less than 24-byte records); see-through asks refused %lu; skinned parts at full size %lu (%lu vertices; 10 bytes less a vertex would free %lu KB)",
				model14_parts, model14_vertices, model14_vertices * 10 / 1024, model14_refused,
				model14_skinned_parts, model14_skinned_vertices, model14_skinned_vertices * 10 / 1024);
		if (arena_state > 0)
		{
			platform_log("GE: the copies' arena: %lu of %u KB used, largest free %u KB; let go for room %lu; pieces to the heap %lu (%lu there now), kept off the heap's reserve %lu (tables %lu); compacted %lu times, %lu KB; brought back %lu",
				ARENA_ROOM->used / 1024, ARENA_ROOM->bytes / 1024, arena_largest / 1024, arena_let_go, arena_from_heap, arena_heap_live,
				copy_reserve_refused, heap_reserve_refused, arena_compactions, arena_moved_kb, arena_migrated);
			carve_report();
		}
		platform_log("GE: widget sprites %lu (%lu begun, %lu untextured; %lu of %lu flare answers clear, %lu rays cast, %lu answers kept); change colour passes %lu (%lu left out, the map not resident), self-illumination %lu (%lu with glow maps, %lu clamped, %lu without a glow map: unlit); meter palettes %lu worked out, %lu reused; HUD quads %lu us a frame (with the see-through flush inside), characters %lu (picture changed %lu times)",
			widget_sprites, widget_begins, widget_untextured, occlusion_visible, occlusion_tests, psp_occlusion_rays_cast, psp_occlusion_rays_kept, change_passes, change_unbound, glow_passes, glow_mapped, glow_clamped, glow_unmapped, meter_worked, meter_reused, hud_quad_us / (frames ? frames : 1), text_us / (frames ? frames : 1), text_changes);
		platform_log("GE: flares hidden by the level alone %lu (their objects not tested); flarescheck: %lu answers compared, %lu differed",
			psp_flare_walls_hidden, psp_flare_walls_checked, psp_flare_walls_differed);
		if (reflection_words.specular > 0 || reflection_words.env_fp > 0 || reflection_words.env_near > 0 ||
			reflection_words.chief_visor > 0 || reflection_words.sheen > 0)
			platform_log("GE: reflections: %lu parts with a specular highlight, %lu reflection passes, %lu without their sphere map in memory; Chief's visor drawn %lu (%lu textured, %lu shiny, last %.0f pixels across); Chief's sheen passes %lu",
				specular_parts, env_passes, env_unbound, visor_drawn, visor_textured, visor_shiny, visor_pixels, sheen_passes);
		if (PSP_DITHERSHIFT && dither_shift > 0)
		{
			platform_log("GE: %s: %lu passes dithered at a moved place", dither_shift == 2 ? "dithershiftall" : "dithershift", dither_moves);
			dither_moves = 0;
		}
		if (frames > 1)
		{
			platform_log("GE: waited for the GE %lu us a frame (worst %lu) over the last 300 frames; frame end: cache write-back %lu us, buffer swap %lu, textures %lu; draws skipped with the display list full %lu in all; see-through parts placed by the CPU %lu; buffers on several nodes drawn rigid %lu; see-through flushes under the first-person projection %lu; HUD quads that left early with the depth test off %lu",
				ge_wait_us / 300, ge_wait_worst, frame_end_us[0] / 300, frame_end_us[1] / 300, frame_end_us[2] / 300,
				list_full_skips, transparent_placed, multi_node_rigid, first_person_flushes, screen_quad_bailed);
			platform_log("GE: buffers on several nodes drawn rigid, by why: see-through %lu, no triangles given %lu, the skinning plan failed %lu",
				multi_node_rigid_why[0], multi_node_rigid_why[1], multi_node_rigid_why[2]);
			ge_kinds_report(); /* build 103 */
			CENSUS(census_report());
			replay_kinds_report(300); /* mekinds */
			ge_reads_report(frames); /* build 115 */
			vm_report(); /* build 116 */
			/* build 112: every 300 frames, with the skinning plans' heap */
			platform_log("GE: converted-buffer table: %lu sweeps, %lu dead entries forgotten, %d in use of %d (next sweep at %d); skinning plans %lu KB on the heap (peak %lu KB)",
				converted_sweeps, converted_forgotten, converted_used, CONVERTED_BUFFERS,
				converted_sweep_late > 0 ? CONVERTED_BUFFERS * 3 / 4 : converted_sweep_mark,
				skin_live_bytes / 1024, skin_live_peak / 1024);
			converted_sweeps = converted_forgotten = 0;
			/* build 112: gedefer's 300 frames */
			if (defer_on)
				platform_log("GE: gedefer: %lu calls deferred, carried out in %lu runs (%lu when the buffer filled); ran as they came: %lu models with more than %d nodes, %lu calls too large",
					defer_records, defer_drains, defer_drains_full, defer_big_models, DEFER_MAX_NODES, defer_too_large);
			if (inflight_on && inflight_frames)
			{
				platform_log("GE: geinflight: %lu frames carried out after the next frame's ticks (%lu of them on the Media Engine, %lu stall addresses passed on while out), %lu KB a frame written down (fullest %lu KB of %u)",
					inflight_frames, inflight_handed, inflight_forwards, inflight_words * sizeof(defer_word) / 1024 / inflight_frames,
					inflight_words_most * (unsigned long)sizeof(defer_word) / 1024, (unsigned int)(inflight_buffer_words * sizeof(defer_word) / 1024));
				platform_log("GE: geinflight: whole frames %lu KB on average, %lu KB at most, the parts carried out when the buffer filled included (inflightkbN sizes the buffer); strings drawn though their frame's start was still written down %lu; records dropped for a sleep %lu times (since the start)",
					inflight_whole_words * sizeof(defer_word) / 1024 / inflight_frames, inflight_whole_most * (unsigned long)sizeof(defer_word) / 1024,
					inflight_text_early, inflight_room_drops);
				inflight_frames = inflight_words = inflight_words_most = inflight_handed = inflight_forwards = 0;
				inflight_whole_words = inflight_whole_most = inflight_text_early = 0;
			}
			if (defer_on)
			{
				char sites[400];
				int length = 0, k;

				sites[0] = 0;
				for (k = 0; k < DEFER_SITES && defer_site[k]; k++)
				{
					length += snprintf(sites + length, sizeof(sites) - length, " %s %lu;", defer_site[k], defer_site_drains[k]);
					defer_site_drains[k] = 0;
					if (length >= (int)sizeof(sites) - 1)
						break;
				}
				platform_log("GE: gedefer: carried out for:%s", sites);
				if (defer_me_jobs || defer_me_failed)
					platform_log("GE: gemeon: %lu runs on the Media Engine (%lu failed): the main CPU waited %lu us a run, the ME's own time %lu us",
						defer_me_jobs, defer_me_failed, defer_me_jobs ? defer_me_wait_us / defer_me_jobs : 0,
						defer_me_jobs ? defer_me_own_us / defer_me_jobs : 0);
				defer_me_jobs = defer_me_failed = defer_me_wait_us = defer_me_own_us = 0;
				if (chunk_on > 0)
				{
					platform_log("GE: mechunk: %lu frames in pieces, %lu gated off (the last wait %u us); %lu pieces (%lu run here, simulated), "
						"%lu us a piece to publish; stopped early for a texture %lu, for a call that ran at once %lu; the ME's looks for more %u",
						chunk_frames, chunk_gated, chunk_wait_last_us, chunk_publishes, chunk_simulated,
						chunk_publishes ? chunk_publish_us / chunk_publishes : 0, chunk_stops_texture, chunk_stops_drain,
						CHUNK_LINE[3]);
					chunk_frames = chunk_gated = chunk_publishes = chunk_simulated = chunk_publish_us = 0;
					chunk_stops_texture = chunk_stops_drain = 0;
				}
				/* build 118: gemeoverlap. waited: the main CPU's time in
				psp_me_finish a half (simulated: the half's own time) */
				if (me_overlap_runs || me_overlap_alone)
					platform_log("GE: gemeoverlap: %lu halves carried out by the Media Engine while the main CPU wrote the next, the main CPU waited %lu us a half; %lu carried out on the main CPU (the ME couldn't take them)",
						me_overlap_runs, me_overlap_waits ? me_overlap_wait_us / me_overlap_waits : 0, me_overlap_alone);
				me_overlap_runs = me_overlap_waits = me_overlap_wait_us = me_overlap_alone = 0;
				if (walls_wanted_total || walls_wanted_dropped)
					platform_log("GE: gemesim: %lu see-through level triangles wanted asked by the Media Engine's work and asked at the frame's end, %lu more than the list held",
						walls_wanted_total, walls_wanted_dropped);
				if (level_pretouches)
					platform_log("GE: levelpretouch: %lu level copies built as their draws were written down, %lu without room",
						level_pretouches, level_pretouch_missed);
				if (model_pretouches)
					platform_log("GE: modelpretouch: %lu model buffers converted as their draws were written down, %lu without room; %lu model or level draws left to the frame's end (a piece of the frame on the ME)",
						model_pretouches, model_pretouch_missed, pretouch_chunk_skips);
				if (converts_wanted_total || converts_wanted_dropped)
					platform_log("GE: gemesim: %lu model buffers wanted converted by the Media Engine's work and converted at the frame's end, %lu more than the list held",
						converts_wanted_total, converts_wanted_dropped);
				if (streams_wanted_total || streams_wanted_dropped)
					platform_log("GE: gemesim: %lu level copies wanted by the Media Engine's work and built at the frame's end, %lu more than the list held",
						streams_wanted_total, streams_wanted_dropped);
			}
			defer_records = defer_drains = defer_drains_full = defer_big_models = defer_too_large = 0;
			if (ge_skip || list_in_vram || vram_busy || ge_skipped_draws)
			{
				platform_log("GE: bench test: GE skipping %s; draws skipped %lu; display list in %s; VRAM-only load %s (%d sprites, %s)",
					ge_skip & GE_SKIP_ALL ? "every draw" : ge_skip == GE_SKIP_LEVEL ? "the level" : ge_skip == GE_SKIP_MODELS ? "models" : "nothing",
					ge_skipped_draws, list_in_vram ? "VRAM" : "RAM", vram_busy ? "on" : "off", vram_busy_sprites,
					vram_busy_calibrating ? "sizing, GE alone" : "held, overlapped");
				ge_skipped_draws = 0;
			}
			budget_wait_us = ge_wait_us / 300;
			ge_wait_us = ge_wait_worst = 0;
			frame_end_us[0] = frame_end_us[1] = frame_end_us[2] = 0;
			{
				/* build 37: the renderer's own CPU time a frame, by kind (the
				rest of each profile step is the game's) */
				extern unsigned long psp_vfpu_multiplies;
				char text[512];
				int length = 0, k;
				unsigned long total = 0;

				for (k = 0; k < GE_ENTRIES; k++)
				{
					length += snprintf(text + length, sizeof(text) - length, "%s%s %lu", k ? ", " : "",
						ge_entry_names[k], ge_entry_us[k] / 300);
					total += ge_entry_us[k];
					ge_entry_us[k] = 0;
				}
				entry_snapshot = 0;
				platform_log("GE: renderer CPU us a frame: %s (all %lu); VFPU matrix products %lu a frame", text,
					total / 300, psp_vfpu_multiplies / 300);
				budget_renderer_us = total / 300;
				/* build 64: the angle buckets, the overlays' walls, nearauto
				and the trial splits, over the last 300 frames */
				{
					unsigned long all = 0;
					int b;

					for (b = 0; b < ANGLE_BUCKETS; b++)
						all += angle_triangles[b];
					platform_log("GE: angle buckets: %lu draws, %lu us a frame sorting (the bucket loop alone %lu); triangles by 1/cos (1, 1.4, 2, 2.8, 4, 5.7, 8): %lu%% %lu%% %lu%% %lu%% %lu%% %lu%% %lu%%; overlays: %lu asked of the collision (%lu on a wall, %lu free, %lu us in all; %d draws remembered, %lu replaced); slope nudges %lu, held to the lift limit (%d%% of the distance) %lu, dropped edge-on %lu%s",
						angle_draws, angle_us / 300, angle_loop_us / 300, all ? angle_triangles[0] * 100 / all : 0, all ? angle_triangles[1] * 100 / all : 0,
						all ? angle_triangles[2] * 100 / all : 0, all ? angle_triangles[3] * 100 / all : 0,
						all ? angle_triangles[4] * 100 / all : 0, all ? angle_triangles[5] * 100 / all : 0,
						all ? angle_triangles[6] * 100 / all : 0, overlay_wall_asks, overlay_on_walls, overlay_free, overlay_ask_us, overlay_backing_count, overlay_backing_full, nudges_asked, nudge_percent, nudges_clamped, nudges_edge_on,
						nudge_wide > 0 ? " (nudgewide: not held)" : "");
					nudges_asked = nudges_clamped = nudges_edge_on = 0;
					if (overlay_no_room || overlay_no_room_skipped)
						platform_log("GE: overlays without heap for their answers: %lu asks refused, %lu more not asked (overlaymemo%s), each drawn as on a wall",
							overlay_no_room, overlay_no_room_skipped, overlay_memo > 0 ? "" : "off");
					overlay_no_room = overlay_no_room_skipped = 0;
					if (me_slant > 0)
						platform_log("GE: meslant: %lu of %lu sorted draws split with the Media Engine, %lu triangles of %lu (%lu%%) bucketed there; %lu fell back to this CPU; meslantcheck: %lu triangles checked, %lu answers differing",
							me_slant_split, me_slant_draws, me_slant_triangles, all, all ? me_slant_triangles * 100 / all : 0,
							me_slant_fallbacks, me_slant_checked, me_slant_mismatches);
					me_slant_draws = me_slant_split = me_slant_triangles = me_slant_fallbacks = me_slant_checked = 0;
					me_slant_mismatches = 0;
					platform_log("GE: near plane%s: %lu frames at x4 or more (x8 %lu, x16 %lu), %lu at x2, %lu at x1 (%lu with a model near enough to cap it), %lu us a frame deciding; level draws a frame: %lu wholly inside the view (no classifying), %lu classified; triangles past the detail fade %lu a frame, detail passes skipped for whole draws %lu a frame",
						near_auto ? " (nearauto)" : " (fixed)", near_auto_chosen[2], near_auto_chosen_high[0], near_auto_chosen_high[1], near_auto_chosen[1], near_auto_chosen[0],
						near_auto_by_objects, near_auto_frames ? near_auto_us / near_auto_frames : 0, level_draws_inside / 300,
						level_draws_classified / 300, angle_far_triangles / 300, detail_passes_skipped_far / 300);
					if (near_cine_frames || near_cine_first_person)
						platform_log("GE: nearcine: %lu frames at a quarter plane (a cutscene's close-up), %lu left at the game's plane (the player's own eye)",
							near_cine_frames, near_cine_first_person);
					near_cine_frames = near_cine_first_person = 0;
					if (near_auto_check > 0)
						platform_log("GE: nearautocheck: %lu sphere answers compared, %lu differed", near_auto_checked, near_auto_differed);
					platform_log("GE: uv centring %s: %lu draws, largest shift %.0f repeats; GE serial %s",
						uv_centre > 0 ? "on" : "off", uv_centred_draws / 300, uv_shift_largest,
						ge_serial > 0 ? "on (the frame end's wait is the GE's whole busy time)" : "off");
					/* build 106: the near bending corners' VFPU check */
					if (skin_vfpu_check)
						platform_log("GE: bending corners near the eye on the %s; skinvfpucheck: %lu compared with the FPU, %lu verdicts differed",
							skin_vfpu > 0 ? "VFPU" : "FPU", skin_vfpu_corners, skin_vfpu_differ);
					/* build 107: the rigid near corners' one-product transform, checked */
					if (near_check > 0)
						platform_log("GE: nearcheck: %lu rigid corners near the eye compared with the two-step transform, %lu verdicts differed",
							near_check_corners, near_check_differ);
					/* build 119's probe */
					if (probes_on)
						platform_log("GE: level passes by piece, us a frame: uv centring %lu, base pass %lu, lightmap pass %lu, detail passes %lu (%lu draws a frame; the slant sort above)",
						level_uv_us / 300, level_base_us / 300, level_lightmap_us / 300, level_detail_us / 300, level_probe_draws / 300);
					level_uv_us = level_base_us = level_lightmap_us = level_detail_us = level_probe_draws = 0;
					uv_centred_draws = 0;
					uv_shift_largest = 0.0f;
					memset(angle_triangles, 0, sizeof(angle_triangles));
					angle_draws = angle_us = 0;
					angle_loop_us = 0;
					memset(near_auto_chosen, 0, sizeof(near_auto_chosen));
					memset(near_auto_chosen_high, 0, sizeof(near_auto_chosen_high));
					near_auto_by_objects = near_auto_us = near_auto_frames = 0;
					level_draws_inside = level_draws_classified = angle_far_triangles = detail_passes_skipped_far = 0;
				}
				psp_vfpu_multiplies = 0;
				{
					/* build 47: the parts, us a frame and calls a frame, and
					what one timer reading costs */
					static unsigned int timer_cost_ns = 0;
					char parts[900];
					int written = 0;

					if (!timer_cost_ns)
					{
						unsigned int t0 = sceKernelGetSystemTimeLow(), n;

						for (n = 0; n < 1000; n++)
							(void)sceKernelGetSystemTimeLow();
						timer_cost_ns = sceKernelGetSystemTimeLow() - t0 + 1;
					}
					/* two lines: a log line holds 512 characters */
					for (k = 0; k < GE_PARTS && written < (int)sizeof(parts) - 80; k++)
					{
						if (k == 4)
						{
							platform_log("GE: parts, us a frame (calls a frame): %s; one timer reading %u ns", parts, timer_cost_ns);
							written = 0;
							parts[0] = 0;
						}
						written += snprintf(parts + written, sizeof(parts) - written, "%s%s %lu (%lu)", k != 4 && k ? ", " : "",
							ge_part_names[k], ge_part_us[k] / 300, ge_part_calls[k] / 300);
						ge_part_us[k] = ge_part_calls[k] = 0;
					}
					platform_log("GE: parts, us a frame (calls a frame): %s", parts);
					/* build 48: the list's cached regions and the stall updates */
					platform_log("GE: list: %lu regions a frame written cached (%lu KB a frame, %lu lines written back); command words %s (%lu lines a frame written back); stall updates %lu a frame, %lu passed on (%s), %.1f us each (%lu timed); decal quads under 2 pixels left out %lu, wholly off the screen %lu (since the start); decal calls with nothing on the screen %lu a frame",
						cached_takes / 300, cached_bytes / 300 / 1024, cached_lines_flushed / 300,
						list_cached_frame ? "cached (listcachedall)" : "uncached", list_lines_back / 300, stall_calls / 300,
						stall_forwarded / 300, stall_batch == 1 ? "batched" : "each", stall_timed ? (double)stall_timed_us / stall_timed : 0.0, stall_timed, decals_small, decals_offscreen,
						decals_wholly_left_out / 300);
					decals_wholly_left_out = 0;
					/* build 119's probe */
					if (probes_on)
						platform_log("GE: decal calls by piece, us a frame: setup %lu, the off-screen loop %lu, texture and list room %lu, corners %lu, state and draw %lu",
						decal_probe_us[0] / 300, decal_probe_us[1] / 300, decal_probe_us[2] / 300, decal_probe_us[3] / 300, decal_probe_us[4] / 300);
					memset(decal_probe_us, 0, sizeof(decal_probe_us));
					cached_takes = cached_bytes = cached_lines_flushed = stall_calls = stall_forwarded = 0;
					list_lines_back = 0;
					if (lit_memo > 0 || psp_keep("memotex"))
					{
						/* build 114: statememo (or one half: memotex, memolight) */
						extern unsigned long psp_tex_binds_memo, psp_tex_binds_sent;

						platform_log("GE: state memo: texture binds %lu a frame sent, %lu left out; model lighting (ambient and lights) %lu a frame set, %lu left out",
							psp_tex_binds_sent / 300, psp_tex_binds_memo / 300, lit_sets_sent / 300, lit_sets_memo / 300);
						psp_tex_binds_sent = psp_tex_binds_memo = lit_sets_sent = lit_sets_memo = 0;
					}
				}
			}
		}
	}
}

int psp_ge_active(void)
{
	return ge_ready;
}

/* FIXES_GEFEED: whether the engine leaves the Xbox vertex shaders'
constants unmade. The null renderer never makes a GL device, so nothing
reads them; "vshconstants" makes and stores them as before. Asked before
keep.txt is read, the answer is "leave them" and isn't kept */
int psp_vsh_constants_skip(void)
{
	static int skip = -1;
	extern int psp_keep_loaded(void);

	if (skip < 0)
	{
		if (!psp_keep_loaded())
			return 1;
		skip = !psp_keep("vshconstants");
	}
	return skip;
}

/* ---------- build 87: the Media Engine's first render job, measure only
("meclassify" in the launch menu; off by default). Build 105: only in a
build made with PSP_DEBUG_TOOLS=1 (psp_build.py); elsewhere it does nothing.

The level draw's first CPU step is classifying each vertex against the
near plane and the guard band (to_camera + outside, ~1-3 ms a frame on the
PSP in b47). It is pure single-precision arithmetic on 12 bytes a vertex:
the shape of job the ME suits (no VFPU, no library calls, little memory
traffic per unit of work). Here the ME classifies the same vertices again,
after the main CPU has, and the two answers are compared: what it measures
is the ME's own time a vertex against the main CPU's, and whether the ME's
FPU gives the same bits. Nothing it computes is used yet, and the main CPU
waits for it, so it costs frame time while on: a measurement, not a speedup.
The step after this (if the numbers say so) hands the classification to the
ME for the whole visible set at the start of the level's drawing */
#if PSP_DEBUG_TOOLS
static unsigned char me_codes[65536 + 64] __attribute__((aligned(64)));
static float me_view[16] __attribute__((aligned(64)));
#endif
static int me_classify_on = -1;
static unsigned long me_classify_jobs, me_classify_vertices, me_classify_mismatches, me_classify_failures,
	me_classify_wait_us;


/* runs on the ME (or the main CPU when simulated): no system calls, no
library calls, no VFPU. args: the stream's vertices, first and last index,
the output, the view block (eye, right, up, forward, guard x and y, near) */
unsigned int psp_ge_me_classify(const unsigned int *args, unsigned int *results)
{
	const struct ge_vertex *base = (const struct ge_vertex *)args[0];
	unsigned int first = args[1], last = args[2], v;
	unsigned char *out = (unsigned char *)args[3];
	const float *view = (const float *)args[4];
	float ex = view[0], ey = view[1], ez = view[2];
	float rx = view[3], ry = view[4], rz = view[5], ux = view[6], uy = view[7], uz = view[8];
	float fx = view[9], fy = view[10], fz = view[11], gx = view[12], gy = view[13], near = view[14];

	for (v = first; v <= last; v++)
	{
		const struct ge_vertex *b = &base[v];
		float d0 = b->x - ex, d1 = b->y - ey, d2 = b->z - ez;
		float cx = d0 * rx + d1 * ry + d2 * rz, cy = d0 * ux + d1 * uy + d2 * uz, cz = d0 * fx + d1 * fy + d2 * fz;
		unsigned char bits = 0;

		if (cz < near) bits |= 1;
		if (cx > gx * cz) bits |= 2;
		if (cx < -gx * cz) bits |= 4;
		if (cy > gy * cz) bits |= 8;
		if (cy < -gy * cz) bits |= 16;
		out[v] = bits;
	}
	results[0] = last - first + 1;
	return last - first + 1;
}

/* runs on the ME (or the main CPU when simulated): args: the stream's
vertices, the triangles' indices, the count, the output (a bucket a
triangle), the view block (eye, detail fade squared), far possible */
unsigned int psp_ge_me_angle(const unsigned int *args, unsigned int *results)
{
	const struct ge_vertex *stream = (const struct ge_vertex *)args[0];
	const unsigned short *indices = (const unsigned short *)args[1];
	unsigned int count = args[2], t;
	unsigned char *out = (unsigned char *)args[3];
	const float *view = (const float *)args[4];
	int far_possible = (int)args[5];
	float ex = view[0], ey = view[1], ez = view[2], far_squared = view[3];

	for (t = 0; t < count; t++)
		out[t] = (unsigned char)angle_bucket_one(stream, indices + 3 * t, ex, ey, ez, far_possible, far_squared);
	results[0] = count;
	return count;
}

/* "meslant": the ring job for the back part of a draw's triangles
(angle_sort). Runs on the ME (or the main CPU when simulated): no system
calls, no library calls, no VFPU. args: [0] the stream's vertices, [1] the
part's indices, [2] its triangle count, [3] the output (a bucket a triangle),
[4] far possible, [5]-[7] the eye and [8] the detail fade squared (float
bits). results: the part's count in each bucket, two to a word (a draw has
at most ANGLE_TRIANGLES) */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
unsigned int psp_ge_me_slant(const unsigned int *args, unsigned int *results)
{
	const struct ge_vertex *stream = (const struct ge_vertex *)args[0];
	const unsigned short *indices = (const unsigned short *)args[1];
	unsigned int count = args[2], t, b, total[SORT_BUCKETS];
	unsigned char *out = (unsigned char *)args[3];
	int far_possible = (int)args[4];
	union { unsigned int u; float f; } ex, ey, ez, far_squared;

	ex.u = args[5];
	ey.u = args[6];
	ez.u = args[7];
	far_squared.u = args[8];
	for (b = 0; b < SORT_BUCKETS; b++)
		total[b] = 0;
	for (t = 0; t < count; t++)
	{
		int bucket;

		/* audio first: a block waiting is mixed every 64 triangles, ~64 us
		of the ME's time (psp_me.c) */
		if ((t & 63) == 63)
			psp_me_ring_step(t);
		bucket = angle_bucket_one(stream, indices + 3 * t, ex.f, ey.f, ez.f, far_possible, far_squared.f);
		out[t] = (unsigned char)bucket;
		total[bucket]++;
	}
	for (b = 0; b < SORT_BUCKETS; b += 2)
		results[b / 2] = total[b] | total[b + 1] << 16;
	return count;
}

/* the main CPU's side of the slant measure: the same triangles on the ME,
its buckets against the main CPU's (angle_bucket_of), and the same function
again on the main CPU with the triangles' vertices out of its cache first */
#if PSP_DEBUG_TOOLS
static void me_angle_measure(const struct ge_vertex *stream, const unsigned short *indices, long count,
	int far_possible)
{
	extern int psp_me_running(void);
	extern int psp_me_run(int which, unsigned int type, const unsigned int *args, int arg_count, unsigned int *results,
		unsigned int first_look_us);
	extern unsigned int psp_me_last_job_us(int which);
	unsigned int args[6], results[8], start, lowest = 0xFFFF, highest = 0;
	long t;

	if (me_classify_on < 0)
		me_classify_on = psp_keep("meclassify");
	if (!me_classify_on || !psp_me_running() || count <= 0 || count > ANGLE_TRIANGLES)
		return;
	me_angle_view[0] = level_eye[0];
	me_angle_view[1] = level_eye[1];
	me_angle_view[2] = level_eye[2];
	me_angle_view[3] = detail_far_squared;
	/* everything the ME reads (the vertices, a list written this frame, the
	view) to RAM; nothing of the output left in this cache */
	sceKernelDcacheWritebackAll();
	sceKernelDcacheInvalidateRange(me_buckets, (unsigned int)((count + 63) & ~63));
	args[0] = (unsigned int)stream;
	args[1] = (unsigned int)indices;
	args[2] = (unsigned int)count;
	args[3] = (unsigned int)me_buckets;
	args[4] = (unsigned int)me_angle_view;
	args[5] = (unsigned int)far_possible;
	start = sceKernelGetSystemTimeLow();
	if (!psp_me_run(1 /* the render slot */, 3 /* ME_JOB_ANGLE */, args, 6, results, 50))
	{
		me_angle_failures++;
		return;
	}
	me_angle_wait_us += sceKernelGetSystemTimeLow() - start;
	me_angle_me_us += psp_me_last_job_us(1);
	sceKernelDcacheInvalidateRange(me_buckets, (unsigned int)((count + 63) & ~63));
	me_angle_jobs++;
	me_angle_triangles += (unsigned long)count;
	for (t = 0; t < count; t++)
		if (me_buckets[t] != angle_bucket_of[t])
			me_angle_mismatches++;
	/* the main CPU, the same function, the vertices out of its cache first */
	for (t = 0; t < count * 3; t++)
	{
		if (indices[t] < lowest)
			lowest = indices[t];
		if (indices[t] > highest)
			highest = indices[t];
	}
	sceKernelDcacheWritebackInvalidateRange((void *)((unsigned int)&stream[lowest] & ~63u),
		(unsigned int)((highest - lowest + 1) * sizeof(struct ge_vertex) + 64));
	start = sceKernelGetSystemTimeLow();
	for (t = 0; t < count; t++)
		me_buckets[t] = (unsigned char)angle_bucket_one(stream, indices + 3 * t, me_angle_view[0], me_angle_view[1],
			me_angle_view[2], far_possible, me_angle_view[3]);
	me_angle_sc_us += sceKernelGetSystemTimeLow() - start;
}

/* the main CPU's side: after a draw's own classification, the same range on
the ME; the answers compared where the main CPU worked one out */
static void me_classify_measure(const struct level_stream *stream, unsigned int lowest, unsigned int highest)
{
	extern int psp_me_running(void);
	extern int psp_me_run(int which, unsigned int type, const unsigned int *args, int arg_count, unsigned int *results,
		unsigned int first_look_us);
	extern unsigned int psp_me_last_job_us(int which);
	unsigned int args[5], results[8], v, start;
	unsigned int line_first = lowest & ~63u, line_bytes = ((highest + 64) & ~63u) - line_first;

	if (me_classify_on < 0)
		me_classify_on = psp_keep("meclassify");
	if (!me_classify_on || !psp_me_running() || highest < lowest)
		return;
	me_view[0] = level_eye[0]; me_view[1] = level_eye[1]; me_view[2] = level_eye[2];
	me_view[3] = camera_right[0]; me_view[4] = camera_right[1]; me_view[5] = camera_right[2];
	me_view[6] = camera_up[0]; me_view[7] = camera_up[1]; me_view[8] = camera_up[2];
	me_view[9] = camera_forward[0]; me_view[10] = camera_forward[1]; me_view[11] = camera_forward[2];
	me_view[12] = guard_x; me_view[13] = guard_y; me_view[14] = NEAR_CLIP;
	/* what the ME reads, to RAM (the view, and the vertices: a stream made
	this frame may not be there yet); no copy of what it writes left here */
	sceKernelDcacheWritebackAll();
	sceKernelDcacheWritebackInvalidateRange(me_codes + line_first, line_bytes);
	args[0] = (unsigned int)stream->base;
	args[1] = lowest;
	args[2] = highest;
	args[3] = (unsigned int)me_codes;
	args[4] = (unsigned int)me_view;
	start = sceKernelGetSystemTimeLow();
	if (!psp_me_run(1 /* the render slot */, 2 /* ME_JOB_CLASSIFY */, args, 5, results, 50))
	{
		me_classify_failures++;
		return;
	}
	me_classify_wait_us += sceKernelGetSystemTimeLow() - start;
	me_classify_me_us += psp_me_last_job_us(1);
	sceKernelDcacheInvalidateRange(me_codes + line_first, line_bytes);
	me_classify_jobs++;
	me_classify_vertices += highest - lowest + 1;
	for (v = lowest; v <= highest; v++)
		if (!(vertex_codes[v] & CODE_UNKNOWN) && (vertex_codes[v] & CODE_PLANES) != me_codes[v])
			me_classify_mismatches++;
	/* build 87: the same function on the main CPU, the range out of its cache
	first (as the ME starts), into vertex_codes (done with for this draw: every
	user clears its range first) */
	sceKernelDcacheWritebackInvalidateRange((void *)((unsigned int)&stream->base[lowest] & ~63u),
		(highest - lowest + 1) * sizeof(struct ge_vertex) + 64);
	args[3] = (unsigned int)vertex_codes;
	start = sceKernelGetSystemTimeLow();
	psp_ge_me_classify(args, results);
	me_classify_sc_us += sceKernelGetSystemTimeLow() - start;
	for (v = lowest; v <= highest; v++)
		if (vertex_codes[v] != me_codes[v])
			me_classify_sc_mismatches++;
}
#else /* PSP_DEBUG_TOOLS: the instrument is left out (psp_build.py) */
static void me_angle_measure(const struct ge_vertex *stream, const unsigned short *indices, long count,
	int far_possible)
{
	(void)stream; (void)indices; (void)count; (void)far_possible;
}

static void me_classify_measure(const struct level_stream *stream, unsigned int lowest, unsigned int highest)
{
	(void)stream; (void)lowest; (void)highest;
}
#endif

/* the exit (exitatN, ladderN, the level's end, HOME): it comes mid-frame,
from the tick. Under geinflight a frame may be out with the Media Engine
and the GE's list left at a stall address the forwarding thread passes on;
psp_me_exit then stops the ME and the firmware's exit starts with the GE
still waiting in a list nobody will finish. So the frame out is waited for
as every frame's is, the open list is ended, and the GE is given up to
200 ms to reach its end. "exitdrainoff" exits as before */
void psp_ge_exit_drain(void)
{
	unsigned int waited = 0;

	if (!ge_ready || psp_keep("exitdrainoff") || inflight_room_away)
		return;
	if (inflight_pending)
		inflight_sync();
	if (frame_open)
	{
		stall_force = 1;
		sceGuFinish();
		stall_force = 0;
	}
	while (sceGeDrawSync(1) != 0 && waited < 200)
	{
		sceKernelDelayThread(1000);
		waited++;
	}
	platform_log("GE: exit: the frame out waited for, the list ended, the GE %s (%u ms)",
		waited < 200 ? "done" : "still busy", waited);
}
