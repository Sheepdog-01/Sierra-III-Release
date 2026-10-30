#ifndef HOST_PSPPOWER_H
#define HOST_PSPPOWER_H
#define PSP_POWER_CB_SUSPENDING 0x00010000
#define PSP_POWER_CB_RESUMING 0x00020000
#define PSP_POWER_CB_RESUME_COMPLETE 0x00040000
int scePowerLock(int unknown);
int scePowerUnlock(int unknown);
int scePowerRequestSuspend(void);
#endif
