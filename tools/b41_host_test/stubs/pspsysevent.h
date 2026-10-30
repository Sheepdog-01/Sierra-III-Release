#ifndef HOST_PSPSYSEVENT_H
#define HOST_PSPSYSEVENT_H
/* the PSPSDK's layout (pspsysevent.h, PSPSDK, BSD licence): the declaration only */
typedef struct PspSysEventHandler
{
	int size;
	char *name;
	int type_mask;
	int (*handler)(int id, char *name, void *param, int *result);
	int r28;
	int busy;
	struct PspSysEventHandler *next;
	int reserved[9];
} PspSysEventHandler;
int sceKernelRegisterSysEventHandler(PspSysEventHandler *handler);
int sceKernelUnregisterSysEventHandler(PspSysEventHandler *handler);
#endif
