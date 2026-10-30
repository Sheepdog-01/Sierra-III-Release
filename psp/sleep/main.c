/*
HALO_SLEEP.PRX

B41's fix: a small kernel-mode module the game loads at start-up
(psp_sleepguard.c). The power switch's sleep is put off until the game has
quieted itself, instead of held off the whole time the game runs.

B41: the game readied itself for a sleep in its power callback, which runs
on a user thread the firmware doesn't wait for. So the kernel's sleep could
go on while the GE was still drawing from the volatile partition the
texture cache had just handed back, the Media Engine was mid-job and the
game thread was mid-frame, and the crash that followed landed while the
firmware was writing its own settings.

Here the firmware's suspend query (sysevent 0x100), which every driver
answers before anything sleeps, is refused while the game isn't ready, and
the refusal is noted in the game's block. The game sees it at a frame end
with its list finished: the GE drained, the ME stopped, the volatile
partition given back, and only then does the game mark itself ready and
ask for the sleep itself (scePowerRequestSuspend). That query is answered
yes.

Nothing else is touched: the module reads and writes only the game's block
(user memory, its address given by the game), and every other event passes
through unanswered. It fails closed: any start without a valid block, or a
handler the firmware won't take, answers non-zero, and the game keeps the
switch held off instead (psp_sleepguard.c).
*/

#include <pspkernel.h>
#include <pspsysevent.h>

PSP_MODULE_INFO("halo_sleep", PSP_MODULE_KERNEL, 1, 0);
PSP_MAIN_THREAD_ATTR(0);

/* the game's half (psp_sleepguard.c): the same layout and magic */
struct halo_sleep_block
{
	unsigned int magic;
	volatile unsigned int wanted;  /* set here: a sleep was asked for and refused */
	volatile unsigned int ready;   /* set by the game: quiet, the next query is let through */
	volatile unsigned int queries, refused, allowed;
	volatile unsigned int last_event;
	volatile unsigned int query_words[4]; /* the last query's first words, for the game's log */
};
#define HALO_SLEEP_MAGIC 0x48534c32 /* "HSL2" */

#define SUSPEND_QUERY 0x100

static struct halo_sleep_block *block = 0;

static int readable(const void *pointer)
{
	unsigned int address = (unsigned int)pointer & 0x1FFFFFFF;

	return pointer && address >= 0x08000000 && address < 0x0A000000 - 16;
}

static int sleep_event(int id, char *name, void *param, int *result)
{
	(void)name; (void)result;
	if (!block)
		return 0;
	block->last_event = (unsigned int)id;
	if (id != SUSPEND_QUERY)
		return 0;
	block->queries++;
	/* what the query carries (a sleep or a power-off), for the hardware
	test's log: not acted on until it is known */
	if (readable(param))
	{
		const unsigned int *words = param;

		block->query_words[0] = words[0];
		block->query_words[1] = words[1];
		block->query_words[2] = words[2];
		block->query_words[3] = words[3];
	}
	if (block->ready)
	{
		block->allowed++;
		return 0;
	}
	block->wanted = 1;
	block->refused++;
	return -1;
}

static PspSysEventHandler handler =
{
	sizeof(PspSysEventHandler), "halo_sleep", 0x00ffff00, sleep_event, 0, 0, 0, { 0 }
};

static int registered = 0;

int module_start(SceSize args, void *argp)
{
	unsigned int address;

	if (args < 4 || !argp)
		return -1;
	address = *(unsigned int *)argp;
	if (address < 0x08800000 || address >= 0x0A000000 - sizeof(struct halo_sleep_block))
		return -1;
	if (((struct halo_sleep_block *)address)->magic != HALO_SLEEP_MAGIC)
		return -1;
	block = (struct halo_sleep_block *)address;
	registered = sceKernelRegisterSysEventHandler(&handler) >= 0;
	if (!registered)
	{
		block = 0;
		return -1;
	}
	/* 0: the handler is in, and the game may let the switch work */
	return 0;
}

int module_stop(SceSize args, void *argp)
{
	(void)args; (void)argp;
	if (registered)
		sceKernelUnregisterSysEventHandler(&handler);
	registered = 0;
	block = 0;
	return 0;
}
