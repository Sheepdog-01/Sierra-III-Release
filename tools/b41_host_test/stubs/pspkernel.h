/* host-test stand-in for the PSPSDK header: only what the B41 sources use */
#ifndef HOST_PSPKERNEL_H
#define HOST_PSPKERNEL_H
typedef int SceUID;
typedef unsigned int SceSize;
typedef unsigned int SceUInt;
#define PSP_MODULE_KERNEL 0x1000
#define PSP_MODULE_INFO(name, attributes, major, minor)
#define PSP_MAIN_THREAD_ATTR(attributes)
SceUID sceKernelLoadModule(const char *path, int flags, void *option);
int sceKernelStartModule(SceUID module, SceSize args, void *argp, int *status, void *option);
int sceKernelDelayThread(SceUInt microseconds);
unsigned int sceKernelGetSystemTimeLow(void);
long long sceKernelGetSystemTimeWide(void);
#endif
